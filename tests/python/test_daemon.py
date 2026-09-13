#!/usr/bin/env python3
"""test_daemon.py — integração end-to-end do apolovmd.

Monta um servidor RFB (VNC) falso em Python, sobe o `apolovmd` Meson contra
ele e valida, por um cliente WebSocket RFC 6455 também escrito em Python:

  * handshake HTTP/WS (health JSON, sem autenticação);
  * canal /ws/display recebe HELLO + FRAME (payload BGRA idêntico ao fake);
  * canal /ws/input traduz AGP KEY/MOUSE em eventos RFB reais;
  * scroll (wheel) vira botões 4/5 (press+release);
  * clipboard do servidor (ServerCutText) chega no /ws/clipboard.

Roda só com a stdlib. Uso: python3 tests/python/test_daemon.py
"""
import base64
import json
import os
import socket
import struct
import subprocess
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BIN = os.environ.get("APOLOVMD", os.path.join(ROOT, "apolovm-main", "build", "apolovmd"))

AGP_MAGIC = b"APGV01\x00"
T_HELLO, T_FRAME, T_FRAME_DELTA = 0x01, 0x02, 0x03
T_KEY, T_MOUSE, T_RESIZE, T_CLIPBOARD = 0x05, 0x06, 0x07, 0x08


def agp_encode(typ, width=0, height=0, payload=b"", flags=0):
    h = AGP_MAGIC + bytes([typ, flags]) + struct.pack(">III", width, height, len(payload))
    return h + payload


def agp_decode(buf):
    if len(buf) < 21 or AGP_MAGIC != buf[:7]:
        return None
    typ, fl = buf[7], buf[8]
    w, h, ln = struct.unpack(">III", buf[9:21])
    return dict(type=typ, flags=fl, width=w, height=h, payload=buf[21 : 21 + ln])


# ------------------------------------------------------------------ WS client
class Ws:
    def __init__(self, host, port, path):
        self.s = socket.create_connection((host, port), timeout=8)
        self.s.settimeout(8)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.s.sendall(req.encode())
        resp = self._read_line()  # status
        assert b"101" in resp, resp
        while True:
            line = self._read_line()
            if line in (b"", b"\r\n"):
                break

    def _read_line(self):
        data = b""
        while not data.endswith(b"\r\n"):
            c = self.s.recv(1)
            if not c:
                raise EOFError("conexão WS fechada")
            data += c
            if len(data) > 8192:
                raise EOFError("linha WS grande demais")
        return data

    def recv_msg(self, timeout=6):
        self.s.settimeout(timeout)
        hdr = self.s.recv(2)
        if len(hdr) < 2:
            raise EOFError
        op = hdr[0] & 0x0F
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self.s.recv(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self.s.recv(8))[0]
        payload = b""
        while len(payload) < ln:
            chunk = self.s.recv(ln - len(payload))
            if not chunk:
                raise EOFError
            payload += chunk
        return op, payload

    def send(self, payload, binary=True):
        op = 0x2 if binary else 0x1
        mask = os.urandom(4)
        ln = len(payload)
        if ln < 126:
            hdr = bytes([0x80 | op, 0x80 | ln])
        elif ln < 65536:
            hdr = bytes([0x80 | op, 0x80 | 126]) + struct.pack(">H", ln)
        else:
            hdr = bytes([0x80 | op, 0x80 | 127]) + struct.pack(">Q", ln)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.s.sendall(hdr + mask + masked)

    def close(self):
        self.s.close()


# ------------------------------------------------------------------ RFB fake
W, H = 8, 6


def fake_pixels():
    px = bytearray()
    for i in range(W * H):
        px += bytes([i % 251, (i * 3 + 7) % 251, (i * 5 + 11) % 251, 0xFF])
    return bytes(px)


class FakeRfb:
    def __init__(self, port):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.events = []
        self.pixels = fake_pixels()
        self.conn = None
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _exact(self, s, n):
        data = b""
        while len(data) < n:
            c = s.recv(n - len(data))
            if not c:
                raise EOFError
            data += c
        return data

    def _serve(self):
        s, _ = self.srv.accept()
        self.conn = s
        s.sendall(b"RFB 003.008\n")
        self._exact(s, 12)
        s.sendall(b"\x01\x01")  # 1 security type: NONE
        self._exact(s, 1)
        s.sendall(struct.pack(">I", 0))  # security result OK
        self._exact(s, 1)  # ClientInit
        name = b"FakeVM00"
        fmt = b"\x20\x18\x00\x01\x00\xff\x00\xff\x00\xff\x10\x08\x00\x00\x00\x00"
        s.sendall(struct.pack(">HH", W, H) + fmt + struct.pack(">I", len(name)) + name)

        got_enc = False
        while True:
            t = self._exact(s, 1)[0]
            if t == 0:  # SetPixelFormat
                self._exact(s, 19)
            elif t == 2:  # SetEncodings
                b = self._exact(s, 3)
                nenc = b[2]
                self._exact(s, 4 * nenc)
                got_enc = True
            elif t == 3:  # FBUpdateReq
                self._exact(s, 9)
                if got_enc:
                    self._send_update(False)
            elif t == 4:  # KeyEvent
                d = self._exact(s, 7)
                self.events.append(("key", struct.unpack(">I", d[3:7])[0], d[0]))
            elif t == 5:  # PointerEvent
                d = self._exact(s, 5)
                self.events.append(("pointer", d[0], struct.unpack(">HH", d[1:5])))
            elif t == 6:  # ClientCutText
                d = self._exact(s, 7)
                ln = struct.unpack(">I", d[3:7])[0]
                self._exact(s, ln)
            else:
                break

    def _send_update(self, incremental):
        if incremental:
            return
        # rect RAW com o padrão de pixels
        rect = struct.pack(">HHHHi", 0, 0, W, H, 0) + self.pixels
        msg = b"\x00" + struct.pack(">H", 1) + b"\x00" + rect
        self.conn.sendall(msg)

    def send_cuttext(self, text):
        payload = text.encode()
        self.conn.sendall(b"\x03" + struct.pack(">III", 0, 0, len(payload)) + payload)

    def events_after(self, n):
        for _ in range(200):
            if len(self.events) >= n:
                break
            time.sleep(0.02)
        return list(self.events)


# ------------------------------------------------------------------ helpers
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_health(port, tries=60):
    for _ in range(tries):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/health", timeout=2) as r:
                body = json.load(r)
                return body
        except Exception:
            time.sleep(0.1)
    raise RuntimeError("apolovmd não subiu o health")


def main():
    vnc_port = free_port()
    http_port = free_port()
    import tempfile

    webroot = tempfile.mkdtemp(prefix="apolovm-web-")
    with open(os.path.join(webroot, "index.html"), "w") as f:
        f.write("<!doctype html><title>apolovm</title>V2 E2E")

    fake = FakeRfb(vnc_port)
    log = open(os.path.join(tempfile.gettempdir(), "apolovm-e2e.log"), "w")
    proc = subprocess.Popen(
        [BIN, "--vnc-port", str(vnc_port), "--port", str(http_port),
         "--webroot", webroot],
        stdout=log, stderr=log,
    )
    fails = []
    try:
        body = wait_health(http_port)
        assert body.get("app") == "apolovmd" and body.get("auth") == "none", body
        print("  [ok] /api/v1/health sem auth:", body)
        print("  [..] conectando /ws/display ..."); import sys; sys.stdout.flush()

        # ---- display ----
        d = Ws("127.0.0.1", http_port, "/api/v1/vms/fake/ws/display")
        op, hello = d.recv_msg()
        assert op == 0x1, "hello deveria ser texto"
        hello_json = json.loads(hello)
        assert hello_json.get("protocol") == "AGP/1", hello_json
        print("  [ok] display: HELLO", hello_json)

        op, frame = d.recv_msg()
        assert op == 0x2, f"frame binário esperado, op={op}"
        msg = agp_decode(frame)
        assert msg and msg["type"] == T_FRAME, "primeiro frame deve ser FRAME"
        assert (msg["width"], msg["height"]) == (W, H), (msg["width"], msg["height"])
        assert len(msg["payload"]) == W * H * 4
        assert msg["payload"] == fake.pixels, "payload BGRA deve ser idêntico ao VNC fake"
        print("  [ok] display: FRAME 8x6 (',', payload idêntico ao RFB)")

        # ---- input: teclado ----
        i = Ws("127.0.0.1", http_port, "/api/v1/vms/fake/ws/input")
        i.send(agp_encode(T_KEY, payload=json.dumps({"keysym": 0xFF0D, "down": True}).encode(), flags=1))
        i.send(agp_encode(T_KEY, payload=json.dumps({"keysym": 0xFF0D, "down": False}).encode(), flags=1))
        evs = fake.events_after(2)
        assert ("key", 0xFF0D, 1) in evs and ("key", 0xFF0D, 0) in evs, evs
        print("  [ok] input: KEY 0xFF0D down/up chegou ao RFB")

        # ---- input: mouse + scroll ----
        i.send(agp_encode(T_MOUSE, payload=json.dumps({"x": 4, "y": 3, "buttons": 1}).encode(), flags=1))
        evs = fake.events_after(3)
        assert any(kind == "pointer" and m == 1 and (x, y) == (4, 3) for kind, m, (x, y) in evs), evs
        print("  [ok] input: MOUSE tudo-mapa (move+left) -> RFB abs")

        i.send(agp_encode(T_MOUSE, payload=json.dumps({"x": 4, "y": 3, "buttons": 0, "wheel": 2}).encode(), flags=1))
        evs = fake.events_after(5)
        seq = [e for e in evs if e[0] == "pointer" and e[1] in (4, 5)]
        assert len(seq) >= 2 and all(b == 4 and x == 4 and y == 3 for _, b, (x, y) in seq), seq
        print("  [ok] input: SCROLL wheel=2 -> botões 4 press+release")

        # ---- clipboard ----
        c = Ws("127.0.0.1", http_port, "/api/v1/vms/fake/ws/clipboard")
        fake.send_cuttext("olá mundo")
        op, txt = c.recv_msg()
        assert txt.decode() == "olá mundo", txt
        print("  [ok] clipboard: ServerCutText -> /ws/clipboard")

        # ---- HTTP estático ----
        with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/", timeout=3) as r:
            assert b"V2 E2E" in r.read()
        print("  [ok] HTTP estático (index.html)")

        d.close(); i.close(); c.close()
    except Exception as e:  # noqa
        fails.append(e)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        fake.srv.close()

    if fails:
        print("FALHOU:", fails)
        print("log:", os.path.join(tempfile.gettempdir(), "apolovm-e2e.log"))
        return 1
    print("test_daemon: OK (E2E display/input/scroll/clipboard/http)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())