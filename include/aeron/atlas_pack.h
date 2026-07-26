#ifndef AERON_ATLAS_PACK_H
#define AERON_ATLAS_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronAtlasRect {
	int w, h;
	int x, y;
	uint32_t key;
} AeronAtlasRect;

typedef struct AeronAtlasImage {
	const uint8_t* rgba;
	int width, height;
	/* Filled by Aeron_AtlasBuildRgba8. */
	int x, y, page;
} AeronAtlasImage;

typedef struct AeronAtlasBuildOptions {
	int gutter;
	int max_dimension;
	/* Zero allows as many pages as necessary. */
	int max_pages;
} AeronAtlasBuildOptions;

typedef struct AeronAtlasPage {
	uint8_t* rgba;
	int width, height;
} AeronAtlasPage;

typedef struct AeronCpuAtlas {
	AeronAtlasPage* pages;
	int page_count;
} AeronCpuAtlas;

/* Deterministic skyline-bottom-left packing. The caller supplies rectangle
 * order; x/y identify the inner rectangle after adding `gutter`. */
int Aeron_AtlasPackRects(AeronAtlasRect* rects, int count, int atlas_width, int gutter);

/* Builds tightly packed RGBA8 pages. Frames in the result retain input order;
 * packing order is internal and deterministic. */
int Aeron_AtlasBuildRgba8(AeronAtlasImage* images, int count,
						  const AeronAtlasBuildOptions* options, AeronCpuAtlas* out);
void Aeron_AtlasBuildFree(AeronCpuAtlas* atlas);

#ifdef __cplusplus
}
#endif

#endif
