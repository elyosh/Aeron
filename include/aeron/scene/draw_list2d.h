#ifndef AERON_SCENE_DRAW_LIST2D_H
#define AERON_SCENE_DRAW_LIST2D_H

/*
 * aeron_scene 2D blit layer.
 *
 * The implementation owns the shared rect-blit shader pair
 * (scene_blit.vert/.frag), the 4-corner projective variant
 * (scene_blit4.vert), pipeline/sampler caches, and the storage-backed
 * instanced quad path every 2D sprite, text, and primitive draw uses.
 *
 * The z-ordered AeronDrawList2D record API provides sprite, quad4,
 * primitive, and text submissions on top of this layer.
 *
 * Process-wide singleton: Aeron owns the device, so one shader pair and
 * cache set serve every live draw list. GPU objects are created lazily.
 */

#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AERON_BLIT2D_BLEND_NONE = 0, /* opaque overwrite */
	AERON_BLIT2D_BLEND_PMA  = 1, /* premultiplied-alpha over */
} AeronBlit2DBlend;

typedef enum {
	AERON_BLIT2D_FILTER_NEAREST = 0,
	AERON_BLIT2D_FILTER_LINEAR  = 1,
} AeronBlit2DFilter;

/* ===================================================================
 * AeronDrawList2D — retained z-ordered 2D record list (C3a2).
 *
 * Games translate their snapshot 2D channels (sprites, paint prims,
 * later text runs) into records in z order. Prepare() uploads the
 * instance collection before rendering; Render() opens one pass on
 * the target and replays the prepared records. Two clear
 * modes support full-frame redraw (CLEAR) and dirty-rect/incremental
 * redraw models (LOAD — the target persists across frames).
 *
 * Coordinate contract: dst rects and quad corners are in TARGET
 * PIXELS, origin top-left, +Y down (the natural 2D frame games emit
 * in); the list converts to the blit shader's NDC internally. Any
 * classic→viewport scaling stays in the game translators.
 * ================================================================= */

typedef enum {
	AERON_DRAWLIST2D_CLEAR = 0, /* clear the target to clear_rgba */
	AERON_DRAWLIST2D_LOAD  = 1, /* keep contents (incremental redraw) */
} AeronDrawList2DClearMode;

/* One textured sprite. `tint` is a literal RGBA multiplier; use opaque
 * white for an untinted sprite. `bias` adds alpha-weighted RGB
 * (color-fade effects). The trapezoid fields are top-edge horizontal
 * insets in PIXELS plus projective w; all-zero means a plain rect.
 * `scissor` in target pixels; zero width/height = none. */
typedef struct AeronDrawList2DSprite {
	AeronTexture*     texture;
	float             src_u0, src_v0, src_u1, src_v1; /* UV, v=0 top */
	float             dst_x, dst_y, dst_w, dst_h;     /* target px */
	float             tint[4];
	float             bias[4];
	AeronBlit2DBlend  blend;
	AeronBlit2DFilter filter;
	float             trap_top_dx_left_px;
	float             trap_top_dx_right_px;
	float             trap_top_w;
	AeronRectI        scissor;
} AeronDrawList2DSprite;

/* Free-corner projective quad. corners[0..3] = TL, TR, BL, BR in
 * TARGET PIXELS; q is clip W per corner (1.0 for parallelograms — 0 is
 * promoted to 1.0 so zero-init works). ndc_depth is used only when
 * depth_test is set. Same tint/bias/scissor rules as sprites. */
typedef struct AeronDrawList2DQuad4 {
	AeronTexture*     texture;
	float             corners[4][4]; /* [TL,TR,BL,BR]{pos.x_px, pos.y_px, u, v} */
	float             q[4];
	float             ndc_depth[4];
	float             tint[4];
	float             bias[4];
	AeronBlit2DBlend  blend;
	AeronBlit2DFilter filter;
	int               depth_test;
	AeronRectI        scissor;
} AeronDrawList2DQuad4;

typedef struct AeronDrawList2D AeronDrawList2D;

/* record_cap 0 = default (4096). Over-capacity submissions are
 * dropped with a once-per-frame log. */
AeronDrawList2D* AeronDrawList_Create(int record_cap);
void             AeronDrawList_Destroy(AeronDrawList2D* list);

/* Frame start: latch the optional target + clear mode, reset records.
 * `target_w/h` are the target's pixel dims (used for the px→NDC
 * conversion); `clear_rgba` NULL = transparent black. `target` may be
 * NULL when the list will only be rendered through RenderIntoPass(). */
void AeronDrawList_Begin(AeronDrawList2D* list, AeronRenderTarget* target, int target_w,
						 int target_h, AeronDrawList2DClearMode clear_mode,
						 const float clear_rgba[4]);

void AeronDrawList_AddSprite(AeronDrawList2D* list, const AeronDrawList2DSprite* sprite);
void AeronDrawList_AddQuad4(AeronDrawList2D* list, const AeronDrawList2DQuad4* quad);

/* Arbitrary line segment as ONE record (a rotated quad on the shared
 * white texture) — wireframe-heavy consumers (holograms, map grids)
 * must not expand diagonals into per-pixel fills. Endpoints in target
 * px; `thickness_px` is the full stroke width. */
void AeronDrawList_AddLine(AeronDrawList2D* list, float x0, float y0, float x1, float y1,
						   float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
						   const AeronRectI* scissor);

/* Projected reversed-Z line. Endpoint clip_w values preserve perspective
 * interpolation; clip_z is normally the camera near distance. The line reads
 * scene depth with GREATER_EQUAL and never writes it. */
void AeronDrawList_AddProjectedLine(AeronDrawList2D* list, float x0, float y0, float clip_w0,
									float x1, float y1, float clip_w1, float clip_z,
									float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
									const AeronRectI* scissor);

/* Solid prims — drawn via the shared 1x1 white texture + tint.
 * AddFrame expands into four fills of `thickness_px`. `rgba` is the
 * PMA-space color when blend is PMA (rgb premultiplied by a). */
void AeronDrawList_AddFill(AeronDrawList2D* list, float x, float y, float w, float h,
						   const float rgba[4], AeronBlit2DBlend blend,
						   const AeronRectI* scissor);
void AeronDrawList_AddFrame(AeronDrawList2D* list, float x, float y, float w, float h,
							float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
							const AeronRectI* scissor);

/* Compacts and uploads rect/sprite instances. `cmd` must have no active
 * render or compute pass. Adding records after this call invalidates the
 * prepared generation. Empty and quad4-only lists prepare successfully. */
int AeronDrawList_Prepare(AeronDrawList2D* list, AeronCommandBuffer* cmd);

/* Prepare and replay the records into one render pass on the Begin target.
 * `cmd` must have no active pass. */
void AeronDrawList_Render(AeronDrawList2D* list, AeronCommandBuffer* cmd);

/* Replay an already-prepared generation into an active render pass. The
 * caller owns the pass and supplies its actual target so pipeline format
 * selection remains correct for direct swapchain presentation. The
 * draw-list clear mode is ignored. */
void AeronDrawList_RenderIntoPass(AeronDrawList2D* list, AeronCommandBuffer* cmd,
								  AeronRenderPass* pass, AeronRenderTarget* target);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_DRAW_LIST2D_H */
