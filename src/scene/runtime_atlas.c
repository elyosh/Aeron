#include "aeron/scene/runtime_atlas.h"

#include "aeron/atlas_pack.h"
#include "rgba_upload.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define AERON_RUNTIME_ATLAS_GUTTER 2
#define AERON_RUNTIME_ATLAS_MAX_DIMENSION 4096

void Aeron_RuntimeAtlasRelease(AeronRuntimeAtlas *atlas) {
	if (!atlas) return;
	for (int page = 0; page < atlas->layout.page_count; ++page) {
		if (atlas->pages[page].texture)
			Aeron_DestroyTexture(atlas->pages[page].texture);
	}
	free(atlas->pages);
	Aeron_SpriteAtlasFree(&atlas->layout);
	memset(atlas, 0, sizeof *atlas);
}

static bool runtime_atlas_allocate(AeronRuntimeAtlas *atlas,
								   int frame_count, int page_count) {
	atlas->layout.frames = calloc((size_t)frame_count,
								  sizeof *atlas->layout.frames);
	atlas->layout.origin_x = calloc((size_t)frame_count,
									sizeof *atlas->layout.origin_x);
	atlas->layout.origin_y = calloc((size_t)frame_count,
									sizeof *atlas->layout.origin_y);
	atlas->layout.ids = calloc((size_t)frame_count, sizeof *atlas->layout.ids);
	atlas->layout.pages = calloc((size_t)frame_count, sizeof *atlas->layout.pages);
	atlas->layout.classic_w = calloc((size_t)frame_count,
									sizeof *atlas->layout.classic_w);
	atlas->layout.classic_h = calloc((size_t)frame_count,
									sizeof *atlas->layout.classic_h);
	atlas->pages = calloc((size_t)page_count, sizeof *atlas->pages);
	return atlas->layout.frames && atlas->layout.origin_x &&
		   atlas->layout.origin_y && atlas->layout.ids &&
		   atlas->layout.pages && atlas->layout.classic_w &&
		   atlas->layout.classic_h && atlas->pages;
}

bool Aeron_RuntimeAtlasBuild(
		AeronRuntimeAtlas *out, AeronCommandBuffer *cmd,
		const AeronRuntimeAtlasFrame *frames, int frame_count,
		const AeronRuntimeAtlasOptions *options) {
	AeronAtlasImage *images = NULL;
	AeronCpuAtlas cpu = {0};
	if (!out || !cmd || !frames || frame_count <= 0 || !options ||
		(options->alpha_mode != AERON_IMAGE_ALPHA_STRAIGHT &&
		 options->alpha_mode != AERON_IMAGE_ALPHA_PREMULTIPLIED))
		return false;
	memset(out, 0, sizeof *out);
	images = calloc((size_t)frame_count, sizeof *images);
	if (!images) return false;
	for (int index = 0; index < frame_count; ++index) {
		const AeronRuntimeAtlasFrame *frame = &frames[index];
		if (!frame->rgba || frame->width <= 0 || frame->height <= 0 ||
			frame->width > INT16_MAX || frame->height > INT16_MAX ||
			frame->anchor_x < INT16_MIN || frame->anchor_x > INT16_MAX ||
			frame->anchor_y < INT16_MIN || frame->anchor_y > INT16_MAX)
			goto failed;
		images[index] = (AeronAtlasImage){
				.rgba = frame->rgba,
				.width = frame->width,
				.height = frame->height,
		};
	}
	const AeronAtlasBuildOptions build_options = {
			.gutter = AERON_RUNTIME_ATLAS_GUTTER,
			.max_dimension = AERON_RUNTIME_ATLAS_MAX_DIMENSION,
			.max_pages = 0,
	};
	if (!Aeron_AtlasBuildRgba8(images, frame_count, &build_options, &cpu) ||
		cpu.page_count <= 0 ||
		!runtime_atlas_allocate(out, frame_count, cpu.page_count))
		goto failed;
	out->layout.frame_count = frame_count;
	out->layout.page_count = cpu.page_count;
	out->layout.atlas_w = cpu.pages[0].width;
	out->layout.atlas_h = cpu.pages[0].height;
	for (int index = 0; index < frame_count; ++index) {
		out->layout.frames[index] = (AeronSpriteRect){
				(float)images[index].x, (float)images[index].y,
				(float)images[index].width, (float)images[index].height,
		};
		out->layout.ids[index] = frames[index].id;
		out->layout.pages[index] = (int16_t)images[index].page;
		out->layout.origin_x[index] = (int16_t)frames[index].anchor_x;
		out->layout.origin_y[index] = (int16_t)frames[index].anchor_y;
		out->layout.classic_w[index] = (int16_t)frames[index].width;
		out->layout.classic_h[index] = (int16_t)frames[index].height;
	}
	for (int page = 0; page < cpu.page_count; ++page) {
		out->pages[page].texture = aeron_scene_upload_rgba8(
				cmd, cpu.pages[page].rgba, cpu.pages[page].width,
				cpu.pages[page].height, (size_t)cpu.pages[page].width * 4,
				options->format, options->color_space, options->alpha_mode,
				options->generate_mips, options->debug_name);
		if (!out->pages[page].texture) goto failed;
		out->pages[page].width = cpu.pages[page].width;
		out->pages[page].height = cpu.pages[page].height;
	}
	free(images);
	Aeron_AtlasBuildFree(&cpu);
	return true;

failed:
	free(images);
	Aeron_AtlasBuildFree(&cpu);
	Aeron_RuntimeAtlasRelease(out);
	return false;
}
