#ifndef AERON_SCENE_RGBA_UPLOAD_H
#define AERON_SCENE_RGBA_UPLOAD_H

#include <stdbool.h>

#include "aeron/image.h"
#include "aeron/render.h"

AeronTexture *aeron_scene_upload_rgba8(
		AeronCommandBuffer *cmd, const uint8_t *pixels,
		int width, int height, size_t pitch,
		AeronTextureFormat format, AeronColorSpace color_space,
		AeronImageAlphaMode alpha_mode, bool generate_mips,
		const char *debug_name);

#endif
