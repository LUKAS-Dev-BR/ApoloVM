/* apolovm-main/src/daemon.cpp
 * apolovmd — daemon gráfico da ApoloVM V2.
 *
 * Papéis:
 *   - servidor HTTP (web/dist da frontend) sem autenticação;
 *   - canais WebSocket /api/v1/vms/<vm>/ws/{display,input,console,clipboard};
 *   - cliente VNC real (C: vnc.c) alimentando framebuffer AGP (Rust delta);
 *   - rota de console serial (unix socket) para o QEMU.
 *
 * Compilação: C++17 + agp.c + vnc.c + ws.cpp + asm + rust staticlib.
 */
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "agp.h"
#include "vnc.h"
}
#include "ws.h"

/* moletom: encoder Delta do Rust (libapolographics). Se APG_EMBED_DELTA_CPP
 * for definido, usa o fallback em C++ abaixo. */
#ifndef APG_EMBED_DELTA_CPP
extern "C" long long apg_rust_encode_delta(unsigned char *dst, unsigned long dstcap,
                                           const unsigned char *fb, unsigned int fw,
                                           unsigned int fh, const unsigned int *rects,
                                           unsigned long nrects);
#endif

namespace {

struct Options {
    std::string host = "0.0.0.0";
    int port = 8000;
    std::string webroot = "web/dist";
    std::string vnc_host = "127.0.0.1";
    int vnc_port = 5900;
    std::string vnc_pass;
    std::string console_sock;
};

struct DirtyRect {
    unsigned x, y, w, h;
};

class Daemon {
public:
    explicit Daemon(Options o) : opts_(std::move(o)) {}

    int run() {
        ws_.set_http_handler([this](apg::WsServer &s, int fd, const apg::HttpRequest &r) {
            on_http(s, fd, r);
        });
        ws_.set_message_handler([this](int fd, bool binary, const uint8_t *p, size_t n) {
            on_message(fd, binary, p, n);
        });
        ws_.set_close_handler([this](int fd) {
            if (display_clients_.erase(fd)) have_display_ = !display_clients_.empty();
            console_clients_.erase(fd);
            clipboard_clients_.erase(fd);
        });

        if (!ws_.listen(opts_.host, opts_.port)) {
            fprintf(stderr, "apolovmd: não consegui abrir %s:%d\n", opts_.host.c_str(),
                    opts_.port);
            return 1;
        }
        fprintf(stderr, "apolovmd: balcão %s:%d (WebSocket + HTTP)\n", opts_.host.c_str(),
                ws_.port());

        open_console();

        std::atomic<bool> stop{false};
        signal(SIGINT, [](int) { g_stop = true; });
        signal(SIGTERM, [](int) { g_stop = true; });
        g_stop = false;

        long long last_try = 0;
        g_stop = false;
        while (!g_stop && !stop) {
            ws_.loop(20);

            if (vnc_ && !vnc_dead_ && apg_vnc_fd(vnc_) >= 0) {
                pollfd p{apg_vnc_fd(vnc_), POLLIN, 0};
                if (::poll(&p, 1, 0) > 0) apg_vnc_consume(vnc_);
            } else {
                long long now = ::time(nullptr);
                if (now - last_try > 2) {
                    last_try = now;
                    try_vnc();
                }
            }
            pump_console();
            flush_frames();
        }

        fprintf(stderr, "apolovmd: encerrando\n");
        ws_.ws_close(-1); /* no-op */
        close_console();
        if (vnc_) apg_vnc_destroy(vnc_);
        return 0;
    }

private:
    static std::atomic<bool> g_stop;
    Options opts_;
    apg::WsServer ws_;
    apg_vnc *vnc_ = nullptr;
    bool vnc_dead_ = true;
    unsigned last_vnc_w_ = 0, last_vnc_h_ = 0;

    std::vector<uint8_t> fb_;
    unsigned fb_w_ = 0, fb_h_ = 0;
    bool full_pending_ = false;   /* cliente quer frame completo (1º ou resize) */
    bool resize_pending_ = false;
    std::vector<DirtyRect> dirty_;
    bool update_pending_ = false;

    std::set<int> display_clients_;
    bool have_display_ = false;
    std::set<int> console_clients_;
    std::set<int> clipboard_clients_;

    int console_fd_ = -1;
    std::string clipboard_text_;

    /* ---------------- VNC ---------------- */
    static void vnc_cb(void *ud, int ev, int a0, int a1, const void *ptr) {
        Daemon *d = static_cast<Daemon *>(ud);
        switch (ev) {
        case APG_VNC_EV_RESIZE:
            d->vnc_resize((unsigned)a0, (unsigned)a1);
            break;
        case APG_VNC_EV_RECT: {
            const apg_vnc_rect *r = static_cast<const apg_vnc_rect *>(ptr);
            d->vnc_rect(r);
            break;
        }
        case APG_VNC_EV_COPYRECT:
            d->vnc_copyrect(static_cast<const apg_vnc_copyrect *>(ptr));
            break;
        case APG_VNC_EV_CUTTEXT:
            d->apply_cuttext(static_cast<const char *>(ptr));
            break;
        case APG_VNC_EV_CLOSED:
            d->vnc_dead_ = true;
            break;
        case APG_VNC_EV_ERROR:
            d->vnc_dead_ = true;
            break;
        default:
            break;
        }
    }

    void try_vnc() {
        if (vnc_ && !vnc_dead_) return;
        if (vnc_) {
            apg_vnc_destroy(vnc_);
            vnc_ = nullptr;
        }
        char err[160] = {0};
        vnc_ = apg_vnc_new(opts_.vnc_host.c_str(), (uint16_t)opts_.vnc_port,
                           opts_.vnc_pass.empty() ? nullptr : opts_.vnc_pass.c_str());
        if (!vnc_) return;
        apg_vnc_set_callback(vnc_, vnc_cb, this);
        if (apg_vnc_connect(vnc_, 1500, err, sizeof(err)) == 0) {
            vnc_dead_ = false;
            fprintf(stderr, "apolovmd: VNC conectado a %s:%d\n", opts_.vnc_host.c_str(),
                    opts_.vnc_port);
            /* força um refresh completo no 1º frame */
            full_pending_ = true;
        } else {
            fprintf(stderr, "apolovmd: VNC %s:%d — %s\n", opts_.vnc_host.c_str(),
                    opts_.vnc_port, err);
            apg_vnc_destroy(vnc_);
            vnc_ = nullptr;
        }
    }

    void vnc_resize(unsigned w, unsigned h) {
        fb_w_ = w;
        fb_h_ = h;
        fb_.assign(w * h * 4, 0);
        dirty_.clear();
        dirty_.push_back({0, 0, w, h});
        full_pending_ = true;
        resize_pending_ = true;
        update_pending_ = true;
        last_vnc_w_ = w;
        last_vnc_h_ = h;
        fprintf(stderr, "apolovmd: sessão VNC %ux%u\n", w, h);
    }

    void vnc_rect(const apg_vnc_rect *r) {
        if (!fb_w_ || !fb_h_) return;
        const unsigned x = r->head.x, y = r->head.y, w = r->head.w, h = r->head.h;
        if (x + w > fb_w_ || y + h > fb_h_) return;
        for (unsigned row = 0; row < h; row++) {
            size_t dst = ((size_t)(y + row) * fb_w_ + x) * 4;
            size_t src = (size_t)row * w * 4;
            memcpy(fb_.data() + dst, r->data + src, (size_t)w * 4);
        }
        dirty_.push_back({x, y, w, h});
        update_pending_ = true;
    }

    void vnc_copyrect(const apg_vnc_copyrect *cr) {
        /* forma escalar; opcional neste estágio */
        (void)cr;
    }

    /* ---------------- espectadores ---------------- */
    void on_http(apg::WsServer &s, int fd, const apg::HttpRequest &r) {
        const std::string &p = r.path;

        if (p == "/api/v1/health") {
            std::string body = "{\"status\":\"ok\",\"app\":\"apolovmd\",\"version\":\"2.0\","
                               "\"engine\":\"qemu\",\"auth\":\"none\"}";
            s.http_reply(fd, 200, "application/json", body);
            return;
        }

        /* upgrade WebSocket */
        static const char *up = "upgrade";
        if (r.header_str("Upgrade").size() >= std::strlen(up) && p.find("/ws/") != std::string::npos) {
            bool known = p.find("/ws/display") != std::string::npos ||
                         p.find("/ws/input") != std::string::npos ||
                         p.find("/ws/console") != std::string::npos ||
                         p.find("/ws/clipboard") != std::string::npos;
            if (!known) {
                s.http_reply(fd, 404, "text/plain", "no such channel");
                return;
            }
            std::string key = r.header_str("Sec-WebSocket-Key");
            if (key.empty()) {
                s.http_reply(fd, 400, "text/plain", "missing Sec-WebSocket-Key");
                return;
            }
            std::string channel =
                (p.find("/ws/display") != std::string::npos) ? "display"
                : (p.find("/ws/input") != std::string::npos)   ? "input"
                : (p.find("/ws/clipboard") != std::string::npos) ? "clipboard"
                                                                 : "console";
            s.upgrade_response(fd, key, channel);

            if (channel == "display") {
                display_clients_.insert(fd);
                have_display_ = true;
                /* HELLO + frame imediato */
                std::string hello = "{\"protocol\":\"AGP/1\",\"app\":\"apolovmd\",\"version\":\"2.0\"}";
                s.ws_send(fd, hello.data(), hello.size(), false);
                if (!fb_.empty()) {
                    if (!full_pending_) full_pending_ = true;
                    update_pending_ = true;
                }
            } else if (channel == "clipboard") {
                clipboard_clients_.insert(fd);
                if (!clipboard_text_.empty())
                    s.ws_send(fd, clipboard_text_.data(), clipboard_text_.size(), false);
            } else if (channel == "console") {
                console_clients_.insert(fd);
            }
            return;
        }

        /* qualquer outra coisa: estático do web/dist (SPA fallback) */
        serve_static(fd, p);
    }

    void on_message(int fd, bool binary, const uint8_t *payload, size_t n) {
        if (n < 8) return;
        agp_msg msg;
        if (agp_decode(payload, n, &msg) != 1) {
            return;
        }
        switch (msg.type) {
        case AGP_KEY: {
            const char *j = (const char *)msg.payload;
            uint32_t keysym = 0;
            int down = 0;
            if (msg.flags & AGP_FLAG_JSON) {
                if (sscanf(j, "{\"keysym\":%9u,\"down\":%1d", &keysym, &down) >= 1)
                    apg_vnc_key(vnc_ ? vnc_ : nullptr, keysym, down != 0);
            }
            break;
        }
        case AGP_MOUSE: {
            const char *j = (const char *)msg.payload;
            int x = 0, y = 0, buttons = 0, wheel = 0;
            if (sscanf(j, "{\"x\":%d,\"y\":%d,\"buttons\":%d", &x, &y, &buttons) >= 3) {
                const char *w = strstr(j, "\"wheel\"");
                if (w) wheel = atoi(w + 8);
                if (!vnc_) break;
                if (wheel != 0) {
                    /* botão 4 = scroll para cima, 5 = para baixo */
                    unsigned v = (wheel > 0) ? 4u : 5u;
                    apg_vnc_pointer(vnc_, buttons | (int)v, x, y);
                    apg_vnc_pointer(vnc_, buttons, x, y);
                } else {
                    apg_vnc_pointer(vnc_, buttons, x, y);
                }
            }
            break;
        }
        case AGP_CLIPBOARD: {
            std::string text((const char *)msg.payload, msg.len);
            apply_cuttext(text.c_str());
            break;
        }
        case AGP_PING: {
            uint8_t pong[AGP_HEADER_SIZE];
            agp_write_header(pong, AGP_PONG, 0, 0, 0, 0);
            ws_.ws_send(fd, pong, AGP_HEADER_SIZE, true);
            break;
        }
        case AGP_CONSOLE:
            if (console_fd_ >= 0 && (binary || true)) {
                ssize_t r = ::write(console_fd_, msg.payload, msg.len);
                (void)r;
            }
            break;
        default:
            break;
        }
    }

    void apply_cuttext(const char *txt) {
        clipboard_text_ = txt ? txt : "";
        for (int fd : clipboard_clients_) ws_.ws_send(fd, clipboard_text_.data(),
                                                     clipboard_text_.size(), false);
        if (vnc_ && txt) apg_vnc_cuttext(vnc_, txt);
    }

    /* ---------------- arquivos estáticos ---------------- */
    void serve_static(int fd, const std::string &path) {
        std::string rel = path;
        if (rel.empty() || rel == "/") rel = "/index.html";
        while (rel.size() > 1 && rel[0] == '/') rel.erase(0, 1);
        if (rel.find("..") != std::string::npos) {
            ws_.http_reply(fd, 403, "text/plain", "forbidden");
            return;
        }
        std::string full = opts_.webroot + "/" + rel;
        FILE *f = fopen(full.c_str(), "rb");
        if (!f) {
            /* fallback SPA para index.html */
            full = opts_.webroot + "/index.html";
            f = fopen(full.c_str(), "rb");
            if (!f) {
                ws_.http_reply(fd, 404, "text/plain", "web/dist não encontrado");
                return;
            }
        }
        std::string body;
        char buf[65536];
        size_t k;
        while ((k = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, k);
        fclose(f);

        const char *ct = "application/octet-stream";
        auto has_suffix = [&](const char *ext) {
            size_t n = strlen(ext);
            return rel.size() >= n && rel.compare(rel.size() - n, n, ext) == 0;
        };
        if (has_suffix(".html")) ct = "text/html";
        else if (has_suffix(".js")) ct = "application/javascript";
        else if (has_suffix(".css")) ct = "text/css";
        else if (has_suffix(".json")) ct = "application/json";
        else if (has_suffix(".svg")) ct = "image/svg+xml";
        else if (has_suffix(".png")) ct = "image/png";
        else if (has_suffix(".ico")) ct = "image/x-icon";
        else if (has_suffix(".woff2")) ct = "font/woff2";
        else if (has_suffix(".map")) ct = "application/json";

        ws_.http_reply(fd, 200, ct, body);
    }

    /* ---------------- console serial ---------------- */
    void open_console() {
        if (opts_.console_sock.empty()) return;
        console_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (console_fd_ < 0) return;
        sockaddr_un su{};
        su.sun_family = AF_UNIX;
        snprintf(su.sun_path, sizeof(su.sun_path), "%s", opts_.console_sock.c_str());
        if (::connect(console_fd_, (sockaddr *)&su, sizeof(su)) != 0) {
            close(console_fd_);
            console_fd_ = -1;
            return;
        }
        int fl = fcntl(console_fd_, F_GETFL, 0);
        fcntl(console_fd_, F_SETFL, fl | O_NONBLOCK);
        fprintf(stderr, "apolovmd: console conectado (%s)\n", opts_.console_sock.c_str());
    }

    void pump_console() {
        if (console_fd_ < 0) return;
        pollfd p{console_fd_, POLLIN, 0};
        if (::poll(&p, 1, 0) <= 0) return;
        char buf[8192];
        ssize_t n = ::read(console_fd_, buf, sizeof(buf));
        if (n <= 0) return;
        for (int fd : console_clients_) ws_.ws_send(fd, buf, (size_t)n, false);
    }

    void close_console() {
        if (console_fd_ >= 0) close(console_fd_);
        console_fd_ = -1;
    }

    /* ---------------- flush AGP ---------------- */
    void flush_frames() {
        if (!update_pending_) return;
        update_pending_ = false;
        if (!have_display_) {
            dirty_.clear();
            return;
        }
        if (fb_.empty()) return;

        std::vector<uint8_t> frame;

        if (full_pending_) {
            frame.resize(agp_frame_size((uint32_t)fb_.size()));
            unsigned char *out = frame.data();
            (void)out;
            agp_write_header(frame.data(), AGP_FRAME, 0, fb_w_, fb_h_, (uint32_t)fb_.size());
            memcpy(frame.data() + AGP_HEADER_SIZE, fb_.data(), fb_.size());
            full_pending_ = false;
            for (int fd : display_clients_) ws_.ws_send(fd, frame.data(), frame.size(), true);
            dirty_.clear();
            return;
        }

        if (!dirty_.empty()) {
            size_t total_pix = 0;
            for (const auto &d : dirty_) total_pix += (size_t)d.w * d.h;
            if (total_pix < 1) return;

            frame.resize(AGP_HEADER_SIZE + 1024 + total_pix * 4);
            uint8_t *base = frame.data();

            long long body_n;
#ifndef APG_EMBED_DELTA_CPP
            std::vector<unsigned> r4;
            r4.reserve(dirty_.size() * 4);
            for (const auto &d : dirty_) {
                r4.push_back(d.x);
                r4.push_back(d.y);
                r4.push_back(d.w);
                r4.push_back(d.h);
            }
            body_n = apg_rust_encode_delta(base + AGP_HEADER_SIZE, frame.size() - AGP_HEADER_SIZE,
                                           fb_.data(), fb_w_, fb_h_, r4.data(), r4.size() / 4);
#else
            body_n = encode_delta_cpp(base + AGP_HEADER_SIZE, frame.size() - AGP_HEADER_SIZE,
                                      fb_.data(), fb_w_, fb_h_, dirty_);
#endif
            if (body_n < 0) {
                /* buffer pequeno: reenvia frame completo */
                full_pending_ = true;
                update_pending_ = true;
                return;
            }
            agp_write_header(base, AGP_FRAME_DELTA, 0, fb_w_, fb_h_, (uint32_t)body_n);
            frame.resize(AGP_HEADER_SIZE + (size_t)body_n);
            for (int fd : display_clients_) ws_.ws_send(fd, frame.data(), frame.size(), true);
            dirty_.clear();
        }
    }

#ifndef APG_EMBED_DELTA_CPP
#else
    long long encode_delta_cpp(uint8_t *dst, size_t dstcap, const uint8_t *fb, unsigned fw,
                               unsigned fh, const std::vector<DirtyRect> &ds) {
        (void)fh;
        std::string head = "[";
        for (size_t i = 0; i < ds.size(); i++) {
            if (i) head += ",";
            head += "{\"x\":" + std::to_string(ds[i].x) + ",\"y\":" + std::to_string(ds[i].y) +
                    ",\"w\":" + std::to_string(ds[i].w) + ",\"h\":" + std::to_string(ds[i].h) +
                    "}";
        }
        head += "]\n";
        size_t need = head.size();
        for (const auto &d : ds) need += (size_t)d.w * d.h * 4;
        if (need > dstcap) return -1;
        memcpy(dst, head.data(), head.size());
        size_t off = head.size();
        for (const auto &d : ds) {
            for (unsigned row = 0; row < d.h; row++) {
                size_t src = ((size_t)(d.y + row) * fw + d.x) * 4;
                memcpy(dst + off, fb + src, (size_t)d.w * 4);
                off += (size_t)d.w * 4;
            }
        }
        return (long long)need;
    }
#endif
};

std::atomic<bool> Daemon::g_stop{false};

void usage() {
    fprintf(stderr,
            "uso: apolovmd [opções]\n"
            "  --host    HOST          interface (default 0.0.0.0)\n"
            "  --port    PORT          porta HTTP/WS (default 8000)\n"
            "  --webroot DIR           pasta estática (default web/dist)\n"
            "  --vnc-host HOST         QEMU VNC (default 127.0.0.1)\n"
            "  --vnc-port PORT         porta VNC (default 5900)\n"
            "  --vnc-password SENHA    senha do VNC (se houver)\n"
            "  --console-sock PATH     socket unix do serial chardev\n"
            "  --web <vm>              atalho: sobe o display do VNC para web\n");
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 < argc) { i++; return argv[i]; }
            fprintf(stderr, "apolovmd: %s precisa de um argumento\n", name);
            exit(2);
        };
        if (a == "--host") o.host = next("--host");
        else if (a == "--port") o.port = atoi(next("--port").c_str());
        else if (a == "--webroot") o.webroot = next("--webroot");
        else if (a == "--vnc-host") o.vnc_host = next("--vnc-host");
        else if (a == "--vnc-port") o.vnc_port = atoi(next("--vnc-port").c_str());
        else if (a == "--vnc-password") o.vnc_pass = next("--vnc-password");
        else if (a == "--console-sock") o.console_sock = next("--console-sock");
        else if (a == "--web") { /* aceita e ignora: display embutido */
            next("--web");
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "apolovmd: argumento desconhecido: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    {
        Daemon d(std::move(o));
        return d.run();
    }
}