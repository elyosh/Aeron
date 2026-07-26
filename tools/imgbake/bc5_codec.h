/*
 * bc5_codec — C facade over the vendored rgbcx encoder
 * (richgel999/bc7enc_rdo, MIT / public domain). Implementation in
 * bc5_codec.cpp; rgbcx itself lives under third_party/bc7enc/.
 *
 * BC5 stores two independent BC4 channels (R, G) in a 16-byte 4×4 block,
 * same block geometry as BC7 (8 bpp). Designed for tangent-space normal
 * maps: X/Y are encoded; Z is reconstructed in-shader from
 *   z = sqrt(1 - x² - y²).
 *
 * BC4's quality on uncorrelated 1-channel data exceeds BC7's on the same
 * data — no shared block endpoints, no mode-selection compromises. The
 * encoder is also much faster than BC7-medium because BC4 has only two
 * modes (8-color vs 6-color) and no partition search.
 *
 * No SRGB variant — BC5 is linear-only in Vulkan (no VK_FORMAT_BC5_*_SRGB
 * exists). Callers that need sRGB stay on BC7.
 *
 * Reads RGBA8 sources (matching the channel_atlas RGBA layout) and picks
 * R + G; B + A are ignored.
 */
#ifndef FILM_BC5_CODEC_H
#define FILM_BC5_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Encode one 4×4 block. `in_rgba_4x4` is 64 bytes (R first); only R and
 * G are read. `out_block` is 16 bytes (two 8-byte BC4 sub-blocks: R, G). */
void bc5_codec_encode_block_from_rgba(uint8_t out_block[16],
                                      const uint8_t in_rgba_4x4[64]);

/* Encode a whole RGBA8 image (tightly packed, R first) into a flat BC5
 * block stream. Out buffer must be ≥ ceil(w/4)*ceil(h/4)*16 bytes.
 * Pixels at the right/bottom edges that don't fill a full 4×4 block
 * are padded with the last row/col before encode. Returns the number of
 * bytes written (or 0 on overflow). */
size_t bc5_codec_encode_image_from_rgba(uint8_t *out_blocks, size_t out_cap,
                                        const uint8_t *rgba, int w, int h);

/* Convenience: byte length of the BC5 block stream for a (w, h) image. */
size_t bc5_codec_image_size(int w, int h);

#ifdef __cplusplus
}
#endif

#endif
