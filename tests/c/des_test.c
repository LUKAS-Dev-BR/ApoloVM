/* apolovm-main/tests/c/des_test.c
 * Vetores de teste FIPS 46-3 para o DES embutido do cliente VNC.
 * Compila o vnc.c na mesma TU para acessar a função static.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../../src/vnc.c"

static int check(const char *name, const uint8_t key[8], const uint8_t in[8],
                 const uint8_t expect[8]) {
    uint8_t out[8];
    des_ecb_encrypt_block(key, in, out);
    if (memcmp(out, expect, 8) != 0) {
        printf("FALHA [%s]\n  esperado: %02X%02X%02X%02X%02X%02X%02X%02X\n  obtido:   %02X%02X%02X%02X%02X%02X%02X%02X\n",
               name, expect[0], expect[1], expect[2], expect[3], expect[4], expect[5],
               expect[6], expect[7], out[0], out[1], out[2], out[3], out[4], out[5], out[6],
               out[7]);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;

    /* vetor padrão FIPS: key 133457799BBCDFF1, pt 0123456789ABCDEF -> ct 85E813540F0AB405 */
    uint8_t key1[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    uint8_t pt1[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    uint8_t ct1[8] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05};
    fails += check("fips", key1, pt1, ct1);

    /* vetor com chave zerada e bloco zerado */
    uint8_t k0[8] = {0}, p0[8] = {0};
    uint8_t c0[8] = {0x8C, 0xA6, 0x4D, 0xE9, 0xC1, 0xB1, 0x23, 0xA7};
    fails += check("zero-key", k0, p0, c0);

    /* chave 0101010101010101: todos os bits selecionados são de paridade,
     * logo PC1 produz zero => subchaves zeradas (igual à chave zero). */
    uint8_t k2[8] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    uint8_t pt2[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t ct2[8] = {0x8C, 0xA6, 0x4D, 0xE9, 0xC1, 0xB1, 0x23, 0xA7};
    fails += check("key-0101(parity)", k2, pt2, ct2);

    if (fails) {
        printf("%d vetor(es) falhou(ram)\n", fails);
        return 1;
    }
    printf("des_test: OK (3 vetores FIPS)\n");
    return 0;
}