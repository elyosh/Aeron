/*
 * bc6h_codec — implementation. Wraps DirectXTex's CPU BC6H encoder.
 *
 * The DirectXTex API takes 16 XMVECTOR pixels (one per texel in a
 * 4×4 block); we expose a flat float-RGB API and stage to XMVECTOR
 * locally per block.
 *
 * Quality is accepted for parity with bc7_codec but currently
 * ignored — DirectXTex's BC6H encoder has no exposed quality knob.
 */

#include "bc6h_codec.h"

#include "third_party/dxtex/dxtex_compat.h"

#include <mutex>
#include <thread>
#include <vector>

/* Forward-declare the DirectXTex BC6H entry point. The encoder lives
 * in BC6HBC7.cpp (vendored under third_party/dxtex/). We don't
 * include the DirectXTex umbrella header — only this one prototype. */
namespace DirectX
{
    void D3DXEncodeBC6HU(uint8_t *pBC,
                         const DirectX::XMVECTOR *pColor,
                         uint32_t flags) noexcept;
}

static std::once_flag s_init_flag;

extern "C" void bc6h_codec_init(void)
{
    /* No DirectXTex-side initialisation is needed; the encoder uses
     * only static tables. Reserved for parity with bc7_codec_init. */
    std::call_once(s_init_flag, []() {});
}

extern "C"
size_t bc6h_codec_image_size(int w, int h)
{
    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;
    return (size_t)bx * (size_t)by * 16u;
}

/* Stage one 4×4 block. Float RGB input, edge-clamp for blocks that
 * fall off the right/bottom side of a non-multiple-of-4 image. */
static void copy_4x4_padded_f32(DirectX::XMVECTOR out_block[16],
                                const float *rgb, int w, int h,
                                int bx, int by)
{
    for (int y = 0; y < 4; ++y) {
        int sy = by * 4 + y;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < 4; ++x) {
            int sx = bx * 4 + x;
            if (sx >= w) sx = w - 1;
            const float *src = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3u;
            out_block[y * 4 + x] = DirectX::XMVectorSet(src[0], src[1], src[2], 1.0f);
        }
    }
}

extern "C"
void bc6h_codec_encode_block(uint8_t out_block[16],
                             const float in_rgb_4x4[48],
                             Bc6hQuality quality)
{
    (void)quality;
    DirectX::XMVECTOR pixels[16];
    for (int i = 0; i < 16; ++i) {
        const float *p = in_rgb_4x4 + i * 3;
        pixels[i] = DirectX::XMVectorSet(p[0], p[1], p[2], 1.0f);
    }
    DirectX::D3DXEncodeBC6HU(out_block, pixels, 0u);
}

/* Encode contiguous block range [begin, end) into out_blocks. The
 * DirectXTex encoder is stateless and only reads the static lookup
 * tables in BC6HBC7.cpp, so concurrent calls from different threads
 * with disjoint block ranges are safe. */
static void encode_block_range(uint8_t *out_blocks, const float *rgb,
                               int w, int h, int bx, int begin, int end)
{
    for (int i = begin; i < end; ++i) {
        int x = i % bx;
        int y = i / bx;
        DirectX::XMVECTOR block[16];
        copy_4x4_padded_f32(block, rgb, w, h, x, y);
        uint8_t *dst = out_blocks + (size_t)i * 16u;
        DirectX::D3DXEncodeBC6HU(dst, block, 0u);
    }
}

extern "C"
size_t bc6h_codec_encode_image(uint8_t *out_blocks, size_t out_cap,
                               const float *rgb, int w, int h,
                               Bc6hQuality quality)
{
    (void)quality;
    if (!out_blocks || !rgb || w < 1 || h < 1) return 0;
    int bx = (w + 3) / 4;
    int by = (h + 3) / 4;
    size_t need = (size_t)bx * (size_t)by * 16u;
    if (out_cap < need) return 0;

    const int total_blocks = bx * by;

    /* Parallelize across hardware cores. The BC6H reference encoder is
     * ~100-500 µs per block on Apple Silicon; a large face (e.g. 2048²
     * = 262 K blocks) takes minutes single-threaded. Static
     * partitioning is good enough — per-block time variance is bounded
     * by the encoder's fixed mode search. */
    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;
    /* Small images: not worth the thread spin-up overhead. */
    if (total_blocks < 256) nthreads = 1;
    if ((int)nthreads > total_blocks) nthreads = (unsigned int)total_blocks;

    if (nthreads == 1) {
        encode_block_range(out_blocks, rgb, w, h, bx, 0, total_blocks);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        int per_thread = (total_blocks + (int)nthreads - 1) / (int)nthreads;
        for (unsigned int t = 0; t < nthreads; ++t) {
            int begin = (int)t * per_thread;
            if (begin >= total_blocks) break;
            int end = begin + per_thread;
            if (end > total_blocks) end = total_blocks;
            threads.emplace_back(encode_block_range, out_blocks, rgb,
                                 w, h, bx, begin, end);
        }
        for (auto &th : threads) th.join();
    }
    return need;
}
