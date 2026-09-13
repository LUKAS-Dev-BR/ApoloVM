/* apolovm-main/src/ws.cpp
 * Servidor HTTP/1.1 + WebSocket RFC 6455 (server side).
 * SHA-1 e base64 implementados aqui mesmo (sem dependências externas).
 */
#include "ws.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace apg {

/* ------------------------------------------------------------------ sha1 */
static inline uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static std::string base64_encode(const unsigned char *data, size_t n) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? tbl[v & 63] : '=';
    }
    return out;
}

static std::string sha1_raw(const std::string &msg) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
             h4 = 0xC3D2E1F0;
    uint64_t ml = (uint64_t)msg.size() * 8;
    std::string padded = msg;
    padded.push_back((char)0x80);
    while ((padded.size() % 64) != 56) padded.push_back(0);
    for (int i = 7; i >= 0; i--) padded.push_back((char)(ml >> (i * 8)));

    for (size_t off = 0; off < padded.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            uint32_t v = 0;
            for (int j = 0; j < 4; j++) v = (v << 8) | (uint8_t)padded[off + i * 4 + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    std::string out;
    for (int i = 0; i < 20; i++) {
        uint32_t v = hs[i / 4];
        v >>= (3 - (i % 4)) * 8;
        out.push_back((char)(v & 0xff));
    }
    return out;
}

static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string HttpRequest::header_str(const std::string &name) const {
    for (const auto &h : headers) {
        if (h.first.size() == name.size() &&
            strcasecmp(h.first.c_str(), name.c_str()) == 0) {
            return h.second;
        }
    }
    return std::string();
}

/* ------------------------------------------------------------------ server */
WsServer::~WsServer() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (auto &[fd, c] : conns_) ::close(fd);
}

bool WsServer::listen(const std::string &host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (host.empty() || host == "0.0.0.0") sa.sin_addr.s_addr = INADDR_ANY;
    else if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::bind(fd, (sockaddr *)&sa, sizeof(sa)) != 0 || ::listen(fd, 64) != 0) {
        ::close(fd);
        return false;
    }
    socklen_t len = sizeof(sa);
    getsockname(fd, (sockaddr *)&sa, &len);
    listen_fd_ = fd;
    bound_port_ = ntohs(sa.sin_port);
    return true;
}

void WsServer::accept_new() {
    for (;;) {
        sockaddr_in sa{};
        socklen_t sl = sizeof(sa);
        int fd = ::accept(listen_fd_, (sockaddr *)&sa, &sl);
        if (fd < 0) return;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        Conn c;
        c.fd = fd;
        conns_[fd] = std::move(c);
    }
}

bool WsServer::flush_out(Conn &c) {
    while (!c.out.empty()) {
        ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (w > 0) {
            c.out.erase(0, (size_t)w);
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return false;
}

void WsServer::read_data(Conn &c) {
    char buf[65536];
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.in.append(buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            drop(c.fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        drop(c.fd);
        return;
    }
}

/* Responde ao upgrade e marca a conexão como WebSocket. */
void WsServer::upgrade_response(int fd, const std::string &key, const std::string &channel) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn &c = it->second;

    std::string accept = base64_encode(
        (const unsigned char *)sha1_raw(key + kGuid).data(), 20);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "Sec-WebSocket-Protocol: apolovm\r\n\r\n",
                     accept.c_str());
    if (n > 0) c.out.append(buf, (size_t)n);
    c.channel = channel;
    c.upgraded = true;
}

/* Resposta HTTP simples. */
void WsServer::http_reply(int fd, int status, const char *ctype, const std::string &body) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn &c = it->second;
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Server: apolovmd/2.0\r\n"
                     "Connection: close\r\n\r\n",
                     status, (status == 200) ? "OK" : (status == 404) ? "Not Found" : "Error",
                     ctype, body.size());
    (void)n;
    c.out.append(head);
    c.out.append(body);
    c.close_after_write = true;
}

void WsServer::process_http(Conn &c) {
    size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (c.in.size() > 65536) drop(c.fd);
        return;
    }
    HttpRequest req;
    std::string head = c.in.substr(0, end);
    c.in.erase(0, end + 4);

    size_t eol = head.find("\r\n");
    std::string start = head.substr(0, eol);
    size_t sp1 = start.find(' ');
    size_t sp2 = start.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        drop(c.fd);
        return;
    }
    req.method = start.substr(0, sp1);
    std::string target = start.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qm = target.find('?');
    if (qm != std::string::npos) {
        req.path = target.substr(0, qm);
        req.query = target.substr(qm + 1);
    } else {
        req.path = target;
    }

    std::string rest = head.substr(eol + 2);
    size_t pos = 0;
    while ((pos = rest.find("\r\n")) != std::string::npos) {
        std::string line = rest.substr(0, pos);
        rest.erase(0, pos + 2);
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        while (!v.empty() && v.back() == '\r') v.pop_back();
        req.headers.emplace_back(k, v);
    }

    if (!on_http_) {
        std::string msg =
            "HTTP/1.1 500 NoHandler\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c.out.append(msg);
        return;
    }
    on_http_(*this, c.fd, req);
}

bool WsServer::process_ws(Conn &c) {
    while (true) {
        if (c.in.size() < 2) break;
        uint8_t b0 = (uint8_t)c.in[0];
        uint8_t b1 = (uint8_t)c.in[1];
        uint8_t opcode = b0 & 0x0f;
        bool fin = (b0 & 0x80) != 0;
        bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7f;
        size_t off = 2;

        if (len == 126) {
            if (c.in.size() < off + 2) break;
            len = ((uint64_t)(uint8_t)c.in[off] << 8) | (uint8_t)c.in[off + 1];
            off += 2;
        } else if (len == 127) {
            if (c.in.size() < off + 8) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)c.in[off + i];
            off += 8;
        }
        if (masked) {
            if (c.in.size() < off + 4) break;
            memcpy(c.mask, c.in.data() + off, 4);
            off += 4;
        }
        if (len > kMaxWsFrame) {
            drop(c.fd);
            return true;
        }
        if (c.in.size() < off + len) break;

        std::string payload = c.in.substr(off, (size_t)len);
        if (masked) {
            for (size_t i = 0; i < payload.size(); i++) payload[i] ^= c.mask[i & 3];
        }
        c.in.erase(0, off + (size_t)len);

        switch (opcode) {
        case 0x0:
            if (c.frag) {
                c.frag_data += payload;
                if (fin) {
                    handle_ws_payload(c, (const uint8_t *)c.frag_data.data(),
                                      c.frag_data.size(),
                                      c.frag_binary ? 0x2 : 0x1);
                    c.frag = false;
                    c.frag_data.clear();
                }
            } else {
                drop(c.fd);
                return true;
            }
            break;
        case 0x1:
        case 0x2:
            if (fin) {
                handle_ws_payload(c, (const uint8_t *)payload.data(), payload.size(), opcode);
            } else {
                c.frag = true;
                c.frag_binary = (opcode == 0x2);
                c.frag_data = std::move(payload);
            }
            break;
        case 0x8:
            queue_close_frame(c.fd, 1000, "");
            drop(c.fd);
            return true;
        case 0x9: {
            ws_send(c.fd, payload.data(), payload.size(), false);
            break;
        }
        case 0xA:
            break;
        default:
            drop(c.fd);
            return true;
        }
        if (c.in.empty()) break;
    }
    return false;
}

void WsServer::handle_ws_payload(Conn &c, const uint8_t *payload, size_t n, uint8_t opcode) {
    if (opcode == 0x1 || opcode == 0x2) {
        if (on_message_) on_message_(c.fd, opcode == 0x2, payload, n);
    }
}

void WsServer::queue_close_frame(int fd, uint16_t code, const char *reason) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn &c = it->second;
    if (c.close_sent) return;
    c.close_sent = true;
    std::string frame;
    frame.push_back((char)0x88);
    frame.push_back((char)2);
    frame.push_back((char)(code >> 8));
    frame.push_back((char)code);
    c.out.append(frame);
    if (reason) c.out.append(reason);
}

void WsServer::ws_send(int fd, const void *data, size_t n, bool binary) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn &c = it->second;
    if (c.close_sent) return;

    constexpr size_t kChunk = 1 << 24; /* 16 MiB por mensagem */
    size_t consumed = 0;
    do {
        size_t chunk = (n - consumed) > kChunk ? kChunk : (n - consumed);
        std::string f;
        f.push_back((char)(0x80 | (binary ? 0x2 : 0x1)));
        if (chunk < 126) {
            f.push_back((char)chunk);
        } else if (chunk <= 0xFFFF) {
            f.push_back((char)126);
            f.push_back((char)(chunk >> 8));
            f.push_back((char)(chunk & 0xff));
        } else {
            f.push_back((char)127);
            for (int i = 7; i >= 0; i--) f.push_back((char)((uint64_t)chunk >> (i * 8)));
        }
        f.append((const char *)data + consumed, chunk);
        consumed += chunk;
        c.out.append(f);
    } while (consumed < n);
}

void WsServer::ws_close(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    queue_close_frame(fd, 1000, "");
    flush_out(it->second);
}

void WsServer::drop(int fd) {
    conns_.erase(fd);
    if (on_close_) on_close_(fd);
    ::close(fd);
}

void WsServer::loop(int timeout_ms) {
    if (listen_fd_ < 0) return;
    std::vector<pollfd> pfds;
    std::vector<int> ids;
    pfds.push_back({listen_fd_, POLLIN, 0});
    ids.push_back(-1);
    for (auto &[fd, c] : conns_) {
        short ev = POLLIN;
        if (!c.out.empty()) ev |= POLLOUT;
        pfds.push_back({fd, ev, 0});
        ids.push_back(fd);
    }

    int pr = ::poll(pfds.data(), pfds.size(), timeout_ms);
    if (pr <= 0) return;

    if (pfds[0].revents) accept_new();

    std::vector<int> to_read;
    std::vector<int> to_write;
    for (size_t i = 1; i < pfds.size(); i++) {
        if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) to_read.push_back(ids[i]);
        if (pfds[i].revents & POLLOUT) to_write.push_back(ids[i]);
    }

    for (int fd : to_write) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) continue;
        bool more = flush_out(it->second);
        if (!more && it->second.close_sent) drop(fd);
        else if (!more && it->second.close_after_write) drop(fd);
    }

    for (int fd : to_read) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) continue;
        Conn &c = it->second;
        read_data(c);
        if (conns_.count(fd) == 0) continue;
        if (!c.upgraded) {
            process_http(c);
            if (conns_.count(fd) == 0) continue;
            if (c.upgraded && !c.in.empty()) process_ws(c);
        } else {
            if (process_ws(c)) {
                if (conns_.count(fd)) drop(fd);
            }
        }
    }
}

} // namespace apg