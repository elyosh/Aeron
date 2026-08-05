#ifndef AERON_SCENE_RUNTIME_ATLAS_H
#define AERON_SCENE_RUNTIME_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

#include "aeron/image.h"
#include "aeron/render.h"
#include "aeron/scene/sprite_atlas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronRuntimeAtlasFrame {
	const uint8_t *rgba;
	int width, height;
	int32_t id;
	int anchor_x, anchor_y;
} AeronRuntimeAtlasFrame;

typedef struct AeronRuntimeAtlasOptions {
	AeronTextureFormat format;
	AeronColorSpace color_space;
	AeronImageAlphaMode alpha_mode;
	bool generate_mips;
	const char *debug_name;
} AeronRuntimeAtlasOptions;

typedef struct AeronRuntimeAtlasPage {
	AeronTexture *texture;
	int width, height;
} AeronRuntimeAtlasPage;

typedef struct AeronRuntimeAtlas {
	AeronSpriteAtlas layout;
	AeronRuntimeAtlasPage *pages;
} AeronRuntimeAtlas;

bool Aeron_RuntimeAtlasBuild(
		AeronRuntimeAtlas *out, AeronCommandBuffer *cmd,
		const AeronRuntimeAtlasFrame *frames, int frame_count,
		const AeronRuntimeAtlasOptions *options);
void Aeron_RuntimeAtlasRelease(AeronRuntimeAtlas *atlas);

#ifdef __cplusplus
}
#endif

#endif
