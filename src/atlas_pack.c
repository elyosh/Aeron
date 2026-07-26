#include "aeron/atlas_pack.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AERON_ATLAS_MIN_WIDTH 64

int Aeron_AtlasPackRects(AeronAtlasRect* rects, int count, int atlas_width, int gutter) {
	if (!rects || count < 0 || atlas_width <= 0 || gutter < 0)
		return -1;
	int* heights = (int*)calloc((size_t)atlas_width, sizeof *heights);
	if (!heights)
		return -1;
	int atlas_height = gutter;
	for (int i = 0; i < count; i++) {
		AeronAtlasRect* rect = &rects[i];
		const int footprint_width = rect->w + gutter;
		const int footprint_height = rect->h + gutter;
		const int last_x = atlas_width - footprint_width;
		if (last_x < 0) {
			free(heights);
			return -1;
		}
		int best_x = 0;
		int best_y = INT_MAX;
		for (int x = 0; x <= last_x; x++) {
			int y = 0;
			for (int column = 0; column < footprint_width; column++) {
				if (heights[x + column] > y)
					y = heights[x + column];
			}
			if (y < best_y) {
				best_x = x;
				best_y = y;
			}
		}
		rect->x = best_x + gutter;
		rect->y = best_y + gutter;
		const int top = best_y + footprint_height;
		for (int column = 0; column < footprint_width; column++)
			heights[best_x + column] = top;
		if (top + gutter > atlas_height)
			atlas_height = top + gutter;
	}
	free(heights);
	return atlas_height;
}

static int atlas_next_power_of_two(int value, int limit) {
	int result = 1;
	while (result < value && result <= limit / 2)
		result *= 2;
	return result >= value && result <= limit ? result : 0;
}

static void atlas_sort_images(const AeronAtlasImage* images, int* order, int count) {
	for (int i = 0; i < count; i++)
		order[i] = i;
	for (int i = 1; i < count; i++) {
		const int key = order[i];
		const int key_side = images[key].width > images[key].height ? images[key].width : images[key].height;
		const int64_t key_area = (int64_t)images[key].width * images[key].height;
		int j = i - 1;
		while (j >= 0) {
			const int other = order[j];
			const int other_side = images[other].width > images[other].height ? images[other].width
																			 : images[other].height;
			const int64_t other_area = (int64_t)images[other].width * images[other].height;
			if (other_side > key_side || (other_side == key_side && other_area >= key_area))
				break;
			order[j + 1] = other;
			j--;
		}
		order[j + 1] = key;
	}
}

static int atlas_plan_page(const AeronAtlasImage* images, const int* order, int first, int count,
						   const AeronAtlasBuildOptions* options, AeronAtlasRect** out_rects,
						   int* out_width, int* out_height) {
	uint64_t area = 0;
	int widest = AERON_ATLAS_MIN_WIDTH;
	for (int i = 0; i < count; i++) {
		const AeronAtlasImage* image = &images[order[first + i]];
		if (!image->rgba || image->width <= 0 || image->height <= 0)
			return 0;
		const uint64_t footprint_width = (uint64_t)image->width + 2u * (uint64_t)options->gutter;
		const uint64_t footprint_height = (uint64_t)image->height + 2u * (uint64_t)options->gutter;
		if (footprint_width > INT_MAX || footprint_height > INT_MAX ||
			footprint_width > UINT64_MAX / footprint_height)
			return 0;
		const uint64_t footprint_area = footprint_width * footprint_height;
		if (area > UINT64_MAX - footprint_area)
			return 0;
		area += footprint_area;
		if ((int)footprint_width > widest)
			widest = (int)footprint_width;
	}
	int width = atlas_next_power_of_two(widest, options->max_dimension);
	if (!width)
		return 0;
	while ((uint64_t)width * width < area) {
		if (width > options->max_dimension / 2)
			break;
		width *= 2;
	}
	AeronAtlasRect* rects = (AeronAtlasRect*)calloc((size_t)count, sizeof *rects);
	if (!rects)
		return 0;
	for (;;) {
		for (int i = 0; i < count; i++) {
			const int source_index = order[first + i];
			rects[i].w = images[source_index].width;
			rects[i].h = images[source_index].height;
			rects[i].key = (uint32_t)source_index;
		}
		const int used_height = Aeron_AtlasPackRects(rects, count, width, options->gutter);
		const int height = used_height >= 0 && used_height <= options->max_dimension
						   ? (used_height + 3) & ~3
						   : 0;
		if (height) {
			*out_rects = rects;
			*out_width = width;
			*out_height = height;
			return 1;
		}
		if (width > options->max_dimension / 2)
			break;
		width *= 2;
	}
	free(rects);
	return 0;
}

static uint8_t* atlas_compose_page(const AeronAtlasImage* images, const AeronAtlasRect* rects,
							   int count, int width, int height) {
	if ((size_t)width > SIZE_MAX / (size_t)height / 4u)
		return NULL;
	uint8_t* rgba = (uint8_t*)calloc((size_t)width * height, 4u);
	if (!rgba)
		return NULL;
	for (int i = 0; i < count; i++) {
		const AeronAtlasImage* image = &images[rects[i].key];
		for (int y = 0; y < image->height; y++) {
			memcpy(rgba + ((size_t)(rects[i].y + y) * width + rects[i].x) * 4u,
				   image->rgba + (size_t)y * image->width * 4u, (size_t)image->width * 4u);
		}
	}
	return rgba;
}

int Aeron_AtlasBuildRgba8(AeronAtlasImage* images, int count,
						  const AeronAtlasBuildOptions* options, AeronCpuAtlas* out) {
	if (!out)
		return 0;
	memset(out, 0, sizeof *out);
	if (!images || count <= 0 || !options || options->gutter < 0 ||
		options->max_dimension < AERON_ATLAS_MIN_WIDTH || options->max_dimension > INT_MAX - 3 ||
		(options->max_dimension & 3) != 0 || options->max_pages < 0)
		return 0;
	out->pages = (AeronAtlasPage*)calloc((size_t)count, sizeof *out->pages);
	int* order = (int*)malloc((size_t)count * sizeof *order);
	if (!out->pages || !order)
		goto failed;
	atlas_sort_images(images, order, count);

	int first = 0;
	while (first < count) {
		const int remaining = count - first;
		int page_images = 0;
		AeronAtlasRect* rects = NULL;
		int width = 0;
		int height = 0;
		if (atlas_plan_page(images, order, first, remaining, options, &rects, &width, &height)) {
			page_images = remaining;
		} else {
			if (options->max_pages && out->page_count + 1 >= options->max_pages)
				goto failed;
			int low = 1;
			int high = remaining - 1;
			while (low <= high) {
				const int candidate = low + (high - low) / 2;
				AeronAtlasRect* probe = NULL;
				int probe_width = 0, probe_height = 0;
				if (atlas_plan_page(images, order, first, candidate, options, &probe,
									&probe_width, &probe_height)) {
					free(rects);
					rects = probe;
					width = probe_width;
					height = probe_height;
					page_images = candidate;
					low = candidate + 1;
				} else {
					high = candidate - 1;
				}
			}
			if (!page_images)
				goto failed;
		}
		uint8_t* rgba = atlas_compose_page(images, rects, page_images, width, height);
		if (!rgba) {
			free(rects);
			goto failed;
		}
		out->pages[out->page_count] = (AeronAtlasPage) { .rgba = rgba, .width = width, .height = height };
		for (int i = 0; i < page_images; i++) {
			const int source_index = (int)rects[i].key;
			images[source_index].x = rects[i].x;
			images[source_index].y = rects[i].y;
			images[source_index].page = out->page_count;
		}
		free(rects);
		out->page_count++;
		first += page_images;
	}
	free(order);
	return 1;

failed:
	free(order);
	Aeron_AtlasBuildFree(out);
	return 0;
}

void Aeron_AtlasBuildFree(AeronCpuAtlas* atlas) {
	if (!atlas)
		return;
	for (int i = 0; i < atlas->page_count; i++)
		free(atlas->pages[i].rgba);
	free(atlas->pages);
	memset(atlas, 0, sizeof *atlas);
}
