/*
 * bc5_codec — C facade over the vendored rgbcx encoder (BC1/3/4/5 by
 * Rich Geldreich, same upstream as the bc7enc we already vendor; MIT /
 * public domain dual license).
 *
 * rgbcx is C++ and exposes its API in the rgbcx:: namespace, so it
 * lives in a single C++ TU and the public header stays C. Layout
 * mirrors bc7_codec.{h,cpp}.
 *
 * BC5 path uses rgbcx::encode_bc5 with chan0=R, chan1=G, stride=4 to
 * read directly from the RGBA atlas the channel packer materializes.
 * The encoder handles 8-color / 6-color mode selection and endpoint
 * refinement internally — meaningfully higher quality than min/max +
 * snap-to-nearest, especially on smooth-gradient normal-map content.
 */

#include "rgbcx.h"

#include "bc5_codec.h"

#include <cstring>

extern "C" {

/* rgbcx requires init before any encode_* call. Idempotent under our
 * use (we only ever pass cBC1Ideal); first call from any thread of
 * aeron_gltf_cook's single-threaded cook is safe. */
static void ensure_rgbcx_initialised()
{
    static bool inited = false;
    if (!inited) {
        rgbcx::init();
        inited = true;
    }
}

void bc5_codec_encode_block_from_rgba(uint8_t out_block[16],
                                      const uint8_t in_rgba_4x4[64])
{
    ensure_rgbcx_initialised();
    /* chan0=0 (R), chan1=1 (G), stride=4 — pull from RGBA pixels. */
    rgbcx::encode_bc5(out_block, in_rgba_4x4, 0, 1, 4);
}

size_t bc5_codec_image_size(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;
    return (size_t)bx * (size_t)by * 16u;
}

size_t bc5_codec_encode_image_from_rgba(uint8_t *out_blocks, size_t out_cap,
                                        const uint8_t *rgba, int w, int h)
{
    if (!out_blocks || !rgba || w <= 0 || h <= 0) return 0;
    size_t need = bc5_codec_image_size(w, h);
    if (need > out_cap) return 0;

    ensure_rgbcx_initialised();

    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;

    /* rgbcx::encode_bc5 reads a 4×4 RGBA block from contiguous memory
     * (stride=4 between texels, no inter-row stride parameter). Build
     * each block from the source image, replicating the last in-bounds
     * row/col into out-of-bounds slots — same padding strategy as
     * bc7_codec_encode_image. */
    uint8_t block[64];
    uint8_t *dst = out_blocks;
    for (int by_i = 0; by_i < by; by_i++) {
        for (int bx_i = 0; bx_i < bx; bx_i++) {
            for (int yy = 0; yy < 4; yy++) {
                int sy = by_i * 4 + yy;
                if (sy >= h) sy = h - 1;
                for (int xx = 0; xx < 4; xx++) {
                    int sx = bx_i * 4 + xx;
                    if (sx >= w) sx = w - 1;
                    const uint8_t *src = rgba +
                        ((size_t)sy * (size_t)w + (size_t)sx) * 4u;
                    uint8_t *bp = block + (yy * 4 + xx) * 4;
                    bp[0] = src[0];
                    bp[1] = src[1];
                    bp[2] = src[2];
                    bp[3] = src[3];
                }
            }
            rgbcx::encode_bc5(dst, block, 0, 1, 4);
            dst += 16;
        }
    }
    return need;
}

}  // extern "C"
