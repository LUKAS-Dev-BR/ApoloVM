/* apolovm-main/tests/c/neon_test.c
 * Teste da rotina Assembly NEON BGRA->RGBA em vários tamanhos, incluindo o
 * resto (tamanhos não múltiplos de 16 pixels).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void apg_bgra_to_rgba_neon(uint8_t *dst, const uint8_t *src, size_t npix);

static int check_bulk(size_t npix) {
    uint8_t *src = malloc(npix * 4);
    uint8_t *dst = malloc(npix * 4);
    if (!src || !dst) return 1;
    uint32_t seed = 0x12345678;
    for (size_t i = 0; i < npix * 4; i++) {
        seed = seed * 1103515245 + 12345;
        src[i] = (uint8_t)(seed >> 24);
    }
    apg_bgra_to_rgba_neon(dst, src, npix);
    size_t bad = 0;
    for (size_t p = 0; p < npix; p++) {
        if (dst[p * 4] != src[p * 4 + 2] || dst[p * 4 + 1] != src[p * 4 + 1] ||
            dst[p * 4 + 2] != src[p * 4] || dst[p * 4 + 3] != src[p * 4 + 3]) {
            bad++;
        }
    }
    free(src);
    free(dst);
    return (int)(bad != 0);
}

int main(void) {
    const size_t sizes[] = {1, 2, 7, 15, 16, 17, 31, 64, 65, 128, 257, 4096, 8193};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (check_bulk(sizes[i])) {
            printf("neon_test: FALHA em %zu pixels\n", sizes[i]);
            return 1;
        }
    }
    printf("neon_test: OK (13 tamanhos, incluindo rests)\n");
    return 0;
}