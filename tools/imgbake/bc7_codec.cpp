/*
 * bc7_codec — implementation.
 *
 * Builds with the ISPC Texture Compressor (IMGBAKE_HAVE_ISPC) encode with
 * it: on the game's textures, its "fast" profile gives 1 to 3 dB more than
 * bc7enc's "med" preset, with 2 to 4 times less CPU time on opaque blocks.
 * Blocks with alpha use the matching alpha profile, which is slower. The
 * presets are:
 *   - fast: veryfast
 *   - med:  fast
 *   - uber: slow
 *
 * Builds without ISPC, such as the add-on's standalone cooker, use
 * richgel999/bc7enc. Its presets map onto its two main knobs:
 *   - m_max_partitions (0 disables mode 1; 64 is the full search space)
 *   - m_uber_level     (0..4; higher = slower / better)
 * The settings below were calibrated against the README's published
 * numbers — see https://github.com/richgel999/bc7enc_rdo for the full
 * speed/PSNR ladder.
 */

#include "bc7_codec.h"

extern "C" {
/* The vendored header lacks an include guard; including it from
 * multiple TUs would cause re-definition errors. Confined to this
 * single C++ TU. */
}
#include "third_party/bc7enc/bc7enc.h"
#ifdef IMGBAKE_HAVE_ISPC
#include "ispc_texcomp.h"
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static std::once_flag s_init_flag;
static std::atomic<unsigned> s_max_threads(0);

extern "C" void bc7_codec_set_max_threads(unsigned threads)
{
    s_max_threads = threads;
}

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

#ifdef IMGBAKE_HAVE_ISPC
/* The ISPC profile of `quality`, for an image with or without alpha. */
static void ispc_settings_for(bc7_enc_settings *settings, Bc7Quality quality, bool alpha)
{
    switch (quality) {
    case BC7_QUALITY_FAST:
        alpha ? GetProfile_alpha_veryfast(settings) : GetProfile_veryfast(settings);
        break;
    case BC7_QUALITY_MED:
        alpha ? GetProfile_alpha_fast(settings) : GetProfile_fast(settings);
        break;
    case BC7_QUALITY_UBER:
        alpha ? GetProfile_alpha_slow(settings) : GetProfile_slow(settings);
        break;
    }
}

static bool has_alpha(const uint8_t *rgba, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i)
        if (rgba[i * 4u + 3u] != 255u)
            return true;
    return false;
}
#endif

extern "C"
void bc7_codec_encode_block(uint8_t out_block[16],
                            const uint8_t in_rgba_4x4[64],
                            Bc7Quality quality)
{
#ifdef IMGBAKE_HAVE_ISPC
    bc7_enc_settings settings;
    ispc_settings_for(&settings, quality, has_alpha(in_rgba_4x4, 16));
    rgba_surface surface = { const_cast<uint8_t *>(in_rgba_4x4), 4, 4, 16 };
    CompressBlocksBC7(&surface, out_block, &settings);
#else
    bc7enc_compress_block_params params;
    params_for(&params, quality);
    bc7enc_compress_block(out_block, in_rgba_4x4, &params);
#endif
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

#ifdef IMGBAKE_HAVE_ISPC
    bc7_enc_settings opaque_settings, alpha_settings;
    ispc_settings_for(&opaque_settings, quality, false);
    ispc_settings_for(&alpha_settings, quality, true);
#else
    /* Build params once, reuse per block — bc7enc doesn't mutate the
     * struct, and this avoids re-running the perceptual-weights
     * helper on every block. */
    bc7enc_compress_block_params params;
    params_for(&params, quality);
#endif

    /* Block rows go to worker threads one at a time (block cost varies
     * widely). Blocks are encoded independently, and the encoders only
     * read their tables, so the output does not depend on the thread
     * count. Small images stay on the calling thread. */
    std::atomic<int> next_row(0);
    auto encode_rows = [&]() {
#ifdef IMGBAKE_HAVE_ISPC
        /* Each row of blocks is split in two strips: the opaque blocks,
         * which take the faster opaque profile, and the blocks with
         * alpha. Each strip is encoded in one call, so the kernel fills
         * its SIMD lanes, and its blocks go back to their places. */
        std::vector<uint8_t> strips[2] = { std::vector<uint8_t>((size_t)bx * 64u),
                                           std::vector<uint8_t>((size_t)bx * 64u) };
        std::vector<int> columns[2] = { std::vector<int>((size_t)bx), std::vector<int>((size_t)bx) };
        std::vector<uint8_t> encoded((size_t)bx * 16u);
        bc7_enc_settings *settings[2] = { &opaque_settings, &alpha_settings };
        for (int y = next_row++; y < by; y = next_row++) {
            int count[2] = { 0, 0 };
            for (int x = 0; x < bx; ++x) {
                uint8_t block_rgba[64];
                copy_4x4_padded(block_rgba, rgba, w, h, x, y);
                const int kind = has_alpha(block_rgba, 16) ? 1 : 0;
                const int slot = count[kind]++;
                columns[kind][(size_t)slot] = x;
                for (int r = 0; r < 4; ++r)
                    memcpy(&strips[kind][(size_t)r * bx * 16u + (size_t)slot * 16u], block_rgba + r * 16, 16);
            }
            uint8_t *row_out = out_blocks + (size_t)y * (size_t)bx * 16u;
            for (int kind = 0; kind < 2; ++kind) {
                if (count[kind] == 0)
                    continue;
                rgba_surface surface = { strips[kind].data(), count[kind] * 4, 4, bx * 16 };
                CompressBlocksBC7(&surface, encoded.data(), settings[kind]);
                for (int i = 0; i < count[kind]; ++i)
                    memcpy(row_out + (size_t)columns[kind][(size_t)i] * 16u, &encoded[(size_t)i * 16u], 16);
            }
        }
#else
        for (int y = next_row++; y < by; y = next_row++) {
            for (int x = 0; x < bx; ++x) {
                uint8_t block_rgba[64];
                copy_4x4_padded(block_rgba, rgba, w, h, x, y);
                uint8_t *dst = out_blocks + ((size_t)y * (size_t)bx + (size_t)x) * 16u;
                bc7enc_compress_block(dst, block_rgba, &params);
            }
        }
#endif
    };
    unsigned threads = s_max_threads ? s_max_threads.load() : std::thread::hardware_concurrency();
    if ((size_t)bx * (size_t)by < 4096u || threads < 2) {
        encode_rows();
        return need;
    }
    if (threads > (unsigned)by)
        threads = (unsigned)by;
    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t)
        workers.emplace_back(encode_rows);
    encode_rows();
    for (std::thread &worker : workers)
        worker.join();
    return need;
}
