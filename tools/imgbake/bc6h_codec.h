/*
 * bc6h_codec — C facade over the vendored DirectXTex BC6H encoder.
 *
 * Mirrors bc7_codec's surface. Operates on linear HDR RGB float32
 * input — three channels, tightly packed, no alpha. Output is a
 * BC6H block stream (16 bytes per 4×4 block, same block geometry
 * as BC7). The KTX2 vkFormat to pair this with is
 * VK_FORMAT_BC6H_UFLOAT_BLOCK (143).
 *
 * DirectXTex's BC6H encoder is single-pass; the `quality` parameter
 * is accepted for API parity with bc7_codec but currently ignored —
 * the encoder has no exposed quality knob.
 */
#ifndef IMGBAKE_BC6H_CODEC_H
#define IMGBAKE_BC6H_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BC6H_QUALITY_FAST = 0,
    BC6H_QUALITY_MED  = 1,
    BC6H_QUALITY_UBER = 2,
} Bc6hQuality;

/* Idempotent initialization hook; safe to call multiple times. */
void bc6h_codec_init(void);

/* Encode one 4×4 block of linear HDR RGB (48 floats = 16 pixels ×
 * R, G, B) into a 16-byte BC6H block. Both buffers are caller-owned. */
void bc6h_codec_encode_block(uint8_t out_block[16],
                             const float in_rgb_4x4[48],
                             Bc6hQuality quality);

/* Encode a whole RGB-f32 image (tightly packed, R first) into a flat
 * BC6H block stream. Output buffer must be ≥ ceil(w/4)*ceil(h/4)*16
 * bytes. Right/bottom edges that don't fill a full 4×4 block are
 * padded by clamping to the last row/col. Returns the number of bytes
 * written. */
size_t bc6h_codec_encode_image(uint8_t *out_blocks, size_t out_cap,
                               const float *rgb, int w, int h,
                               Bc6hQuality quality);

/* Convenience: byte length of the BC6H block stream for a (w, h)
 * image. Identical to bc7's because the block geometry matches. */
size_t bc6h_codec_image_size(int w, int h);

#ifdef __cplusplus
}
#endif

#endif
