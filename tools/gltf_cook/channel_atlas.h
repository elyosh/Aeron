/*
 * channel_atlas — per-channel atlas planning + materialization for
 * aeron_gltf_cook.
 *
 * Each PBR channel (base_color / normal / metallic_roughness /
 * emissive) gets its own atlas because materials author them
 * independently and the runtime samples them with different transfer
 * functions (sRGB vs linear). One ChannelAtlas struct per channel.
 *
 * Lifecycle:
 *   channel_atlas_init(&ca, channel_index)
 *   for each material → channel_atlas_add_rect(&ca, mat_idx, src_w,
 *                                              src_h, src_rgba)
 *   channel_atlas_pack(&ca, opts)         — runs skyline_pack
 *   channel_atlas_materialize(&ca)        — allocates + blits + bleeds
 *   ... use ca.rgba, ca.width, ca.height ...
 *   channel_atlas_free(&ca)
 *
 * Source pixels are NOT copied into the atlas struct — the caller owns
 * them; `channel_atlas_materialize` consults them in place.
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

/* One sub-rect: which material owns it + source image + final atlas
 * position. `src_rgba` is borrowed (not freed by channel_atlas_free). */
typedef struct ChannelRect {
	uint32_t       mat_idx; /* index into the source cgltf_data->materials */
	int            src_w, src_h;
	const uint8_t* src_rgba; /* tightly-packed RGBA8, borrowed */
	/* Output (filled by channel_atlas_pack). */
	int x, y; /* top-left of the unpadded image in the atlas */
} ChannelRect;

typedef struct ChannelAtlas {
	int          channel; /* AeronGltfCookChannel */
	ChannelRect* rects;
	int          rect_count;
	int          rect_capacity;

	int      width, height; /* set by channel_atlas_pack */
	uint8_t* rgba;          /* width*height*4 bytes; set by materialize */

	int mip_count; /* generated mip pyramid level count */
	int pad;       /* gutter texels around each sub-rect */
} ChannelAtlas;

void channel_atlas_init(ChannelAtlas* ca, int channel);
void channel_atlas_free(ChannelAtlas* ca);

/* Append a sub-rect. Returns false on OOM. */
bool channel_atlas_add_rect(ChannelAtlas* ca, uint32_t mat_idx, int src_w, int src_h,
							const uint8_t* src_rgba);

/* Plan: pick atlas width (power-of-2 square up to `max_atlas_size`),
 * compute per-rect positions via skyline pack, set ca.width/.height.
 * `pad` is the gutter around every sub-rect (caller sets, typically
 * derived from desired mip depth). Returns false if any rect is too
 * large to fit max_atlas_size or on OOM. */
bool channel_atlas_pack(ChannelAtlas* ca, int max_atlas_size, int pad);

/* Allocate ca.rgba (width*height*4) and blit every sub-rect into its
 * position, periodically extending it into its private gutter. Returns
 * false on OOM. */
bool channel_atlas_materialize(ChannelAtlas* ca);

/* Convenience: derive normalized UV transform for a rect (offset_u,
 * offset_v, scale_u, scale_v) such that
 *   atlas_uv = vertex_uv * scale + offset
 * lands inside the rect. */
void channel_atlas_rect_uv_transform(const ChannelAtlas* ca, const ChannelRect* r, float out_offset[2],
									 float out_scale[2]);

#ifdef __cplusplus
}
#endif

#endif
