/* apolovm-main/src/vnc.h
 * Cliente RFB (VNC) 3.8 real em C — usado para capturar o framebuffer real do
 * QEMU e injetar teclado/mouse/scroll de verdade.
 *
 * Bloqueia em apenas três fases: connect(), o handshake e a leitura inicial.
 * Depois disso é totalmente não-bloqueante: chame apg_vnc_consume() quando o
 * fd estiver legível.
 */
#ifndef APOLOVM_VNC_H
#define APOLOVM_VNC_H

#include <stddef.h>
#include <stdint.h>

/* Eventos notificados via callback. */
enum apg_vnc_ev {
    APG_VNC_EV_RESIZE,       /* arg0=width, arg1=height */
    APG_VNC_EV_POINTER_MODE, /* arg0=1 se pointer absoluto (virtio-tablet) */
    APG_VNC_EV_LEDS,         /* arg0=máscara de LEDs */
    APG_VNC_EV_RECT,         /* ptr é apg_vnc_rect*: bloco de pixels BGRA */
    APG_VNC_EV_COPYRECT,     /* ptr é apg_vnc_copyrect* */
    APG_VNC_EV_CUTTEXT,      /* ptr é texto UTF-8 (com NUL) */
    APG_VNC_EV_BELL,
    APG_VNC_EV_CLOSED,
    APG_VNC_EV_ERROR,        /* ptr é texto do erro (com NUL) */
};

typedef void (*apg_vnc_cb)(void *ud, int ev, int arg0, int arg1, const void *ptr);

/* Chunk de framebuffer (strict-mode SAFE: não-bloqueante sem desync). */
typedef struct {
    uint16_t x, y, w, h;
    uint32_t encoding;
} apg_vnc_rect_head;

typedef struct {
    apg_vnc_rect_head head;
    const uint8_t *data; /* aponta para dentro do buffer interno; BGRA */
    uint32_t sizedata;
} apg_vnc_rect;

typedef struct {
    uint16_t x, y, w, h, sx, sy;
} apg_vnc_copyrect;

typedef struct apg_vnc apg_vnc;

/* Cria um cliente. host pode ser "127.0.0.1". */
apg_vnc *apg_vnc_new(const char *host, uint16_t port, const char *password);

/* Instala o callback de eventos e o ponteiro de contexto de usuário. */
void apg_vnc_set_callback(apg_vnc *v, apg_vnc_cb cb, void *ud);

/* Conecta e negocia o protocolo. timeout_ms (0 => sem timeout).
 * Retorna -1 em erro (strerror em errno / mensagem em *err se não for NULL). */
int apg_vnc_connect(apg_vnc *v, int timeout_ms, char *err, size_t errsz);

/* fd para poll(). -1 se desconectado. */
int apg_vnc_fd(apg_vnc *v);

/* Processa os dados disponíveis (não bloqueia). Retorna 0 em sucesso. */
int apg_vnc_consume(apg_vnc *v);

/* Injeção de entrada. */
void apg_vnc_key(apg_vnc *v, uint32_t keysym, int down);
void apg_vnc_pointer(apg_vnc *v, int button_mask, int x, int y);
void apg_vnc_cuttext(apg_vnc *v, const char *text);

/* Pede outro update incremental (chamado automaticamente após cada frame). */
void apg_vnc_update_request(apg_vnc *v, int incremental, int x, int y, int w, int h);

int apg_vnc_width(apg_vnc *v);
int apg_vnc_height(apg_vnc *v);

void apg_vnc_destroy(apg_vnc *v);

#endif /* APOLOVM_VNC_H */