/*
 * channel_atlas — per-channel atlas planning + materialization for
 * aeron_gltf_cook.
 *
 * Each PBR channel (base_color / normal / metallic_roughness /
 * emissive) gets its own atlas because materials author them
 * independently and the runtime samples them with different transfer
 * functions (sRGB vs linear). One ChannelAtlas struct per channel.
 *
 * A channel is a texture array of equally sized pages. Sources larger
 * than half a page get a dedicated layer, resampled to the full page. So
 * does a source whose `dedicated` is set before packing.
 * The others are packed onto shared pages, each inside a private
 * footprint of `gutter` texels on every side, aligned to `gutter` and
 * filled by REPEAT so mip levels up to log2(gutter) never mix two
 * sources.
 *
 * With `one_per_page` set before packing, every source is dedicated
 * instead: one page each, resampled to fill it, with no gutter. Each page
 * then holds only its source, so the texture wraps at the page's edge and
 * every mip level is exact. The atlas has the complete mip chain.
 *
 * Lifecycle:
 *   channel_atlas_init(&ca, channel_index)
 *   for each distinct source → channel_atlas_add_rect(&ca, key, src_w,
 *                                                     src_h, src_rgba,
 *                                                     max_texture)
 *   channel_atlas_pack(&ca, page_size, gutter)
 *   channel_atlas_materialize(&ca)        — allocates + blits
 *   ... use ca.rgba, ca.width, ca.height, ca.page_count ...
 *   channel_atlas_free(&ca)
 *
 * A source can also be added by its size alone, with
 * channel_atlas_add_deferred_rect: channel_atlas_materialize_lazy then
 * fetches its pixels when it fills the page, and releases them right after,
 * so that a cook doesn't hold every source at once.
 *
 * Source pixels are NOT copied into the atlas struct unless they had to
 * be downscaled — the caller owns them; `channel_atlas_materialize`
 * consults them in place.
 */
#ifndef AERON_GLTF_COOK_CHANNEL_ATLAS_H
#define AERON_GLTF_COOK_CHANNEL_ATLAS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	AERON_GLTF_COOK_CHANNEL_BASE_COLOR = 0,
	AERON_GLTF_COOK_CHANNEL_NORMAL,
	AERON_GLTF_COOK_CHANNEL_METALLIC_ROUGHNESS,
	AERON_GLTF_COOK_CHANNEL_EMISSIVE,
	AERON_GLTF_COOK_CHANNEL_COUNT
} AeronGltfCookChannel;

/* Mip limit of a dedicated layer: the runtime does not clamp it. */
#define CHANNEL_ATLAS_UNCLAMPED_LOD 15

/* One sub-rect: one distinct source image + its final atlas position. */
typedef struct ChannelRect {
	const void*    key; /* caller's identity for deduplication */
	int            src_w, src_h;
	const uint8_t* src_rgba; /* tightly-packed RGBA8, borrowed or `owned`; NULL until fetched when deferred */
	uint8_t*       owned;    /* downscaled copy, freed by channel_atlas_free */
	/* A deferred source's size, before any downscale; 0 otherwise. */
	int full_w, full_h;
	/* Output (filled by channel_atlas_pack). */
	int  page;
	int  x, y;      /* top-left of the source in its page; 0 for a dedicated layer */
	int  max_lod;   /* deepest mip level sampled; CHANNEL_ATLAS_UNCLAMPED_LOD when dedicated */
	bool dedicated; /* resampled to fill the whole page; set by the caller or by the pack */
} ChannelRect;

typedef struct ChannelAtlas {
	int          channel; /* AeronGltfCookChannel */
	ChannelRect* rects;
	int          rect_count;
	int          rect_capacity;

	int      width, height; /* page dimensions; set by channel_atlas_pack */
	int      page;          /* the power-of-two page size P; set by channel_atlas_pack */
	int      page_count;    /* array layers; set by channel_atlas_pack */
	uint8_t* rgba;          /* page_count pages of width*height*4 bytes; set by materialize */

	int mip_count; /* level count to generate; 0 = the complete chain */
	int gutter;    /* footprint gutter and alignment, in texels */

	/* Set before channel_atlas_pack to pack onto pages of exactly this
	 * size (multiples of the gutter), as another atlas whose array this
	 * one joins; 0 packs as described there. */
	int fixed_width, fixed_height;

	/* Set before channel_atlas_pack to give each source its own page,
	 * resampled to fill it. The pages are fixed_width × fixed_height when
	 * set, or the largest source's size rounded up to a multiple of 4 (a
	 * BC block). */
	bool one_per_page;
} ChannelAtlas;

void channel_atlas_init(ChannelAtlas* ca, int channel);
void channel_atlas_free(ChannelAtlas* ca);

/* Append a sub-rect, box-downscaling it first when either axis exceeds
 * `max_texture` (aspect ratio kept). Returns its index, or -1 on OOM. */
int channel_atlas_add_rect(ChannelAtlas* ca, const void* key, int src_w, int src_h, const uint8_t* src_rgba,
						   int max_texture);

/* Plan the pages. The page size P is `page_size` raised to the next power
 * of two of the largest source. A single shared page with no dedicated
 * layer is trimmed to its occupied bounds; otherwise every page is P×P, or
 * (P + P/16)² when there is no dedicated layer and that size needs fewer
 * texels (the gutters keep power-of-two sources from tiling P).
 * `page_size` must be a power of two ≥ 128 and ≥ 8 × `gutter`, and
 * `gutter` a power of two in [4, 32]. With fixed_width/fixed_height, every
 * page has that size instead, a source over half a page must find a P×P
 * page, and a source the caller marked dedicated is resampled to the fixed
 * size. Returns false on invalid arguments, OOM or a source that does not
 * fit. */
bool channel_atlas_pack(ChannelAtlas* ca, int page_size, int gutter);

/* Widens `*width` × `*height` to the page size that `one_per_page` gives
 * `ca`'s sources without a fixed size: the largest source, rounded up to a
 * multiple of 4. Start from 0 × 0; call it for each atlas that shares the
 * pages. */
void channel_atlas_one_per_page_extent(const ChannelAtlas* ca, int* width, int* height);

/* The fewest texels that channel_atlas_pack, without `one_per_page`, can
 * give `ca`'s sources, however well it packs them. Each source counts its
 * footprint, or the square power-of-two page of at least `page_size` that
 * holds it when that is smaller (a source over half a page is dedicated). */
int64_t channel_atlas_texel_lower_bound(const ChannelAtlas* ca, int page_size, int gutter);

/* Allocate ca.rgba and fill every footprint and dedicated layer. Returns
 * false on OOM. */
bool channel_atlas_materialize(ChannelAtlas* ca);

/* Append a sub-rect whose pixels come later: `src_w` x `src_h` is the
 * source's size, and a source over `max_texture` is downscaled when it is
 * fetched. Returns its index, or -1 on OOM. */
int channel_atlas_add_deferred_rect(ChannelAtlas* ca, const void* key, int src_w, int src_h, int max_texture);

/* Gives the pixels of the deferred source `key` (`*w` x `*h`), and releases
 * them once they are copied. */
typedef bool (*ChannelAtlasFetch)(void* context, const void* key, const uint8_t** rgba, int* w, int* h);
typedef void (*ChannelAtlasRelease)(void* context, const void* key);

/* channel_atlas_materialize, fetching each deferred source when it fills
 * its footprint, and releasing it right after. Returns false on OOM, or
 * when a source can't be fetched or has another size. */
bool channel_atlas_materialize_lazy(ChannelAtlas* ca, ChannelAtlasFetch fetch, ChannelAtlasRelease release,
									void* context);

/* Convenience: derive normalized UV transform for a rect (offset_u,
 * offset_v, scale_u, scale_v) such that
 *   atlas_uv = vertex_uv * scale + offset
 * lands inside the rect on its page. */
void channel_atlas_rect_uv_transform(const ChannelAtlas* ca, const ChannelRect* r, float out_offset[2],
									 float out_scale[2]);

#ifdef __cplusplus
}
#endif

#endif
