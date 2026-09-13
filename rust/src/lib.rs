//! libapolographics — componentes de alto nível da V2 escritos em Rust.
//!
//! Compilada como staticlib e linkada no daemon `apolovm`. Provê:
//!  1. `apg_rust_encode_delta`: codifica o payload de um FRAME_DELTA do AGP/1
//!     (cabeçalho JSON dos retângulos + pixels concatenados na ordem).
//!  2. `apg_rust_swizzle_bgra_rgba`: conversão escalar BGRA→RGBA (fallback
//!     portável; em AArch64 a versão NEON assembly é prioritária).
//!
//! O formato dos retângulos (rect-by-rect, big-endian do AGP) é idêntico ao
//! da V1 (`apolo/graphics/bridge.js`).

#![allow(clippy::missing_safety_doc)]

use std::slice;

/// Retângulo de regravação {x, y, w, h}.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DirtyRect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

const MAX_HEAD_JSON: usize = 1 << 23; // 8 MiB de cabeçalho JSON

/// Codifica o payload de FRAME_DELTA. `rects` aponta para `nrects` valores
/// u32 em sequência x,y,w,h. Copia os pixels do framebuffer BGRA na ordem dos
/// retângulos, sem swizzle (compatível byte-a-byte com a V1).
///
/// Retorna o tamanho escrito, ou -1 se `dstcap` for insuficiente.
#[no_mangle]
pub unsafe extern "C" fn apg_rust_encode_delta(
    dst: *mut u8,
    dstcap: usize,
    fb: *const u8,
    fw: u32,
    fh: u32,
    rects: *const u32,
    nrects: usize,
) -> i64 {
    if dst.is_null() || fb.is_null() || (rects.is_null() && nrects != 0) {
        return -1;
    }
    if fw == 0 || fh == 0 {
        return -1;
    }

    // monta a lista de retângulos em heap
    let mut rs: Vec<DirtyRect> = Vec::with_capacity(nrects);
    for i in 0..nrects {
        let b = rects.add(i * 4);
        rs.push(DirtyRect {
            x: *b,
            y: *b.add(1),
            w: *b.add(2),
            h: *b.add(3),
        });
    }

    // cabeçalho JSON de um FRAME_DELTA/1
    let mut head = String::with_capacity(64 + nrects * 64);
    head.push('[');
    for (i, r) in rs.iter().enumerate() {
        if i > 0 {
            head.push(',');
        }
        head.push_str(&format!(
            r#"{{"x":{},"y":{},"w":{},"h":{}}}"#,
            r.x, r.y, r.w, r.h
        ));
    }
    head.push_str("]\n");

    let head_bytes = head.as_bytes();
    let total = head_bytes.len() + rs.iter().map(|r| (r.w * r.h) as usize * 4).sum::<usize>();
    if total > dstcap {
        return -1;
    }

    let out = slice::from_raw_parts_mut(dst, total);
    out[..head_bytes.len()].copy_from_slice(head_bytes);

    let mut off = head_bytes.len();
    let fblen = (fw as usize) * (fh as usize) * 4;
    let src = slice::from_raw_parts(fb, fblen);
    for r in &rs {
        let npix = (r.w as usize) * (r.h as usize);
        // cópia linha a linha do retângulo do framebuffer BGRA
        for row in 0..r.h as usize {
            let src_off = ((r.y as usize + row) * fw as usize + r.x as usize) * 4;
            let dst_off = off + row * r.w as usize * 4;
            out[dst_off..dst_off + r.w as usize * 4]
                .copy_from_slice(&src[src_off..src_off + r.w as usize * 4]);
        }
        off += npix * 4;
    }

    total as i64
}

/// Conversão escalar BGRA→RGBA de `npix` pixels (fallback portável).
#[no_mangle]
pub unsafe extern "C" fn apg_rust_swizzle_bgra_rgba(
    dst: *mut u8,
    src: *const u8,
    npix: usize,
) {
    let d = slice::from_raw_parts_mut(dst, npix * 4);
    let s = slice::from_raw_parts(src, npix * 4);
    for i in 0..npix {
        let b = s[i * 4];
        let g = s[i * 4 + 1];
        let r = s[i * 4 + 2];
        let a = s[i * 4 + 3];
        d[i * 4] = r;
        d[i * 4 + 1] = g;
        d[i * 4 + 2] = b;
        d[i * 4 + 3] = a;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn swizzle_fallback() {
        let mut src = vec![0u8; 8];
        src.copy_from_slice(&[9, 8, 7, 6, 1, 2, 3, 4]);
        let mut dst = vec![0u8; 8];
        unsafe { apg_rust_swizzle_bgra_rgba(dst.as_mut_ptr(), src.as_ptr(), 2) };
        assert_eq!(dst, vec![7, 8, 9, 6, 3, 2, 1, 4]);
    }

    #[test]
    fn delta_json_layout() {
        let mut fb = vec![0u8; 16 * 10];
        for (i, b) in fb.iter_mut().enumerate() {
            *b = (i % 251) as u8;
        }
        let rects: Vec<u32> = vec![1, 2, 3, 4, 9, 0, 1, 1];
        let mut out = vec![0u8; 4096];
        let n = unsafe { apg_rust_encode_delta(out.as_mut_ptr(), out.len(), fb.as_ptr(), 16, 10, rects.as_ptr(), 2) };
        assert!(n > 0, "n={}", n);
        let head = String::from_utf8_lossy(&out[..80]);
        assert!(head.starts_with(r#"[{"x":1,"y":2,"w":3,"h":4},"#));
        assert!(head.contains(r#"{"x":9,"y":0,"w":1,"h":1}]"#));
        // len do payload = headers + 12*4 + 1*4
        let hlen = head.find("]\n").unwrap() + 2;
        assert_eq!(n as usize, hlen + 12 * 4 + 1 * 4);
    }
}