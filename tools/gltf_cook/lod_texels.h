#ifndef AERON_GLTF_COOK_LOD_TEXELS_H
#define AERON_GLTF_COOK_LOD_TEXELS_H

/* Texels of a model's atlas channels, decoded at a small mip, for the level
 * generator (lod_generate.c) and its measures (lod_measure.c). */

#include "aeron/scene/gltf_mesh.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Ktx2 Ktx2;

/* One atlas page of a channel, decoded to linear RGB. */
typedef struct LodPage {
	float* rgb;
	int    width, height;
} LodPage;

typedef struct LodChannel {
	Ktx2*    ktx2;
	int      level;
	bool     srgb;
	int      pages;
	LodPage* page; /* decoded on first use */
} LodChannel;

/* Opens a channel at its first mip 256 texels wide or less. False when the
 * model has no such channel or its format can't be decoded. */
bool lod_channel_open(LodChannel* c, const AeronGltfChannelKtx2* source);
void lod_channel_close(LodChannel* c);

/* The texel that material `m` shows at UV `uv` in `channel`, found the way
 * the shader samples it (atlas_sample: offset + frac(uv) * scale). False
 * when the material doesn't use the channel. */
bool lod_material_texel(LodChannel* c, const AeronGltfMaterial* m, int channel, const float uv[2],
						float out[3]);

/* The luminance of the light that material `m` emits at `uv`: its emissive
 * factor and strength, times its emissive texture when it has one.
 * `emissive` is the model's emissive channel, or NULL when it has none. */
float lod_emitted_luminance(LodChannel* emissive, const AeronGltfMaterial* m, const float uv[2]);

/* The material of primitive `prim` in the first variant, or NULL. */
const AeronGltfMaterial* lod_primitive_material(const AeronGltfModel* model, uint32_t prim);

#endif
