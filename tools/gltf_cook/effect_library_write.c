/*
 * Effect image library writer (effect_library.h).
 */
#include "effect_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel_atlas.h"
#include "ktx2_writer.h"

/* Pages of 2048 + 2048/16 texels: with 16-texel gutters, two 1024² or four
 * 512² images fit a row (a 2048² page holds one 1024² image). Every image,
 * up to EFFECT_LIBRARY_IMAGE_MAX, is a shared rect: none needs a dedicated
 * layer. */
#define PACK_SIZE 4096
#define GUTTER 16

static bool write_file(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

static void yaml_rect(FILE* f, const char* name, const ChannelAtlas* ca, int index) {
	const ChannelRect* r = &ca->rects[index];
	const int          w = r->dedicated ? ca->width : r->src_w, h = r->dedicated ? ca->height : r->src_h;
	fprintf(f, "%s: { page: %d, rect: [%d, %d, %d, %d], lod: %d }", name, r->page, r->x, r->y, w, h,
			r->max_lod);
}

static bool encode(const ChannelAtlas* ca, const char* path) {
	uint8_t* buf  = NULL;
	size_t   size = 0;
	bool     ok   = write_ktx2_bc7_with_generated_mips_to_buffer_limited_alpha(
						ca->width, ca->height, ca->page_count, ca->rgba, KTX2_BC7_QUALITY_MED, KTX2_TF_SRGB, true,
						ca->mip_count, KTX2_ALPHA_INDEPENDENT, &buf, &size) &&
					write_file(path, buf, size);
	free(buf);
	return ok;
}

static const EffectLibraryImage* g_sort_images;

static int cmp_key(const void* a, const void* b) {
	return strcmp(g_sort_images[*(const size_t*)a].key, g_sort_images[*(const size_t*)b].key);
}

bool EffectLibrary_Write(const char* tool, const char* out_dir, const char* name, const char* source,
						 const char* inputs, const EffectLibraryImage* images, size_t count,
						 EffectLibraryResult* result) {
	size_t* order         = (size_t*)malloc((count ? count : 1) * sizeof *order);
	int*    layer_rect    = (int*)malloc((count ? count : 1) * sizeof *layer_rect);
	int*    emissive_rect = (int*)malloc((count ? count : 1) * sizeof *emissive_rect);
	bool    ok            = order && layer_rect && emissive_rect;
	for (size_t i = 0; ok && i < count; ++i)
		order[i] = i;
	g_sort_images = images;
	if (ok)
		qsort(order, count, sizeof *order, cmp_key);
	ChannelAtlas layer, emissive;
	channel_atlas_init(&layer, AERON_GLTF_COOK_CHANNEL_BASE_COLOR);
	channel_atlas_init(&emissive, AERON_GLTF_COOK_CHANNEL_EMISSIVE);
	for (size_t k = 0; ok && k < count; ++k) {
		const size_t              i   = order[k];
		const EffectLibraryImage* im  = &images[i];
		const int                 max = im->max_size > 0 ? im->max_size : EFFECT_LIBRARY_IMAGE_MAX;
		if (k > 0 && strcmp(images[order[k - 1]].key, im->key) == 0) {
			fprintf(stderr, "%s: %s: image \"%s\" given twice\n", tool, name, im->key);
			ok = false;
			break;
		}
		layer_rect[i] = emissive_rect[i] = -1;
		if (im->layer)
			layer_rect[i] = channel_atlas_add_rect(&layer, im, im->width, im->height, im->layer, max);
		if (im->emissive)
			emissive_rect[i] =
				channel_atlas_add_rect(&emissive, im, im->width, im->height, im->emissive, max);
		ok = (!im->layer || layer_rect[i] >= 0) && (!im->emissive || emissive_rect[i] >= 0);
	}
	char          path[1024];
	ChannelAtlas* atlases[2] = { &layer, &emissive };
	const char*   suffix[2]  = { "layer", "emissive" };
	for (int a = 0; ok && a < 2; ++a) {
		if (!atlases[a]->rect_count)
			continue;
		atlases[a]->fixed_width = atlases[a]->fixed_height = EFFECT_LIBRARY_PAGE_SIZE;
		ok = channel_atlas_pack(atlases[a], PACK_SIZE, GUTTER) && channel_atlas_materialize(atlases[a]);
		snprintf(path, sizeof path, "%s/%s.%s.ktx2", out_dir, name, suffix[a]);
		ok = ok && encode(atlases[a], path);
		if (!ok)
			fprintf(stderr, "%s: %s: cannot pack or write the %s pages\n", tool, name, suffix[a]);
	}
	const int layer_pages    = layer.rect_count ? layer.page_count : 0;
	const int emissive_pages = emissive.rect_count ? emissive.page_count : 0;
	if (ok) {
		snprintf(path, sizeof path, "%s/%s.yaml", out_dir, name);
		FILE* f = fopen(path, "wb");
		ok      = f != NULL;
		if (f) {
			fprintf(f, "version: 1\n");
			if (source)
				fprintf(f, "source: %s\n", source);
			if (inputs)
				fprintf(f, "inputs: %s\n", inputs);
			fprintf(f, "page_size: %d\nlayer_pages: %d\nemissive_pages: %d\nimages:\n",
					EFFECT_LIBRARY_PAGE_SIZE, layer_pages, emissive_pages);
			for (size_t k = 0; k < count; ++k) {
				const size_t              i  = order[k];
				const EffectLibraryImage* im = &images[i];
				if (layer_rect[i] < 0 && emissive_rect[i] < 0)
					continue;
				fprintf(f, "  \"%s\": { ", im->key);
				if (layer_rect[i] >= 0)
					yaml_rect(f, "layer", &layer, layer_rect[i]);
				if (emissive_rect[i] >= 0) {
					fprintf(f, "%s", layer_rect[i] >= 0 ? ", " : "");
					yaml_rect(f, "emissive", &emissive, emissive_rect[i]);
				}
				if (im->crop)
					fprintf(f, ", crop: [%g, %g, %g, %g]", (double)im->crop[0], (double)im->crop[1],
							(double)im->crop[2], (double)im->crop[3]);
				fprintf(f, " }\n");
			}
			ok = fclose(f) == 0;
		}
		if (!ok)
			fprintf(stderr, "%s: %s: cannot write %s\n", tool, name, path);
	}
	if (result) {
		result->layer_pages    = layer_pages;
		result->emissive_pages = emissive_pages;
	}
	channel_atlas_free(&layer);
	channel_atlas_free(&emissive);
	free(order);
	free(layer_rect);
	free(emissive_rect);
	return ok;
}
