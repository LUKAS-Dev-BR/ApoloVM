/* apolovm-main/src/neon.h
 * Rótulos do núcleo de linha de base Assembly (AArch64 NEON).
 * Fallback escalar em C disponível quando __aarch64__ não estiver definido.
 */
#ifndef APOLOVM_NEON_H
#define APOLOVM_NEON_H

#include <stddef.h>
#include <stdint.h>

/* Projection: conversão BGRA -> RGBA (big-endless host) de [npix] pixels. */
void apg_bgra_to_rgba_neon(uint8_t *dst, const uint8_t *src, size_t npix);

#if defined(__aarch64__)
#define APG_HAVE_NEON 1
#else
#define APG_HAVE_NEON 0
#endif

#endif /* APOLOVM_NEON_H */