/*
 * bc7_codec — C facade over the vendored bc7enc encoder.
 *
 * bc7enc.h itself has no include guard and uses C++ types in its
 * params struct, so we keep it confined to a single C++ TU
 * (bc7_codec.cpp) and expose a flat C API to the rest of the film
 * tooling (filmextract, ktx2_writer, the standalone bc7enc CLI).
 */
#ifndef FILM_BC7_CODEC_H
#define FILM_BC7_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BC7_QUALITY_FAST = 0,   /* Mode 6 only-ish, no partition search. ~ms per 4K block. */
    BC7_QUALITY_MED  = 1,   /* All modes, moderate partition count. */
    BC7_QUALITY_UBER = 2,   /* All modes, all 64 partitions, max uber level. */
} Bc7Quality;

/* Initialise bc7enc's internal lookup tables. Idempotent. Call once
 * at startup before any encode_* call. */
void bc7_codec_init(void);

/* Encode a 4×4 RGBA block (64 bytes, R first) to a 16-byte BC7
 * block. Both buffers are caller-owned. */
void bc7_codec_encode_block(uint8_t out_block[16],
                            const uint8_t in_rgba_4x4[64],
                            Bc7Quality quality);

/* Encode a whole RGBA8 image (tightly-packed, R first) into a flat
 * BC7 block stream. Out buffer must be ≥ ceil(w/4)*ceil(h/4)*16
 * bytes. Pixels at the right/bottom edges that don't fill a full
 * 4×4 block are padded with the last row/col before encode. Returns
 * the number of bytes written. */
size_t bc7_codec_encode_image(uint8_t *out_blocks, size_t out_cap,
                              const uint8_t *rgba, int w, int h,
                              Bc7Quality quality);

/* Convenience: byte length of the BC7 block stream for a
 * (w, h)-sized image. */
size_t bc7_codec_image_size(int w, int h);

#ifdef __cplusplus
}
#endif

#endif
