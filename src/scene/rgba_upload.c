#include "rgba_upload.h"

#include <stdlib.h>
#include <string.h>

static int rgba_mip_count(int width, int height, bool enabled) {
	int count = 1;
	if (!enabled) return count;
	while (width > 1 || height > 1) {
		if (width > 1) width /= 2;
		if (height > 1) height /= 2;
		++count;
	}
	return count;
}

AeronTexture *aeron_scene_upload_rgba8(
		AeronCommandBuffer *cmd, const uint8_t *pixels,
		int width, int height, size_t pitch,
		AeronTextureFormat format, AeronColorSpace color_space,
		AeronImageAlphaMode alpha_mode, bool generate_mips,
		const char *debug_name) {
	if (!cmd || !pixels || width <= 0 || height <= 0 ||
		pitch < (size_t)width * 4 || (size_t)height > SIZE_MAX / pitch)
		return NULL;
	const int mip_count = rgba_mip_count(width, height, generate_mips);
	uint8_t **levels = calloc((size_t)mip_count, sizeof *levels);
	AeronTextureUploadDesc *uploads =
			calloc((size_t)mip_count, sizeof *uploads);
	if (!levels || !uploads) {
		free(levels);
		free(uploads);
		return NULL;
	}
	const size_t base_pitch = (size_t)width * 4;
	if ((size_t)height > SIZE_MAX / base_pitch) goto failed;
	levels[0] = malloc(base_pitch * (size_t)height);
	if (!levels[0]) goto failed;
	for (int row = 0; row < height; ++row)
		memcpy(levels[0] + (size_t)row * base_pitch,
			   pixels + (size_t)row * pitch, base_pitch);
	if (alpha_mode == AERON_IMAGE_ALPHA_STRAIGHT)
		Aeron_ImagePremultiplyRgba8(levels[0], (size_t)width * height);

	int level_width = width;
	int level_height = height;
	for (int mip = 0; mip < mip_count; ++mip) {
		uploads[mip] = (AeronTextureUploadDesc){
				.texture = NULL,
				.mip_level = mip,
				.width = level_width,
				.height = level_height,
				.pixels = levels[mip],
				.pitch = level_width * 4,
				.pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
				.color_space = color_space,
		};
		if (mip + 1 < mip_count) {
			levels[mip + 1] = Aeron_ImageDownsampleRgba8(
					levels[mip], level_width, level_height,
					&level_width, &level_height);
			if (!levels[mip + 1]) goto failed;
		}
	}
	AeronTexture *texture = Aeron_CreateTexture(&(AeronTextureDesc){
			.width = width,
			.height = height,
			.mip_count = mip_count,
			.format = format,
			.usage = AERON_TEXTURE_USAGE_SAMPLED |
					 AERON_TEXTURE_USAGE_TRANSFER_DST,
			.debug_name = debug_name,
	});
	if (!texture) goto failed;
	for (int mip = 0; mip < mip_count; ++mip)
		uploads[mip].texture = texture;
	if (!Aeron_UploadTextureBatchCmd(cmd, uploads, (uint32_t)mip_count)) {
		Aeron_DestroyTexture(texture);
		texture = NULL;
	}
	for (int mip = 0; mip < mip_count; ++mip) free(levels[mip]);
	free(levels);
	free(uploads);
	return texture;

failed:
	for (int mip = 0; mip < mip_count; ++mip) free(levels[mip]);
	free(levels);
	free(uploads);
	return NULL;
}
