/*
 * upscale — source-art → 4K-aspect-corrected RGBA upscaler.
 *
 * Two source modes, same target (2880 × 2160, the 4:3 region of a
 * 16:9 4K viewport). Both follow the same "NN expand to preserve
 * hard edges, then Lanczos for the fractional residue" philosophy:
 *
 *   VGA (320×200, mode-13h pixel aspect 1:1.2):
 *     1. Horizontal NN expand 9× — 320 × 9 = 2880.
 *     2. Vertical Lanczos-3 at ratio 54/5 (= 10.8). Aspect
 *        correction 10.8 / 9 = 6/5 = 1.2 — exact VGA pixel aspect
 *        on a 4:3 CRT.
 *
 *   SVGA (640×480, square pixels):
 *     1. NN expand 5× in both axes — 640×480 → 3200×2400.
 *     2. Lanczos-3 downscale to 2880×2160 (ratio 0.9 in both axes).
 *     Net 4.5× isotropic with the integer NN preserving sprite-edge
 *     fidelity that a direct Lanczos upsample would soften.
 *
 * Lanczos runs in premultiplied-alpha space so transparent texels
 * contribute 0 to RGB averages at edges (standard correctness rule).
 * Output is un-PMA'd before return so the buffer stays straight-alpha
 * (PNG-compatible).
 */
#ifndef FILM_UPSCALE_H
#define FILM_UPSCALE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* VGA → 4K. On success replaces *inout_pixels with a fresh malloc'd
 * buffer (the old one is freed) and updates *inout_w / *inout_h to
 *   out_w = in_w * 9, out_h = round(in_h * 10.8).
 * Returns false on allocation failure; original buffer untouched. */
bool atlas_vga_to_4k(uint8_t **inout_pixels, int *inout_w, int *inout_h);

/* SVGA → 4K. Same contract as atlas_vga_to_4k. Output dims:
 *   out_w = round(in_w * 4.5), out_h = round(in_h * 4.5).
 * For the canonical 640×480 source this lands at 2880×2160. */
bool atlas_svga_to_4k(uint8_t **inout_pixels, int *inout_w, int *inout_h);

/* Y-axis scale for VGA (10.8×). Identity-stable: scale_y_4k(0) == 0,
 * scale_y_4k(in_h) matches the height atlas_vga_to_4k returns.
 * Round-to-nearest, NOT additive — `scale_y_4k(top) + scale_y_4k(h)`
 * may differ from `scale_y_4k(top + h)` by 1 px. Sub-rect callers
 * must scale corner coordinates, not (top, h) independently. */
int scale_y_4k(int v);

/* Both-axis scale for SVGA (4.5×). Same non-additive caveat — call
 * on corner coords, not on (origin, size) separately. */
int scale_svga_xy_to_4k(int v);

/* X scale factor for VGA. Constant; exposed for symmetry with the
 * scale_*-style helpers above. */
#define SCALE_X_4K 9

#ifdef __cplusplus
}
#endif

#endif
