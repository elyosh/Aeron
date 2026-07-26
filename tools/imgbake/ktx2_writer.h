/*
 * ktx2_writer — minimal KTX2 writer for filmextract.
 *
 * Writes uncompressed RGBA8 KTX2 files with a pre-built mip chain.
 * The reader (shells/sdl3/external/ktx2_reader.c) consumes them
 * directly into SDL_GPUTexture mip levels at runtime.
 *
 * Subset emitted (matches what the runtime reader accepts):
 *   - 2D (faceCount=1) or cubemap (faceCount=6), single layer.
 *   - supercompressionScheme=0 (raw) or 2 (zstd).
 *   - vkFormat in { 37 R8G8B8A8_UNORM, 145 BC7_UNORM_BLOCK,
 *                  146 BC7_SRGB_BLOCK }. The SRGB variant is used
 *     by the cubemap path so the GPU does sRGB decode at sample
 *     time; existing 2D writers stay UNORM-only to preserve the
 *     cutscene compositor's bit-for-bit expectations.
 *   - DFD/KVD/SGD blocks: empty (offset=0, length=0). The runtime
 *     reader doesn't parse them; some external validators will warn,
 *     which we accept for the in-tree pipeline.
 */
#ifndef FILM_KTX2_WRITER_H
#define FILM_KTX2_WRITER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One mip level for the writer. `data` is RGBA8 pixels (4 bytes per
 * pixel) for write_ktx2_rgba_mips, BC7 blocks (16 bytes per 4×4) for
 * write_ktx2_bc7_mips. The caller owns the data; the writer only
 * reads from it during the call. */
typedef struct {
	int            width;
	int            height;
	const uint8_t* data;
	size_t         size; /* bytes; must match the format's block math */
} Ktx2WriteLevel;

/* Optional zstd supercompression. When `zstd` is true the per-level
 * payload bytes are compressed with libzstd and the KTX2 header's
 * supercompressionScheme is set to 2 (the spec-assigned value for
 * zstd). The runtime reader (shells/sdl3/external/ktx2_reader.c)
 * detects that and decompresses on load. Worth it for the BC7 path
 * because TIE Fighter's upscaled VGA content has large uniform
 * regions BC7 encodes to repeating block patterns — zstd ratio of
 * ~0.43 measured on the EMPEROR corpus.
 *
 * Disable for debug or when a downstream tool can't handle
 * supercompressed KTX2 (e.g. quick visual inspection in a generic
 * image viewer that ignores supercompression). */

/* Transfer-function tag — picks between the _UNORM and _SRGB variants
 * of the chosen Vulkan format. Every 2D writer needs the same choice:
 * sRGB-authored colour textures (palette art, cockpit bitmaps, film
 * cels) MUST be tagged KTX2_TF_SRGB so the runtime HW decodes to
 * linear at sample time. Mask / coverage / pure-data textures stay
 * KTX2_TF_LINEAR. */
typedef enum {
	KTX2_TF_LINEAR = 0,
	KTX2_TF_SRGB   = 1,
} Ktx2TransferFn;

/* Write an uncompressed RGBA8 KTX2 with the given mip chain. */
bool write_ktx2_rgba_mips(const char* path, const Ktx2WriteLevel* levels, int level_count, Ktx2TransferFn tf,
						  bool zstd);

/* Write a BC7-compressed KTX2. Each level's `data` must already be
 * a packed BC7 block stream (16 bytes per 4×4 block, ceil(w/4) ×
 * ceil(h/4) blocks). Use bc7_codec_encode_image to produce them. */
bool write_ktx2_bc7_mips(const char* path, const Ktx2WriteLevel* levels, int level_count, Ktx2TransferFn tf,
						 bool zstd);

/* Convenience: encode an RGBA8 base image with bc7_codec, generate a
 * mip chain via 2× box downsample, write the result as BC7 KTX2.
 * Mirrors write_ktx2_rgba_with_generated_mips for the BC7 path. */
typedef enum {
	KTX2_BC7_QUALITY_FAST = 0,
	KTX2_BC7_QUALITY_MED  = 1,
	KTX2_BC7_QUALITY_UBER = 2,
} Ktx2Bc7Quality;

bool write_ktx2_bc7_with_generated_mips(const char* path, int base_w, int base_h, const uint8_t* base_rgba,
										Ktx2Bc7Quality quality, Ktx2TransferFn tf, bool zstd);

/* Buffer-mode sibling: build the same BC7 KTX2 image in memory and
 * hand the caller a fresh malloc'd buffer (free with `free()`). Used by
 * aeron_gltf_cook to embed cooked KTX2 payloads into the GLB BIN chunk without
 * touching the filesystem. On success: *out_buf is non-NULL and
 * *out_size is the total KTX2 image size. On failure: returns false,
 * *out_buf is NULL, *out_size is 0. */
bool write_ktx2_bc7_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba,
												  Ktx2Bc7Quality quality, Ktx2TransferFn tf, bool zstd,
												  uint8_t** out_buf, size_t* out_size);

/* Buffer-mode variant that emits at most `max_levels` levels. A value
 * of zero retains the complete mip chain. */
bool write_ktx2_bc7_with_generated_mips_to_buffer_limited(int base_w, int base_h, const uint8_t* base_rgba,
														  Ktx2Bc7Quality quality, Ktx2TransferFn tf,
														  bool zstd, int max_levels, uint8_t** out_buf,
														  size_t* out_size);

/* Convenience: build a power-of-two-halving mip chain from an RGBA8
 * base image via 2×2 box downsample, write the result as RGBA8 KTX2.
 * Stops at the smallest dim becoming 1. The base image is included
 * as level 0 (no copy of `base` retained after return). */
bool write_ktx2_rgba_with_generated_mips(const char* path, int base_w, int base_h, const uint8_t* base_rgba,
										 Ktx2TransferFn tf, bool zstd);

/* Buffer-mode sibling of write_ktx2_rgba_with_generated_mips. */
bool write_ktx2_rgba_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba,
												   Ktx2TransferFn tf, bool zstd, uint8_t** out_buf,
												   size_t* out_size);

/* Buffer-mode variant that emits at most `max_levels` levels. A value
 * of zero retains the complete mip chain. */
bool write_ktx2_rgba_with_generated_mips_to_buffer_limited(int base_w, int base_h, const uint8_t* base_rgba,
														   Ktx2TransferFn tf, bool zstd, int max_levels,
														   uint8_t** out_buf, size_t* out_size);

/* Convenience: encode an RGBA8 base image's R+G channels with bc5_codec,
 * generate a mip chain via 2× box downsample, write the result as a
 * BC5 KTX2 (vkFormat=141 VK_FORMAT_BC5_UNORM_BLOCK). No SRGB variant —
 * BC5 is linear-only per Vulkan. No quality knob — the encoder is
 * single-pass. Mirrors write_ktx2_bc7_with_generated_mips_to_buffer
 * for the BC5 path; consumed by aeron_gltf_cook for the normal-map channel. */
bool write_ktx2_bc5_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba, bool zstd,
												  uint8_t** out_buf, size_t* out_size);

/* Buffer-mode variant that emits at most `max_levels` levels. A value
 * of zero retains the complete mip chain. */
bool write_ktx2_bc5_with_generated_mips_to_buffer_limited(int base_w, int base_h, const uint8_t* base_rgba,
														  bool zstd, int max_levels, uint8_t** out_buf,
														  size_t* out_size);

/* Write a BC7 KTX2 cubemap (faceCount=6) with a generated mip chain.
 *
 * `face_size` is the per-face square dimension in pixels. `faces` is
 * six pointers to RGBA8 face buffers in KTX2 face order:
 *   [0]=+X (right), [1]=-X (left),
 *   [2]=+Y (up),    [3]=-Y (down),
 *   [4]=+Z (back),  [5]=-Z (forward).
 *
 * Each face buffer is `face_size * face_size * 4` bytes, tightly
 * packed, top-row-first. No premultiplication is applied — skybox
 * faces are opaque and the runtime sampler ignores alpha. The mip
 * chain is generated per face via 2× box downsample, then each
 * level's 6 faces are concatenated into one blob and (optionally)
 * zstd-compressed before being written. */
bool write_ktx2_bc7_cubemap_with_generated_mips(const char* path, int face_size,
												const uint8_t* const faces[6], Ktx2Bc7Quality quality,
												Ktx2TransferFn tf, bool zstd);

/* BC6H cubemap path — HDR sibling of write_ktx2_bc7_cubemap.
 *
 * `faces[f]` is `face_size * face_size * 3` linear RGB floats, tight,
 * top-row-first, faces in KTX2 order (+X, -X, +Y, -Y, +Z, -Z). The
 * writer generates a 2× box mip chain in linear-HDR space, encodes
 * each level's 6 faces to BC6H (16 B per 4×4 block, identical block
 * geometry to BC7), and packages as KTX2 with
 * VK_FORMAT_BC6H_UFLOAT_BLOCK.
 *
 * `quality` is accepted for API parity; the vendored DirectXTex
 * BC6H encoder is single-pass and ignores it. */
typedef enum {
	KTX2_BC6H_QUALITY_FAST = 0,
	KTX2_BC6H_QUALITY_MED  = 1,
	KTX2_BC6H_QUALITY_UBER = 2,
} Ktx2Bc6hQuality;

bool write_ktx2_bc6h_cubemap_with_generated_mips(const char* path, int face_size, const float* const faces[6],
												 Ktx2Bc6hQuality quality, bool zstd);

#ifdef __cplusplus
}
#endif

#endif
