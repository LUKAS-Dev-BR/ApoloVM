/* apolovm-main/src/vnc.c
 * Cliente RFB 3.8 em C puro (sem dependências além da libc).
 *
 * Portado do cliente JavaScript validado da V1 (apolo/graphics/vnc.js),
 * incluindo o modo "strict" que evita dessincronização: quando um retângulo
 * RAW está incompleto no buffer, restauramos o buffer e esperamos mais dados.
 */
#define _GNU_SOURCE
#include "vnc.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* ------------------------------------------------------------------ memória */
#define BUF_INIT 65536

struct apg_vnc {
    char host[64];
    uint16_t port;
    char password[256];
    int sock; /* -1 = fechado */

    uint8_t *buf;
    size_t bufcap;
    size_t buflen;

    uint16_t width;
    uint16_t height;
    int absolute_pointer;
    int handshake_done;

    apg_vnc_cb cb;
    void *ud;
    int cb_live; /* emite errors apenas se houver listener */
};

/* Mensagens RFB (cliente -> servidor) */
enum {
    MSG_SET_PIXEL_FORMAT = 0,
    MSG_SET_ENCODINGS = 2,
    MSG_FB_UPDATE_REQ = 3,
    MSG_KEY_EVENT = 4,
    MSG_POINTER_EVENT = 5,
    MSG_CLIENT_CUT_TEXT = 6,
};

/* Mensagens RFB (servidor -> cliente) */
enum {
    MSG_SRV_FB_UPDATE = 0,
    MSG_SRV_BELL = 2,
    MSG_SRV_CUT_TEXT = 3,
};

#define ENC_RAW 0
#define ENC_COPY_RECT 1
#define ENC_DESKTOP_SIZE (-223)
#define ENC_POINTER_TYPE_CHANGE (-257)
#define ENC_LED_STATE (-261)

#define SEC_NONE 1
#define SEC_VNC 2
#define SEC_RA2NE 5

static int set_tcp_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* ---- buffer interno ---- */
static int buf_ensure(apg_vnc *v, size_t extra) {
    if (v->buflen + extra <= v->bufcap) return 0;
    size_t cap = v->bufcap ? v->bufcap : BUF_INIT;
    while (cap < v->buflen + extra) cap *= 2;
    uint8_t *nb = realloc(v->buf, cap);
    if (!nb) return -1;
    v->buf = nb;
    v->bufcap = cap;
    return 0;
}

/* acrescenta dados do socket ao buffer (até count bytes); retorna #lidos */
static ssize_t sock_recv(apg_vnc *v) {
    if (buf_ensure(v, 65536) != 0) return -1;
    ssize_t n = recv(v->sock, v->buf + v->buflen, v->bufcap - v->buflen, 0);
    if (n > 0) v->buflen += (size_t)n;
    return n;
}

/* consome n bytes do início do buffer */
static void buf_consume(apg_vnc *v, size_t n) {
    v->buflen -= n;
    memmove(v->buf, v->buf + n, v->buflen);
}

/* ---- I/O auxiliares ---- */
static int rd_u8(apg_vnc *v, uint8_t *out) {
    if (v->buflen < 1) return -1;
    *out = v->buf[0];
    buf_consume(v, 1);
    return 0;
}

static int rd_u16be(apg_vnc *v, uint16_t *out) {
    if (v->buflen < 2) return -1;
    *out = (uint16_t)((v->buf[0] << 8) | v->buf[1]);
    buf_consume(v, 2);
    return 0;
}

static int rd_u32be(apg_vnc *v, uint32_t *out) {
    if (v->buflen < 4) return -1;
    *out = ((uint32_t)v->buf[0] << 24) | ((uint32_t)v->buf[1] << 16) |
           ((uint32_t)v->buf[2] << 8) | v->buf[3];
    buf_consume(v, 4);
    return 0;
}

/* ---- emissão de eventos ---- */
static void ev(apg_vnc *v, int what, int a0, int a1, const void *ptr) {
    if (v->cb) v->cb(v->ud, what, a0, a1, ptr);
}

#define FMT_ERR(sz) ((sz) > 0 ? errbuf : NULL), (sz)

apg_vnc *apg_vnc_new(const char *host, uint16_t port, const char *password) {
    apg_vnc *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    snprintf(v->host, sizeof(v->host), "%s", host ? host : "127.0.0.1");
    v->port = port;
    if (password) snprintf(v->password, sizeof(v->password), "%s", password);
    v->sock = -1;
    v->width = 0;
    v->height = 0;
    return v;
}

void apg_vnc_destroy(apg_vnc *v) {
    if (!v) return;
    if (v->sock >= 0) close(v->sock);
    free(v->buf);
    free(v);
}

int apg_vnc_fd(apg_vnc *v) { return v ? v->sock : -1; }
int apg_vnc_width(apg_vnc *v) { return v ? (int)v->width : 0; }
int apg_vnc_height(apg_vnc *v) { return v ? (int)v->height : 0; }

/* ---- helpers RFB ---- */
static int send_all(int fd, const uint8_t *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, data + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int vnc_write(apg_vnc *v, const uint8_t *data, size_t n) {
    return send_all(v->sock, data, n);
}

static int vnc_read_n_blocking(apg_vnc *v, size_t n, int timeout_ms, char *err, size_t errsz) {
    /* usa poll + recv até acumular n bytes */
    while (v->buflen < n) {
        struct pollfd p = {.fd = v->sock, .events = POLLIN};
        int pr = poll(&p, 1, timeout_ms);
        if (pr == 0) {
            if (errsz) snprintf(err, errsz, "read timeout");
            return -1;
        }
        if (pr < 0) {
            if (errsz) snprintf(err, errsz, "poll: %s", strerror(errno));
            return -1;
        }
        if (p.revents & (POLLERR | POLLHUP)) {
            if (errsz) snprintf(err, errsz, "connection closed during handshake");
            return -1;
        }
        ssize_t r = sock_recv(v);
        if (r == 0) {
            if (errsz) snprintf(err, errsz, "connection closed by peer");
            return -1;
        }
        if (r < 0) {
            if (errsz) snprintf(err, errsz, "recv: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* ---- criptografia VNC password: DES-ECB com bits invertidos por byte ----
 * Implementação DES em C puro (tabelas padrão FIPS 46-3), sem dependências
 * externas. Só precisamos criptografar 2 blocos de 8 bytes por conexão. */

static void reverse_bits8(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = src[i], r = 0;
        for (int k = 0; k < 8; k++) {
            r = (uint8_t)((r << 1) | (b & 1));
            b >>= 1;
        }
        dst[i] = r;
    }
}

static const uint8_t DES_IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t DES_FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};
static const uint8_t DES_PC1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};
static const uint8_t DES_PC2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
    23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t DES_SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2,
                                       1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t DES_E[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
    8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t DES_P[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};
static const uint8_t DES_S[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
     0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
     15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
     3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
     13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
     13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
     1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
     13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
     3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
     14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
     11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
     10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
     4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
     13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
     6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
     1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
     2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}};

typedef uint64_t U64;

static int des_get_bit(const uint8_t *bits, int pos) {
    pos--; /* posições são 1..64 */
    return (bits[pos / 8] >> (7 - (pos % 8))) & 1;
}

static U64 des_f(U64 r, const uint8_t ks[8]) {
    /* expanded = E(r) XOR subchave: fluxo de 48 bits, MSB primeiro. */
    uint8_t ex[6] = {0};
    for (int i = 0; i < 48; i++) {
        int rbit = (int)((r >> (31 - (DES_E[i] - 1))) & 1);
        int kbit = (ks[i / 8] >> (7 - (i % 8))) & 1;
        if (rbit ^ kbit) ex[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
    }

    U64 out = 0;
    for (int s = 0; s < 8; s++) {
        int b6 = 0;
        for (int i = 0; i < 6; i++) {
            int p = 6 * s + i; /* 0-based no fluxo de 48 bits */
            b6 = (b6 << 1) | ((ex[p / 8] >> (7 - (p % 8))) & 1);
        }
        int row = ((b6 & 0x20) >> 4) | (b6 & 1);
        int col = (b6 >> 1) & 0x0f;
        out = (out << 4) | (DES_S[s][row * 16 + col] & 0x0f);
    }
    /* P permutation, out tem 32 bits */
    uint8_t p_in[4];
    for (int i = 0; i < 4; i++) p_in[i] = (uint8_t)(out >> (24 - i * 8));
    uint8_t p_out[4] = {0, 0, 0, 0};
    for (int i = 0; i < 32; i++) {
        if (des_get_bit(p_in, DES_P[i])) p_out[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
    }
    out = 0;
    for (int i = 0; i < 4; i++) out = (out << 8) | p_out[i];
    return out;
}

/* DES-ECB de um bloco de 64 bits. */
static void des_ecb_encrypt_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
    uint8_t ip[8] = {0};
    for (int i = 0; i < 64; i++) {
        if (des_get_bit(in, DES_IP[i])) ip[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
    }

    uint8_t subkeys[16][8];
    uint8_t c[4], d[4];
    uint8_t cd[7] = {0};
    for (int i = 0; i < 56; i++) {
        if (des_get_bit(key, DES_PC1[i])) cd[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
    }
    /* extrai C (28 bits) e D (28 bits) de cd */
    for (int i = 0; i < 28; i++) {
        int bit = (cd[i / 8] >> (7 - (i % 8))) & 1;
        if (bit) c[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        else c[i / 8] &= (uint8_t)~(1u << (7 - (i % 8)));
    }
    for (int i = 0; i < 28; i++) {
        int bit = (cd[(i + 28) / 8] >> (7 - ((i + 28) % 8))) & 1;
        if (bit) d[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        else d[i / 8] &= (uint8_t)~(1u << (7 - (i % 8)));
    }

    int cbits[28], dbits[28];
    for (int i = 0; i < 28; i++) {
        cbits[i] = (c[i / 8] >> (7 - (i % 8))) & 1;
        dbits[i] = (d[i / 8] >> (7 - (i % 8))) & 1;
    }

    for (int round = 0; round < 16; round++) {
        int sh = DES_SHIFTS[round];
        for (int s = 0; s < sh; s++) {
            int ct = cbits[0];
            int dt = dbits[0];
            for (int i = 0; i < 27; i++) {
                cbits[i] = cbits[i + 1];
                dbits[i] = dbits[i + 1];
            }
            cbits[27] = ct;
            dbits[27] = dt;
        }
        /* PC2 em C||D */
        uint8_t cd2[7] = {0};
        for (int i = 0; i < 28; i++) {
            if (cbits[i]) cd2[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        }
        for (int i = 0; i < 28; i++) {
            if (dbits[i]) cd2[(i + 28) / 8] |= (uint8_t)(1u << (7 - ((i + 28) % 8)));
        }
        uint8_t ks[8] = {0};
        for (int i = 0; i < 48; i++) {
            if (des_get_bit(cd2, DES_PC2[i])) ks[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        }
        memcpy(subkeys[round], ks, 8);
    }

    U64 L = 0, R = 0;
    for (int i = 0; i < 4; i++) {
        L = (L << 8) | ip[i];
        R = (R << 8) | ip[i + 4];
    }
    for (int round = 0; round < 16; round++) {
        U64 oldL = L;
        L = R;
        R = oldL ^ des_f(R, subkeys[round]);
    }
    U64 preoutput = (R << 32) | (L & 0xffffffff);

    uint8_t po[8];
    for (int i = 0; i < 8; i++) po[i] = (uint8_t)(preoutput >> (56 - i * 8));
    uint8_t fp[8] = {0};
    for (int i = 0; i < 64; i++) {
        if (des_get_bit(po, DES_FP[i])) fp[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
    }
    memcpy(out, fp, 8);
}

/* ---- pixel format: 32bpp BGRA, big-endian, true colour ---- */
static uint8_t bgra_pixel_format[16] = {
    32, /* bpp */
    24, /* depth */
    0,  /* big-endian */
    1,  /* true colour */
    0xff, 0x00, 0xff, 0x00, 0xff, 0x00, /* red/green/blue max (16-bit BE) */
    16, 8, 0,                           /* shifts */
};

/* ---- estados da negociação ---- */

/* security result: 4 bytes; 0 = ok */
static int sec_result(apg_vnc *v, char *err, size_t errsz) {
    uint32_t r;
    if (vnc_read_n_blocking(v, 4, 8000, err, errsz) != 0) return -1;
    if (rd_u32be(v, &r) != 0) {
        if (errsz) snprintf(err, errsz, "short security result");
        return -1;
    }
    if (r != 0) {
        if (errsz) snprintf(err, errsz, "VNC auth falhou (result=0x%08x)", r);
        return -1;
    }
    return 0;
}

static int handshake(apg_vnc *v, char *err, size_t errsz) {
    /* greeting */
    if (vnc_read_n_blocking(v, 12, 8000, err, errsz) != 0) return -1;
    if (memcmp(v->buf, "RFB 003.008", 11) != 0) {
        if (errsz) snprintf(err, errsz, "versão RFB não suportada: %.12s", v->buf);
        return -1;
    }
    buf_consume(v, 12);
    static const char ver[] = "RFB 003.008\n";
    if (vnc_write(v, (const uint8_t *)ver, 12) != 0) return -1;

    /* security types */
    if (vnc_read_n_blocking(v, 1, 8000, err, errsz) != 0) return -1;
    uint8_t count;
    rd_u8(v, &count);
    if (vnc_read_n_blocking(v, count, 8000, err, errsz) != 0) return -1;

    int have_none = 0, have_vnc = 0, have_ra2 = 0;
    for (int i = 0; i < count; i++) {
        if (v->buf[i] == SEC_NONE) have_none = 1;
        if (v->buf[i] == SEC_VNC) have_vnc = 1;
        if (v->buf[i] == SEC_RA2NE) have_ra2 = 1;
    }
    buf_consume(v, count);

    if (have_none) {
        uint8_t sel = SEC_NONE;
        if (vnc_write(v, &sel, 1) != 0) return -1;
        if (sec_result(v, err, errsz) != 0) return -1;
    } else if (have_vnc) {
        uint8_t sel = SEC_VNC;
        if (vnc_write(v, &sel, 1) != 0) return -1;
        if (vnc_read_n_blocking(v, 16, 8000, err, errsz) != 0) return -1;
        uint8_t challenge[16];
        memcpy(challenge, v->buf, 16);
        buf_consume(v, 16);

        uint8_t pw[8] = {0};
        size_t pl = strlen(v->password);
        memcpy(pw, v->password, pl > 8 ? 8 : pl);
        reverse_bits8(pw, pw, 8);

        uint8_t resp[16];
        des_ecb_encrypt_block(pw, challenge, resp);
        des_ecb_encrypt_block(pw, challenge + 8, resp + 8);
        if (vnc_write(v, resp, 16) != 0) return -1;
        if (sec_result(v, err, errsz) != 0) return -1;
    } else if (have_ra2) {
        uint8_t sel = 0;
        if (vnc_write(v, &sel, 1) != 0) return -1;
        if (sec_result(v, err, errsz) != 0) return -1;
    } else {
        if (errsz) snprintf(err, errsz, "sem tipo de segurança VNC utilizável");
        return -1;
    }

    /* ClientInit — precisa vir ANTES do ServerInit. */
    uint8_t shared = 0;
    if (vnc_write(v, &shared, 1) != 0) return -1;

    /* ServerInit: w h pixelformat namelen name */
    if (vnc_read_n_blocking(v, 2 + 2 + 16 + 4, 8000, err, errsz) != 0) return -1;
    uint16_t w, h;
    rd_u16be(v, &w);
    rd_u16be(v, &h);
    buf_consume(v, 16); /* pixel format ignorado */
    uint32_t namelen;
    rd_u32be(v, &namelen);
    if (namelen > (1u << 20)) {
        if (errsz) snprintf(err, errsz, "ServerInit name impossivel (%u)", namelen);
        return -1;
    }
    if (vnc_read_n_blocking(v, namelen, 8000, err, errsz) != 0) return -1;
    buf_consume(v, namelen);

    v->width = w;
    v->height = h;
    v->handshake_done = 1;

    /* pixel format + encodings */
    uint8_t pf[20];
    memset(pf, 0, sizeof(pf));
    memcpy(pf + 4, bgra_pixel_format, 16);
    if (vnc_write(v, pf, 20) != 0) return -1;

    uint8_t enc[4 + 4 * 4];
    memset(enc, 0, sizeof(enc));
    enc[0] = MSG_SET_ENCODINGS;
    enc[2] = 0;
    enc[3] = 4;
    int32_t list[4] = {ENC_RAW, ENC_DESKTOP_SIZE, ENC_POINTER_TYPE_CHANGE, ENC_LED_STATE};
    memcpy(enc + 4, list, 16); /* big-endian já está no host (aarch64 LE) -> precisa BE */

    /* lista de encodings é big-endian; converte manualmente */
    for (int i = 0; i < 4; i++) {
        uint32_t e = (uint32_t)list[i];
        enc[4 + i * 4 + 0] = (uint8_t)(e >> 24);
        enc[4 + i * 4 + 1] = (uint8_t)(e >> 16);
        enc[4 + i * 4 + 2] = (uint8_t)(e >> 8);
        enc[4 + i * 4 + 3] = (uint8_t)(e);
    }
    if (vnc_write(v, enc, sizeof(enc)) != 0) return -1;

    ev(v, APG_VNC_EV_RESIZE, w, h, NULL);
    apg_vnc_update_request(v, 0, 0, 0, w, h);
    return 0;
}

int apg_vnc_connect(apg_vnc *v, int timeout_ms, char *err, size_t errsz) {
    (void)timeout_ms;
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)v->port);
    char errb[64] = {0};
    if (getaddrinfo(v->host, portstr, &hints, &ai) != 0) {
        if (errsz) snprintf(err, errsz, "getaddrinfo(%s): %s", v->host, gai_strerror(errno));
        return -1;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(ai);
        if (errsz) snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    v->sock = fd;
    set_tcp_nodelay(fd);

    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (errsz) snprintf(err, errsz, "connect(%s:%u): %s", v->host, (unsigned)v->port,
                            strerror(errno));
        freeaddrinfo(ai);
        close(fd);
        v->sock = -1;
        return -1;
    }
    freeaddrinfo(ai);

    if (handshake(v, errb, sizeof(errb)) != 0) {
        if (errsz) snprintf(err, errsz, "%s", errb);
        ev(v, APG_VNC_EV_ERROR, 0, 0, errb);
        close(fd);
        v->sock = -1;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ fluxo */

static void set_error(apg_vnc *v, const char *msg) {
    ev(v, APG_VNC_EV_ERROR, 0, 0, msg);
}

/* Processa um bloco de atualização de framebuffer completo. */
static int consume_fb_update(apg_vnc *v) {
    /* header: padding(1) nrects(2) padding(1) = 4 bytes */
    if (v->buflen < 4) return -1;
    uint16_t nrects = (uint16_t)((v->buf[2] << 8) | v->buf[3]);
    size_t head = 4 + (size_t)nrects * 12;
    if (v->buflen < head) return -1;

    /* modo STRICT: copiamos cabeçalhos, consumimos e, se os dados de um
     * retângulo RAW estiverem incompletos, RESTAURAMOS o buffer intacto. */
    uint8_t *saved = NULL;
    size_t savedcap = 0;
    if (v->buflen > 0) {
        saved = malloc(v->buflen);
        if (!saved) return -1;
        memcpy(saved, v->buf, v->buflen);
        savedcap = v->buflen;
    }

    buf_consume(v, head);

    for (uint16_t i = 0; i < nrects; i++) {
        /* os cabeçalhos ficaram no buffer; mas já consumimos... precisamos ler
         * do início do que sobrou — os cab. estão em saved[i*12+4..] */
        size_t off = 4 + (size_t)i * 12;
        uint16_t x = (uint16_t)((saved[off + 0] << 8) | saved[off + 1]);
        uint16_t y = (uint16_t)((saved[off + 2] << 8) | saved[off + 3]);
        uint16_t w = (uint16_t)((saved[off + 4] << 8) | saved[off + 5]);
        uint16_t h = (uint16_t)((saved[off + 6] << 8) | saved[off + 7]);
        int32_t enc = (int32_t)(((uint32_t)saved[off + 8] << 24) | ((uint32_t)saved[off + 9] << 16) |
                                ((uint32_t)saved[off + 10] << 8) | (uint32_t)saved[off + 11]);

        if (enc == ENC_DESKTOP_SIZE) {
            v->width = w;
            v->height = h;
            ev(v, APG_VNC_EV_RESIZE, w, h, NULL);
            continue;
        }
        if (enc == ENC_POINTER_TYPE_CHANGE) {
            v->absolute_pointer = (x != 0);
            ev(v, APG_VNC_EV_POINTER_MODE, v->absolute_pointer, 0, NULL);
            continue;
        }
        if (enc == ENC_LED_STATE) {
            if (v->buflen < 1) goto incomplete;
            ev(v, APG_VNC_EV_LEDS, v->buf[0], 0, NULL);
            buf_consume(v, 1);
            continue;
        }
        if (enc == ENC_COPY_RECT) {
            if (v->buflen < 4) goto incomplete;
            uint16_t sx = (uint16_t)((v->buf[0] << 8) | v->buf[1]);
            uint16_t sy = (uint16_t)((v->buf[2] << 8) | v->buf[3]);
            buf_consume(v, 4);
            apg_vnc_copyrect cr = {x, y, w, h, sx, sy};
            ev(v, APG_VNC_EV_COPYRECT, 0, 0, &cr);
            continue;
        }
        if (enc != ENC_RAW) {
            char m[64];
            snprintf(m, sizeof(m), "unsupported rect encoding %d", enc);
            set_error(v, m);
            /* não consome dados; segue */
            continue;
        }
        size_t size = (size_t)w * h * 4;
        if (v->buflen < size) goto incomplete;
        apg_vnc_rect r;
        r.head.x = x;
        r.head.y = y;
        r.head.w = w;
        r.head.h = h;
        r.head.encoding = ENC_RAW;
        r.data = v->buf;
        r.sizedata = (uint32_t)size;
        ev(v, APG_VNC_EV_RECT, 0, 0, &r);
        buf_consume(v, size);
    }

    free(saved);
    return 0;

incomplete:
    /* restaura o buffer completo para processar quando chegarem os dados */
    if (saved) {
        memcpy(v->buf, saved, savedcap);
        v->buflen = savedcap;
        free(saved);
    }
    return -1;
}

int apg_vnc_consume(apg_vnc *v) {
    if (!v->handshake_done) return 0;
    int eof = 0;
    ssize_t r = sock_recv(v);
    if (r == 0) eof = 1;
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) r = 0;
        else {
            ev(v, APG_VNC_EV_CLOSED, 0, 0, NULL);
            return 0;
        }
    }

    for (;;) {
        if (v->buflen == 0) break;

        if (v->buf[0] == MSG_SRV_FB_UPDATE) {
            if (consume_fb_update(v) != 0) break; /* precisa mais dados */
            apg_vnc_update_request(v, 1, 0, 0, v->width, v->height);
            continue;
        }
        if (v->buf[0] == MSG_SRV_BELL) {
            if (v->buflen < 2) break;
            buf_consume(v, 2);
            ev(v, APG_VNC_EV_BELL, 0, 0, NULL);
            continue;
        }
        if (v->buf[0] == MSG_SRV_CUT_TEXT) {
            if (v->buflen < 8) break;
            uint32_t len = ((uint32_t)v->buf[4] << 24) | ((uint32_t)v->buf[5] << 16) |
                           ((uint32_t)v->buf[6] << 8) | v->buf[7];
            if (v->buflen < 8 + len) break;
            /* null-terminated copy */
            char *txt = malloc((size_t)len + 1);
            if (!txt) break;
            memcpy(txt, v->buf + 8, len);
            txt[len] = 0;
            buf_consume(v, 8 + len);
            size_t l = strlen(txt);
            while (l > 0 && txt[l - 1] == 0) txt[--l] = 0;
            ev(v, APG_VNC_EV_CUTTEXT, 0, 0, txt);
            free(txt);
            continue;
        }
        /* mensagem desconhecida: descarta 1 byte para evitar loop */
        buf_consume(v, 1);
    }
    if (eof && v->buflen == 0) {
        ev(v, APG_VNC_EV_CLOSED, 0, 0, NULL);
    }
    return 0;
}

void apg_vnc_key(apg_vnc *v, uint32_t keysym, int down) {
    uint8_t msg[8];
    msg[0] = MSG_KEY_EVENT;
    msg[1] = down ? 1 : 0;
    msg[2] = 0;
    msg[3] = 0;
    msg[4] = (uint8_t)(keysym >> 24);
    msg[5] = (uint8_t)(keysym >> 16);
    msg[6] = (uint8_t)(keysym >> 8);
    msg[7] = (uint8_t)(keysym);
    vnc_write(v, msg, sizeof(msg));
}

static uint16_t clamp16(int v) {
    if (v < 0) return 0;
    if (v > 0xffff) return 0xffff;
    return (uint16_t)v;
}

void apg_vnc_pointer(apg_vnc *v, int button_mask, int x, int y) {
    uint8_t msg[6];
    msg[0] = MSG_POINTER_EVENT;
    msg[1] = (uint8_t)(button_mask & 0xff);
    if (v->absolute_pointer) {
        uint16_t px = clamp16(x);
        uint16_t py = clamp16(y);
        msg[2] = (uint8_t)(px >> 8);
        msg[3] = (uint8_t)(px);
        msg[4] = (uint8_t)(py >> 8);
        msg[5] = (uint8_t)(py);
    } else {
        uint16_t px = clamp16(x + 0x7fff);
        uint16_t py = clamp16(y + 0x7fff);
        msg[2] = (uint8_t)(px >> 8);
        msg[3] = (uint8_t)(px);
        msg[4] = (uint8_t)(py >> 8);
        msg[5] = (uint8_t)(py);
    }
    vnc_write(v, msg, sizeof(msg));
}

void apg_vnc_cuttext(apg_vnc *v, const char *text) {
    size_t n = strlen(text);
    uint8_t *msg = malloc(8 + n);
    if (!msg) return;
    msg[0] = MSG_CLIENT_CUT_TEXT;
    msg[1] = msg[2] = msg[3] = 0;
    msg[4] = (uint8_t)(n >> 24);
    msg[5] = (uint8_t)(n >> 16);
    msg[6] = (uint8_t)(n >> 8);
    msg[7] = (uint8_t)(n);
    memcpy(msg + 8, text, n);
    vnc_write(v, msg, 8 + n);
    free(msg);
}

void apg_vnc_update_request(apg_vnc *v, int incremental, int x, int y, int w, int h) {
    if (w <= 0) w = v->width;
    if (h <= 0) h = v->height;
    uint8_t msg[10];
    msg[0] = MSG_FB_UPDATE_REQ;
    msg[1] = incremental ? 1 : 0;
    msg[2] = (uint8_t)(x >> 8);
    msg[3] = (uint8_t)(x);
    msg[4] = (uint8_t)(y >> 8);
    msg[5] = (uint8_t)(y);
    msg[6] = (uint8_t)(w >> 8);
    msg[7] = (uint8_t)(w);
    msg[8] = (uint8_t)(h >> 8);
    msg[9] = (uint8_t)(h);
    vnc_write(v, msg, sizeof(msg));
}