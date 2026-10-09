#ifndef AERON_SCENE_EFFECT_ATLAS_H
#define AERON_SCENE_EFFECT_ATLAS_H

/*
 * AeronEffectAtlas — resident effect image libraries.
 *
 * A library is `<dir>/<name>.yaml` (its index) with `<name>.layer.ktx2`
 * and/or `<name>.emissive.ktx2`, 2D arrays of equal square pages. The atlas
 * owns two 2D array textures, layer and emissive, and uploads each library's
 * pages into free layers of them. Libraries are reference-counted; a
 * released library's layers are reused by later loads. A load that does not
 * fit grows the texture: a new texture receives a copy of every layer and
 * the old one is retired until AeronEffectAtlas_Commit. A loaded page never
 * changes layer, so resolved images stay valid while their library is held.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronEffectAtlas AeronEffectAtlas;

#define AERON_EFFECT_LAYER 0
#define AERON_EFFECT_EMISSIVE 1

/* One library image. `rect` is in texels of a page (AeronEffectAtlas_PageSize). */
typedef struct AeronEffectImage {
	bool     present[2]; /* per texture (AERON_EFFECT_*): the image has one */
	uint32_t layer[2];   /* array layer of the atlas texture */
	uint32_t rect[2][4]; /* x, y, w, h */
	uint32_t lod[2];     /* deepest mip level the image may sample */
	float    crop[4];    /* sampled part in the image's UV: u0, v0, u1, v1 */
} AeronEffectImage;

/* An atlas loading libraries from `dir` under `root` of `vfs` (borrowed;
 * it must outlive the atlas). */
AeronEffectAtlas* AeronEffectAtlas_Create(AeronVfs* vfs, AeronVfsRoot root, const char* dir);
/* Destroys the textures, including retired ones; the GPU must be done with them. */
void AeronEffectAtlas_Destroy(AeronEffectAtlas* atlas);

/* Loads library `name` through `cmd` (no pass may be open), or takes another
 * reference to it when it is resident. Returns 0 when it cannot be read or
 * does not fit the atlas's pages (logged). */
int  AeronEffectAtlas_Acquire(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd, const char* name,
							  uint32_t* out_library);
void AeronEffectAtlas_Release(AeronEffectAtlas* atlas, uint32_t library);

/* Destroys the textures retired by growth; call once the command buffers that
 * recorded the growth copies are submitted. */
void AeronEffectAtlas_Commit(AeronEffectAtlas* atlas);

/* The image `key` of an acquired library; 0 when the library has no such key. */
int AeronEffectAtlas_Find(const AeronEffectAtlas* atlas, uint32_t library, const char* key,
						  AeronEffectImage* out);

/* The layer or emissive texture (AERON_EFFECT_*), NULL before any library
 * with such pages is loaded. */
AeronTexture* AeronEffectAtlas_Texture(const AeronEffectAtlas* atlas, int which);

/* Texel size of a page; 0 before any library is loaded. */
uint32_t AeronEffectAtlas_PageSize(const AeronEffectAtlas* atlas);

/*
 * AeronEffectData — the game's effect data, `effects.yaml` in the atlas
 * directory: flipbooks for images the game draws directly, keyed by the
 * game's image name ("G-I"), with their frames resolved in the atlas.
 */
typedef struct AeronEffectData AeronEffectData;

/* Loads effects.yaml and acquires the libraries its images use, through
 * `cmd`. NULL when the file is absent (silently) or malformed (logged). The
 * atlas must outlive the data. */
AeronEffectData* AeronEffectData_Load(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd);
/* Releases the libraries. */
void AeronEffectData_Destroy(AeronEffectData* data);

/* Index of the flipbook of DAT image `key` ("G-I"); -1 without one. */
int AeronEffectData_FindImage(const AeronEffectData* data, const char* key);
/* Seconds of one pass through image flipbook `index`. */
double AeronEffectData_ImageDuration(const AeronEffectData* data, int index);
/* The frame of image flipbook `index` `t` seconds after it started (looped,
 * or held at the end for `once`); 0 when that frame is not in its library. */
int AeronEffectData_ImageFrame(const AeronEffectData* data, int index, double t, AeronEffectImage* out);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_EFFECT_ATLAS_H */
