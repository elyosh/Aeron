/*
 * Effect image library writer (README.md, "Libraries"): packs baked images into the layer and emissive page arrays
 * and writes the library's three files,
 *
 *   <out>/<name>.yaml           the index: key -> page, rect, mip limit
 *   <out>/<name>.layer.ktx2     when an image has a layer
 *   <out>/<name>.emissive.ktx2  when an image has an emissive part
 *
 * Pages are 2176² BC7 sRGB arrays with mips, alpha stored independently
 * (KTX2_ALPHA_INDEPENDENT). Images are packed in key order, so the output
 * does not depend on the order they are given in.
 */
#ifndef AERON_EFFECT_PACK_H
#define AERON_EFFECT_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EFFECT_PACK_PAGE_SIZE 2176
#define EFFECT_PACK_IMAGE_MAX 1024 /* longest side; larger images are box-downscaled */

/* One baked image. `layer` and `emissive` are tightly packed RGBA8 of
 * width × height, sRGB colour with linear alpha, NULL when absent (at least
 * one is set). They are read in place until EffectPack_Write returns. */
typedef struct EffectPackImage {
	const char*    key;
	int            width, height;
	const uint8_t* layer;
	const uint8_t* emissive;
	int            max_size; /* longest side cap; 0: EFFECT_PACK_IMAGE_MAX */
	const float*   crop;     /* [x0, y0, x1, y1] written to the index, or NULL */
} EffectPackImage;

typedef struct EffectPackResult {
	int layer_pages;
	int emissive_pages;
} EffectPackResult;

/* Writes library `name` into `out_dir`. `source` and `inputs`, when not
 * NULL, are noted in the index (`source:`, `inputs:`, which the runtime
 * ignores: a tool's own record of what it built the library from). Keys
 * must be unique. Messages go to stderr, prefixed with `tool`. Returns false
 * on failure. */
bool EffectPack_Write(const char* tool, const char* out_dir, const char* name, const char* source,
					  const char* inputs, const EffectPackImage* images, size_t count, EffectPackResult* result);

#ifdef __cplusplus
}
#endif

#endif /* AERON_EFFECT_PACK_H */
