/*
 * bc7_codec — C facade over the BC7 encoders: the ISPC Texture Compressor
 * in builds that have it, else the vendored bc7enc.
 *
 * Both encoders are C++, so they stay confined to a single C++ TU
 * (bc7_codec.cpp), which exposes a flat C API to asset tools.
 */
#ifndef IMGBAKE_BC7_CODEC_H
#define IMGBAKE_BC7_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* With ISPC: its veryfast, fast and slow profiles. With bc7enc: mode 6
 * mostly and no partition search; all modes with a moderate partition
 * count; all modes, all 64 partitions and the maximum uber level. */
typedef enum {
	BC7_QUALITY_FAST = 0,
	BC7_QUALITY_MED  = 1,
	BC7_QUALITY_UBER = 2,
} Bc7Quality;

/* Initialise the encoders' internal lookup tables. Idempotent. Call once
 * at startup before any encode_* call. */
void bc7_codec_init(void);

/* Encode a 4×4 RGBA block (64 bytes, R first) to a 16-byte BC7
 * block. Both buffers are caller-owned. */
void bc7_codec_encode_block(uint8_t out_block[16], const uint8_t in_rgba_4x4[64], Bc7Quality quality);

/* The most threads that bc7_codec_encode_image uses; 0, the default, uses
 * one per hardware thread. A tool that runs in several copies at once sets
 * it, so that the copies don't run more threads than the CPU has. */
void bc7_codec_set_max_threads(unsigned threads);

/* Empties the calling thread's cache of encoded blocks, and frees it.
 * bc7_codec_encode_image reuses the blocks that the same thread has already
 * encoded, until this is called. Call it when a job starts, such as one
 * model's cook, so that the cache doesn't keep growing. */
void bc7_codec_clear_cache(void);

/* Encode a whole RGBA8 image (tightly-packed, R first) into a flat
 * BC7 block stream. Out buffer must be ≥ ceil(w/4)*ceil(h/4)*16
 * bytes. Pixels at the right/bottom edges that don't fill a full
 * 4×4 block are padded with the last row/col before encode. Large
 * images are encoded on several threads (bc7_codec_set_max_threads);
 * the output is the same as a single-threaded encode. Returns the number
 * of bytes written. */
size_t bc7_codec_encode_image(uint8_t* out_blocks, size_t out_cap, const uint8_t* rgba, int w, int h,
							  Bc7Quality quality);

/* Convenience: byte length of the BC7 block stream for a
 * (w, h)-sized image. */
size_t bc7_codec_image_size(int w, int h);

#ifdef __cplusplus
}
#endif

#endif
