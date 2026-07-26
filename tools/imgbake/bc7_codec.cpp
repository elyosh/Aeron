/*
 * bc7_codec — implementation. Wraps richgel999/bc7enc.
 *
 * Quality presets map onto bc7enc's two main knobs:
 *   - m_max_partitions (0 disables mode 1; 64 is the full search space)
 *   - m_uber_level     (0..4; higher = slower / better)
 *
 * The settings below were calibrated against the README's published
 * numbers — see https://github.com/richgel999/bc7enc_rdo for the full
 * speed/PSNR ladder and define the "fast / med / uber" presets.
 */

#include "bc7_codec.h"

extern "C" {
/* The vendored header lacks an include guard; including it from
 * multiple TUs would cause re-definition errors. Confined to this
 * single C++ TU. */
}
#include "third_party/bc7enc/bc7enc.h"

#include <mutex>

static std::once_flag s_init_flag;

extern "C" void bc7_codec_init(void)
{
    std::call_once(s_init_flag, []() { bc7enc_compress_block_init(); });
}

/* Translate Bc7Quality → bc7enc params. Caller must have called
 * bc7_codec_init() first; we don't pay for the once-flag check on
 * every block. */
static void params_for(bc7enc_compress_block_params *p, Bc7Quality q)
{
    bc7enc_compress_block_params_init(p);
    bc7enc_compress_block_params_init_perceptual_weights(p);
    switch (q) {
    case BC7_QUALITY_FAST:
        p->m_max_partitions = 16;
        p->m_uber_level     = 0;
        p->m_try_least_squares = false;
        break;
    case BC7_QUALITY_MED:
        p->m_max_partitions = 64;
        p->m_uber_level     = 1;
        break;
    case BC7_QUALITY_UBER:
        p->m_max_partitions = 64;
        p->m_uber_level     = BC7ENC_MAX_UBER_LEVEL;
        break;
    }
}

extern "C"
void bc7_codec_encode_block(uint8_t out_block[16],
                            const uint8_t in_rgba_4x4[64],
                            Bc7Quality quality)
{
    bc7enc_compress_block_params params;
    params_for(&params, quality);
    bc7enc_compress_block(out_block, in_rgba_4x4, &params);
}

extern "C"
size_t bc7_codec_image_size(int w, int h)
{
    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;
    return (size_t)bx * (size_t)by * 16u;
}

/* Edge-padding 4×4 staging block. Rows shorter than 4 px or with
 * fewer than 4 cols clamp to the last available pixel — same
 * convention the bc7enc reference encoder uses and what every BC7
 * codec assumes for non-multiple-of-4 dims. */
static void copy_4x4_padded(uint8_t out_block[64],
                            const uint8_t *rgba, int w, int h,
                            int bx, int by)
{
    for (int y = 0; y < 4; ++y) {
        int sy = by * 4 + y;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < 4; ++x) {
            int sx = bx * 4 + x;
            if (sx >= w) sx = w - 1;
            const uint8_t *src = rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4u;
            uint8_t *dst = out_block + (y * 4 + x) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
}

extern "C"
size_t bc7_codec_encode_image(uint8_t *out_blocks, size_t out_cap,
                              const uint8_t *rgba, int w, int h,
                              Bc7Quality quality)
{
    if (!out_blocks || !rgba || w < 1 || h < 1) return 0;
    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;
    size_t need = (size_t)bx * (size_t)by * 16u;
    if (out_cap < need) return 0;

    /* Build params once, reuse per block — bc7enc doesn't mutate the
     * struct, and this avoids re-running the perceptual-weights
     * helper on every block. */
    bc7enc_compress_block_params params;
    params_for(&params, quality);

    for (int y = 0; y < by; ++y) {
        for (int x = 0; x < bx; ++x) {
            uint8_t block_rgba[64];
            copy_4x4_padded(block_rgba, rgba, w, h, x, y);
            uint8_t *dst = out_blocks + ((size_t)y * (size_t)bx + (size_t)x) * 16u;
            bc7enc_compress_block(dst, block_rgba, &params);
        }
    }
    return need;
}
