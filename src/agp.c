/* apolovm-main/src/agp.c */
#include "agp.h"

int agp_decode(const uint8_t *buf, size_t n, agp_msg *out) {
    if (!buf || !out || n < AGP_HEADER_SIZE) return 0;

    if (buf[0] != AGP_MAGIC_0 || buf[1] != AGP_MAGIC_1 || buf[2] != AGP_MAGIC_2 ||
        buf[3] != AGP_MAGIC_3 || buf[4] != AGP_MAGIC_4 || buf[5] != AGP_MAGIC_5 ||
        buf[6] != AGP_MAGIC_6) {
        return -1; /* magic inválido */
    }

    out->type  = buf[7];
    out->flags = buf[8];
    out->width = ((uint32_t)buf[9] << 24) | ((uint32_t)buf[10] << 16) |
                 ((uint32_t)buf[11] << 8) | (uint32_t)buf[12];
    out->height = ((uint32_t)buf[13] << 24) | ((uint32_t)buf[14] << 16) |
                  ((uint32_t)buf[15] << 8) | (uint32_t)buf[16];
    out->len = ((uint32_t)buf[17] << 24) | ((uint32_t)buf[18] << 16) |
               ((uint32_t)buf[19] << 8) | (uint32_t)buf[20];

    if ((size_t)AGP_HEADER_SIZE + out->len > n) return 0; /* incompleto */
    out->payload = buf + AGP_HEADER_SIZE;
    return 1;
}