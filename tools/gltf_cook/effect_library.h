/*
 * Effect image libraries (animated_materials.md, "Effect libraries"):
 * reading a library file, <library>.yaml, building it, and the writer,
 * which packs baked images into the layer and emissive page arrays and
 * writes the library's three files,
 *
 *   <out>/<name>.yaml           the index: key -> page, rect, mip limit
 *   <out>/<name>.layer.ktx2     when an image has a layer
 *   <out>/<name>.emissive.ktx2  when an image has an emissive part
 *
 * Pages are 2176² BC7 sRGB arrays with mips, alpha stored independently
 * (KTX2_ALPHA_INDEPENDENT). Images are packed in key order, so the output
 * does not depend on the order they are given in.
 */
#ifndef AERON_EFFECT_LIBRARY_H
#define AERON_EFFECT_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/* The version of what makes a library's files from its inputs: baking the
 * images by kind, packing and encoding them. Raise it with any change that
 * alters the files built from the same inputs, including in the encoder
 * (imgbake): the inputs hash a library's index records includes it, so the
 * tools rebuild every library once, and only then. */
#define EFFECT_LIBRARY_BAKE_VERSION 2

#define EFFECT_LIBRARY_PAGE_SIZE 2176
#define EFFECT_LIBRARY_IMAGE_MAX 1024 /* longest side; larger images are box-downscaled */

typedef enum EffectLibraryKind {
	EFFECT_LIBRARY_BASE_COLOR,
	EFFECT_LIBRARY_EMISSIVE,
	EFFECT_LIBRARY_DAMAGE,
	EFFECT_LIBRARY_MARK,
	EFFECT_LIBRARY_ENERGY,
	EFFECT_LIBRARY_KIND_COUNT, /* not a kind */
} EffectLibraryKind;

/* One baked image. `layer` and `emissive` are tightly packed RGBA8 of
 * width × height, sRGB colour with linear alpha, NULL when absent (at least
 * one is set). They are read in place until EffectLibrary_Write returns. */
typedef struct EffectLibraryImage {
	const char*       key;
	EffectLibraryKind kind; /* written to the index */
	int            width, height;
	const uint8_t* layer;
	const uint8_t* emissive;
	int            max_size; /* longest side cap; 0: EFFECT_LIBRARY_IMAGE_MAX */
	const float*   crop;     /* [x0, y0, x1, y1] written to the index, or NULL */
} EffectLibraryImage;

typedef struct EffectLibraryResult {
	int layer_pages;
	int emissive_pages;
} EffectLibraryResult;

/* Checks library file content `library` (read from `label`, for messages)
 * for what can be checked without reading the images: its members, version
 * and name, and per image its kind, its inputs' presence (strings, paths
 * relative to the file), maxSize and crop. Messages go to stderr, prefixed
 * with `tool`. */
bool EffectLibrary_CheckLibrary(const char* tool, const char* label, const struct cJSON* library);

/* Reads library file `file` as a cJSON tree (owned by the caller) and checks
 * it (EffectLibrary_CheckLibrary). NULL on failure, with messages on stderr
 * prefixed with `tool`. */
struct cJSON* EffectLibrary_ReadLibrary(const char* tool, const char* file);

/* The kind of a checked image entry. */
EffectLibraryKind EffectLibrary_ImageKind(const struct cJSON* image);

/* A kind's name in library files and indexes (baseColor, emissive, damage,
 * mark, energy). */
const char* EffectLibrary_KindName(EffectLibraryKind kind);

/* The field naming a kind's required input (`optional` 0), or its optional
 * one (1, NULL when it has none). */
const char* EffectLibrary_ImageInput(EffectLibraryKind kind, int optional);

typedef enum EffectLibraryBuild {
	EFFECT_LIBRARY_FAILED,
	EFFECT_LIBRARY_UNCHANGED, /* the index in the output directory records the same inputs */
	EFFECT_LIBRARY_BUILT,
} EffectLibraryBuild;

/* Builds library file `file` into `out_dir`: reads and checks it, bakes its
 * images by kind and writes the library, unless the index there already
 * records the same inputs (EFFECT_LIBRARY_BAKE_VERSION, the file and its
 * images). With `name`, the file must hold that library. Messages go to
 * stderr, prefixed with `tool`: one line per library built or unchanged,
 * and, when a rebuild replaces an index, one per key it no longer has
 * (`<tool>: <library>: removed "<key>"`) and per image whose kind changed
 * (`<tool>: <library>: "<key>" changed kind from <old> to <new>`), keys as
 * JSON strings. */
EffectLibraryBuild EffectLibrary_BuildLibrary(const char* tool, const char* file, const char* name,
											  const char* out_dir);

/* Writes library `name` into `out_dir`. `source` and `inputs`, when not
 * NULL, are noted in the index (`source:`, `inputs:`, which the runtime
 * ignores: a tool's own record of what it built the library from). Keys
 * must be unique. Messages go to stderr, prefixed with `tool`. Returns false
 * on failure. */
bool EffectLibrary_Write(const char* tool, const char* out_dir, const char* name, const char* source,
						 const char* inputs, const EffectLibraryImage* images, size_t count,
						 EffectLibraryResult* result);

#ifdef __cplusplus
}
#endif

#endif /* AERON_EFFECT_LIBRARY_H */
