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

typedef enum AeronAtlasAddressMode {
	AERON_ATLAS_ADDRESS_CLAMP = 0,
	AERON_ATLAS_ADDRESS_REPEAT,
} AeronAtlasAddressMode;

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
 * order; every rectangle owns `gutter` texels on all four sides, and x/y
 * identify the inner rectangle. */
int Aeron_AtlasPackRects(AeronAtlasRect* rects, int count, int atlas_width, int gutter);

/* Copies a tightly packed RGBA8 image at inner position x/y and fills its
 * private gutter according to the requested address mode. */
int Aeron_AtlasBlitRgba8(uint8_t* atlas, int atlas_width, int atlas_height,
						 const uint8_t* source, int source_width, int source_height,
						 int x, int y, int gutter, AeronAtlasAddressMode address_mode);

/* Builds tightly packed RGBA8 pages. Each frame owns an edge-extruded gutter
 * on all sides. Frames in the result retain input order; packing order is
 * internal and deterministic. */
int Aeron_AtlasBuildRgba8(AeronAtlasImage* images, int count,
						  const AeronAtlasBuildOptions* options, AeronCpuAtlas* out);
void Aeron_AtlasBuildFree(AeronCpuAtlas* atlas);

#ifdef __cplusplus
}
#endif

#endif
