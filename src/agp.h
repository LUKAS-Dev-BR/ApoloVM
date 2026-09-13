/* apolovm-main/src/agp.h
 * Apolo Graphics Protocol AGP/1 - codificação/decodificação de quadros.
 * Compatível byte-a-byte com a implementação JavaScript (web/src/agp/protocol.ts).
 *
 * Framing binário (big-endian):
 *   offset  size  campo
 *   0       7     magic "APGV01\0"
 *   7       1     tipo da mensagem
 *   8       1     flags
 *   9       4     width  (u32)
 *   13      4     height (u32)
 *   17      4     comprimento do payload (u32)
 *   21      n     payload
 */
#ifndef APOLOVM_AGP_H
#define APOLOVM_AGP_H

#include <stddef.h>
#include <stdint.h>

#define AGP_MAGIC_BYTES 7
#define AGP_HEADER_SIZE 21

#define AGP_MAGIC_0 0x41 /* A */
#define AGP_MAGIC_1 0x50 /* P */
#define AGP_MAGIC_2 0x47 /* G */
#define AGP_MAGIC_3 0x56 /* V */
#define AGP_MAGIC_4 0x30 /* 0 */
#define AGP_MAGIC_5 0x31 /* 1 */
#define AGP_MAGIC_6 0x00

/* Tipos de mensagem (alinhados com AGP/1 do cliente web). */
enum agp_type {
    AGP_HELLO       = 0x01,
    AGP_FRAME       = 0x02, /* framebuffer completo */
    AGP_FRAME_DELTA = 0x03, /* retângulos sujos */
    AGP_CURSOR      = 0x04,
    AGP_KEY         = 0x05,
    AGP_MOUSE       = 0x06,
    AGP_RESIZE      = 0x07,
    AGP_CLIPBOARD   = 0x08,
    AGP_AUDIO       = 0x09,
    AGP_PING        = 0x0a,
    AGP_PONG        = 0x0b,
    AGP_ERROR       = 0x0c,
    AGP_CONSOLE     = 0x0d,
};

/* Flags */
#define AGP_FLAG_JSON     0x01 /* payload é texto UTF-8 JSON */
#define AGP_FLAG_SCANCODE 0x02
#define AGP_FLAG_BINARY   0x04
#define AGP_FLAG_MORE     0x08

/* Cabeçalho decodificado de um quadro AGP. */
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t len;
    const uint8_t *payload;
} agp_msg;

/* Tamanho total do quadro (header + payload). */
static inline size_t agp_frame_size(uint32_t payload_len) {
    return (size_t)AGP_HEADER_SIZE + payload_len;
}

/* Escreve o header AGP/1 em [out] (out deve ter >= agp_frame_size(len) bytes).
 * Retorna o offset onde o payload deve ser gravado (= AGP_HEADER_SIZE). */
static inline size_t agp_write_header(uint8_t *out, uint8_t type, uint8_t flags,
                                      uint32_t width, uint32_t height, uint32_t len) {
    out[0] = AGP_MAGIC_0; out[1] = AGP_MAGIC_1; out[2] = AGP_MAGIC_2;
    out[3] = AGP_MAGIC_3; out[4] = AGP_MAGIC_4; out[5] = AGP_MAGIC_5;
    out[6] = AGP_MAGIC_6;
    out[7] = type;
    out[8] = flags;
    out[9]  = (uint8_t)(width >> 24);
    out[10] = (uint8_t)(width >> 16);
    out[11] = (uint8_t)(width >> 8);
    out[12] = (uint8_t)(width);
    out[13] = (uint8_t)(height >> 24);
    out[14] = (uint8_t)(height >> 16);
    out[15] = (uint8_t)(height >> 8);
    out[16] = (uint8_t)(height);
    out[17] = (uint8_t)(len >> 24);
    out[18] = (uint8_t)(len >> 16);
    out[19] = (uint8_t)(len >> 8);
    out[20] = (uint8_t)(len);
    return AGP_HEADER_SIZE;
}

/* Verifica se [buf] (com [n] bytes) é um quadro AGP completo.
 * Se completo: preenche *out e retorna 1. Incompleto: 0. Inválido: -1. */
int agp_decode(const uint8_t *buf, size_t n, agp_msg *out);

#endif /* APOLOVM_AGP_H */