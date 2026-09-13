/* apolovm-main/src/ws.h
 * Servidor HTTP/1.1 + WebSocket RFC 6455 em C++ puro (poll, não-bloqueante).
 * Sem autenticação — delega a rota e o upgrade por callback.
 */
#ifndef APOLOVM_WS_H
#define APOLOVM_WS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace apg {

struct HttpRequest {
    std::string method;
    std::string path;      /* sem query */
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string header_str(const std::string &name) const;
};

/* Máximo que aceitamos de um frame WebSocket (128 MiB — cabe até framebuffer). */
constexpr size_t kMaxWsFrame = 128u << 20;

class WsServer {
public:
    using OnHttp = std::function<void(WsServer &, int fd, const HttpRequest &)>;
    using OnUpgrade = std::function<void(WsServer &, int fd, const HttpRequest &)>;
    using OnMessage = std::function<void(int fd, bool binary, const uint8_t *, size_t)>;
    using OnClose = std::function<void(int fd)>;

    WsServer() = default;
    ~WsServer();

    bool listen(const std::string &host, int port);
    int port() const { return bound_port_; }
    void set_http_handler(OnHttp f) { on_http_ = std::move(f); }
    void set_upgrade_handler(OnUpgrade f) { on_upgrade_ = std::move(f); }
    void set_message_handler(OnMessage f) { on_message_ = std::move(f); }
    void set_close_handler(OnClose f) { on_close_ = std::move(f); }

    /* Processa eventos por até timeout_ms. Callbacks são síncronos aqui. */
    void loop(int timeout_ms);

    /* Envia uma mensagem WebSocket (enfileira no fd; flush no loop). */
    void ws_send(int fd, const void *data, size_t n, bool binary);

    /* Responde 101 Switching Protocols para o upgrade já negociado. */
    void upgrade_response(int fd, const std::string &sec_key, const std::string &channel);

    /* Resposta HTTP simples (status 200/404/...). */
    void http_reply(int fd, int status, const char *ctype, const std::string &body);

    void ws_close(int fd);
    bool is_open(int fd) const { return conns_.count(fd) != 0; }

private:
    struct Conn {
        int fd = -1;
        bool upgraded = false;
        bool closed_log = false;
        std::string in;      /* dados crus do socket */
        std::string out;     /* dados para escrever */
        /* parse WS */
        std::string wsbuf;   /* payload acumulado */
        size_t payload_seen = 0;
        uint64_t frame_len = 0;
        uint8_t frame_op = 0;
        bool frame_fin = true;
        bool frame_masked = false;
        uint8_t mask[4] = {0, 0, 0, 0};
        bool frag = false;
        bool frag_binary = false;
        std::string frag_data;
        bool close_sent = false;
        bool close_after_write = false;
        std::string channel; /* rota escolhida: display/input/console/clipboard */
    };

    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::map<int, Conn> conns_;
    OnHttp on_http_;
    OnUpgrade on_upgrade_;
    OnMessage on_message_;
    OnClose on_close_;

    void accept_new();
    void read_data(Conn &c);
    void process_http(Conn &c);
    bool process_ws(Conn &c);
    void handle_ws_payload(Conn &c, const uint8_t *payload, size_t n, uint8_t opcode);
    void queue_close_frame(int fd, uint16_t code, const char *reason);
    bool flush_out(Conn &c);
    void drop(int fd);
};

} // namespace apg

#endif /* APOLOVM_WS_H */