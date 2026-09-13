/* apolovm-main/asm/bgra_rgba_a64.S
 * Rotina NEON AArch64: converte um buffer de pixels BGRA (32bpp, memória
 * little-endian) para RGBA trocando os bytes 0 e 2 de cada pixel.
 *
 *   void apg_bgra_to_rgba_neon(uint8_t *dst, const uint8_t *src, size_t npix);
 *
 * Por lane de 32 bits (pixel B G R A): mantém G e A e desloca R para a posição
 * do B e B para a posição do R. Rodapé escalar para o resto não-divisível por
 * 16 pixels. Escrita em sintaxe GNU AS.
 */

    .text
    .globl  apg_bgra_to_rgba_neon
    .p2align 3

apg_bgra_to_rgba_neon:
    /* x0 = dst, x1 = src, x2 = npix */

    /* v1 = 0xFF00FF00 (mantém G,A); v2 = 0x000000FF; v3 = 0x00FF0000 */
    mov     w3, #0xFF00
    movk    w3, #0xFF00, lsl #16
    dup     v1.4s, w3
    mov     w4, #0xFF
    dup     v2.4s, w4
    dup     v3.4s, w4
    shl     v3.4s, v3.4s, #16

    lsr     x3, x2, #4            /* quantos blocos de 16 pixels */
    cbz     x3, 2f

1:  /* 64 pixels por volta: 4 vetores de 16 pixels */
    ld1     {v0.16b}, [x1], #16
    ld1     {v4.16b}, [x1], #16
    ld1     {v5.16b}, [x1], #16
    ld1     {v6.16b}, [x1], #16

    ushr    v8.4s, v0.4s, #16
    and     v8.16b, v8.16b, v2.16b
    shl     v20.4s, v0.4s, #16
    and     v20.16b, v20.16b, v3.16b
    and     v0.16b, v0.16b, v1.16b
    orr     v0.16b, v0.16b, v8.16b
    orr     v0.16b, v0.16b, v20.16b

    ushr    v8.4s, v4.4s, #16
    and     v8.16b, v8.16b, v2.16b
    shl     v20.4s, v4.4s, #16
    and     v20.16b, v20.16b, v3.16b
    and     v4.16b, v4.16b, v1.16b
    orr     v4.16b, v4.16b, v8.16b
    orr     v4.16b, v4.16b, v20.16b

    ushr    v8.4s, v5.4s, #16
    and     v8.16b, v8.16b, v2.16b
    shl     v20.4s, v5.4s, #16
    and     v20.16b, v20.16b, v3.16b
    and     v5.16b, v5.16b, v1.16b
    orr     v5.16b, v5.16b, v8.16b
    orr     v5.16b, v5.16b, v20.16b

    ushr    v8.4s, v6.4s, #16
    and     v8.16b, v8.16b, v2.16b
    shl     v20.4s, v6.4s, #16
    and     v20.16b, v20.16b, v3.16b
    and     v6.16b, v6.16b, v1.16b
    orr     v6.16b, v6.16b, v8.16b
    orr     v6.16b, v6.16b, v20.16b

    st1     {v0.16b}, [x0], #16
    st1     {v4.16b}, [x0], #16
    st1     {v5.16b}, [x0], #16
    st1     {v6.16b}, [x0], #16

    subs    x3, x3, #1
    b.ne    1b

2:  /* resto: npix & 15 */
    and     x3, x2, #15
    cbz     x3, 3f
    lsl     x4, x3, #2            /* bytes a processar */
2:
    ldrb    w5, [x1]
    ldrb    w6, [x1, #2]
    ldrb    w7, [x1, #1]
    ldrb    w8, [x1, #3]
    strb    w6, [x0]
    strb    w7, [x0, #1]
    strb    w5, [x0, #2]
    strb    w8, [x0, #3]
    add     x1, x1, #4
    add     x0, x0, #4
    subs    x4, x4, #4
    b.ne    2b

3:
    ret

    .size   apg_bgra_to_rgba_neon, .-apg_bgra_to_rgba_neon

    .section .note.GNU-stack,"",%progbits