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
#define XXH_INLINE_ALL
#include "xxhash.h"
#ifdef IMGBAKE_HAVE_ISPC
#include "ispc_texcomp.h"
#endif

#include <atomic>
#include <mutex>
#include <utility>
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

/* ---- Encoded blocks --------------------------------------------------
 *
 * BC7 encodes each 4x4 block on its own, so a block that the process has
 * already encoded is copied instead of encoded again. A model's skin variants
 * repeat their base texture: in the X-wing's cook, 1 block in 10 is distinct.
 * Blocks are found by a 128-bit hash of their 64 texel bytes. The cache is
 * for one quality, and lasts for the process. */

struct CachedBlock {
    uint64_t lo, hi; /* hi == 0: empty */
    uint8_t  block[16];
};

static std::mutex               s_cache_mutex;
static std::vector<CachedBlock> s_cache;
static size_t                   s_cache_count;
static int                      s_cache_quality = -1;

static XXH128_hash_t block_hash(const uint8_t rgba[64])
{
    XXH128_hash_t hash = XXH3_128bits(rgba, 64);
    hash.high64 |= 1u; /* never 0, which marks an empty slot */
    return hash;
}

static CachedBlock *cache_slot(std::vector<CachedBlock> &table, XXH128_hash_t hash)
{
    const size_t mask = table.size() - 1;
    for (size_t i = (size_t)hash.low64 & mask;; i = (i + 1) & mask) {
        CachedBlock &slot = table[i];
        if (slot.hi == 0 || (slot.hi == hash.high64 && slot.lo == hash.low64))
            return &slot;
    }
}

static void cache_reserve(size_t more)
{
    if ((s_cache_count + more) * 2 <= s_cache.size())
        return;
    size_t size = s_cache.empty() ? 1u << 16 : s_cache.size();
    while ((s_cache_count + more) * 2 > size)
        size *= 2;
    std::vector<CachedBlock> table(size);
    for (const CachedBlock &old : s_cache)
        if (old.hi != 0)
            *cache_slot(table, XXH128_hash_t { old.lo, old.hi }) = old;
    s_cache.swap(table);
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

    std::lock_guard<std::mutex> lock(s_cache_mutex);
    if (s_cache_quality != (int)quality) {
        s_cache.clear();
        s_cache_count   = 0;
        s_cache_quality = (int)quality;
    }

    /* The blocks to encode: the distinct blocks that the cache doesn't
     * have. `fills` gives, for each other block of the image, the one it
     * copies. */
    std::vector<uint8_t>   todo;
    std::vector<XXH128_hash_t> todo_hashes;
    std::vector<std::pair<size_t, size_t>> fills;
    {
        std::vector<CachedBlock> pending(64);
        size_t                   pending_count = 0;
        for (int y = 0; y < by; ++y) {
            for (int x = 0; x < bx; ++x) {
                uint8_t block_rgba[64];
                copy_4x4_padded(block_rgba, rgba, w, h, x, y);
                const XXH128_hash_t hash = block_hash(block_rgba);
                const size_t        index = (size_t)y * (size_t)bx + (size_t)x;
                if (!s_cache.empty()) {
                    const CachedBlock *cached = cache_slot(s_cache, hash);
                    if (cached->hi != 0) {
                        memcpy(out_blocks + index * 16u, cached->block, 16);
                        continue;
                    }
                }
                CachedBlock *slot = cache_slot(pending, hash);
                if (slot->hi == 0) {
                    slot->lo = hash.low64;
                    slot->hi = hash.high64;
                    /* A pending block holds its place in `todo`. */
                    const size_t todo_index = todo_hashes.size();
                    todo_hashes.push_back(hash);
                    memcpy(slot->block, &todo_index, sizeof todo_index);
                    todo.insert(todo.end(), block_rgba, block_rgba + 64);
                    if (++pending_count * 2 > pending.size()) {
                        std::vector<CachedBlock> grown(pending.size() * 2);
                        for (const CachedBlock &old : pending)
                            if (old.hi != 0)
                                *cache_slot(grown, XXH128_hash_t { old.lo, old.hi }) = old;
                        pending.swap(grown);
                    }
                    slot = cache_slot(pending, hash);
                }
                size_t todo_index;
                memcpy(&todo_index, slot->block, sizeof todo_index);
                fills.emplace_back(index, todo_index);
            }
        }
    }

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

    /* Chunks of blocks go to worker threads one at a time (block cost
     * varies widely). Blocks are encoded independently, and the encoders
     * only read their tables, so the output does not depend on the thread
     * count. */
    const size_t         count = todo_hashes.size();
    std::vector<uint8_t> encoded(count * 16u);
    const size_t         chunk = 256;
    std::atomic<size_t>  next_chunk(0);
    auto encode_chunks = [&]() {
#ifdef IMGBAKE_HAVE_ISPC
        /* Each chunk is split in two strips: the opaque blocks, which take
         * the faster opaque profile, and the blocks with alpha. Each strip
         * is encoded in one call, so the kernel fills its SIMD lanes. */
        std::vector<uint8_t> strips[2] = { std::vector<uint8_t>(chunk * 64u),
                                           std::vector<uint8_t>(chunk * 64u) };
        std::vector<size_t>  blocks[2] = { std::vector<size_t>(chunk), std::vector<size_t>(chunk) };
        std::vector<uint8_t> strip_out(chunk * 16u);
        bc7_enc_settings    *settings[2] = { &opaque_settings, &alpha_settings };
        for (size_t first = next_chunk++ * chunk; first < count; first = next_chunk++ * chunk) {
            const size_t last  = first + chunk < count ? first + chunk : count;
            size_t       n[2] = { 0, 0 };
            for (size_t i = first; i < last; ++i) {
                const uint8_t *block_rgba = &todo[i * 64u];
                const int      kind       = has_alpha(block_rgba, 16) ? 1 : 0;
                blocks[kind][n[kind]]     = i;
                for (int r = 0; r < 4; ++r)
                    memcpy(&strips[kind][(size_t)r * chunk * 16u + n[kind] * 16u], block_rgba + r * 16, 16);
                ++n[kind];
            }
            for (int kind = 0; kind < 2; ++kind) {
                if (n[kind] == 0)
                    continue;
                rgba_surface surface = { strips[kind].data(), (int)n[kind] * 4, 4, (int)(chunk * 16u) };
                CompressBlocksBC7(&surface, strip_out.data(), settings[kind]);
                for (size_t i = 0; i < n[kind]; ++i)
                    memcpy(&encoded[blocks[kind][i] * 16u], &strip_out[i * 16u], 16);
            }
        }
#else
        for (size_t first = next_chunk++ * chunk; first < count; first = next_chunk++ * chunk) {
            const size_t last = first + chunk < count ? first + chunk : count;
            for (size_t i = first; i < last; ++i)
                bc7enc_compress_block(&encoded[i * 16u], &todo[i * 64u], &params);
        }
#endif
    };
    unsigned threads = s_max_threads ? s_max_threads.load() : std::thread::hardware_concurrency();
    if (count < 4096u || threads < 2) {
        encode_chunks();
    } else {
        const size_t chunks = (count + chunk - 1) / chunk;
        if (threads > chunks)
            threads = (unsigned)chunks;
        std::vector<std::thread> workers;
        workers.reserve(threads - 1);
        for (unsigned t = 1; t < threads; ++t)
            workers.emplace_back(encode_chunks);
        encode_chunks();
        for (std::thread &worker : workers)
            worker.join();
    }

    for (const auto &fill : fills)
        memcpy(out_blocks + fill.first * 16u, &encoded[fill.second * 16u], 16);
    cache_reserve(count);
    for (size_t i = 0; i < count; ++i) {
        CachedBlock *slot = cache_slot(s_cache, todo_hashes[i]);
        slot->lo = todo_hashes[i].low64;
        slot->hi = todo_hashes[i].high64;
        memcpy(slot->block, &encoded[i * 16u], 16);
    }
    s_cache_count += count;
    return need;
}
