#ifndef AERON_SCENE_BILLBOARD_H
#define AERON_SCENE_BILLBOARD_H

/*
 * Scene billboards: world-space fans queued per frame via
 * AeronScene_AddBillboard and drawn inside AeronScene_Render at their
 * stage (SKY under the meshes, OVERLAY over them), batched into one
 * VB / one draw per consecutive
 * (texture, blend) run, aux attachments write-masked, with optional
 * flat depth bias and motion-blur velocity stamping.
 *
 * No game vocabulary lives here — engine glows, backdrops, flares and
 * sparks are game-side policies that submit fans.
 *
 * Shaders: scene_billboard3d(.vert/.frag), scene_billboard3d_vel.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronSceneBillboardBlend {
	AERON_SCENE_BILLBOARD_BLEND_ALPHA = 0, /* src*a + dst*(1-a) — STRAIGHT-alpha textures */
	AERON_SCENE_BILLBOARD_BLEND_ADDITIVE,  /* src + dst */
	/* src + dst*(1-a) — PREMULTIPLIED-alpha textures (the imgbake KTX2
	 * convention: color channels carry rgb*a). Submitters modulating
	 * with a vertex tint must pre-scale the tint's RGB by the tint's
	 * alpha to keep the modulate equivalent to the straight form. */
	AERON_SCENE_BILLBOARD_BLEND_PMA,
} AeronSceneBillboardBlend;

/* Pass stage the batched billboard draws in:
 *   SKY     — start of the color pass, under all mesh instances. The
 *             pipeline forces reversed-Z far depth, so callers choose a
 *             finite distance only for directional projection. Never
 *             velocity-stamped.
 *   OVERLAY — after the opaque mesh walk (glows, explosions, sprites);
 *             depth-tested against the scene, no write; participates
 *             in the velocity prepass when prev corners are given.
 *   LENS    — lens-artifact quads (flare trains): drawn in a dedicated
 *             pass AFTER the scene passes (and the motion-blur
 *             resolve) into the final scene RT, before the game's
 *             bloom/present consume it. No depth test — always on
 *             top — but the whole quad's color is scaled by the
 *             visibility of `anchor_world` sampled from the scene
 *             depth buffer (soft 5x5 kernel; anchors off-frame or
 *             behind the camera fade to zero). Never velocity-
 *             stamped; depth_bias_view ignored. */
typedef enum AeronSceneBillboardStage {
	AERON_SCENE_BILLBOARD_STAGE_SKY = 0,
	AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
	AERON_SCENE_BILLBOARD_STAGE_LENS,
} AeronSceneBillboardStage;

/* One batched scene billboard: a fan (derived center + 4 rim corners)
 * of WORLD-space positions. Build the corners on the camera right/up
 * axes so all four share one view depth — the quad then projects as an
 * exact screen-aligned rectangle (classic flat-sprite semantics).
 *
 * colors are linear RGBA, HDR values allowed (emissive boost).
 * center_color optionally overrides the fan-center color (XWA glow
 * core); NULL = corner average. depth_bias_view pushes the TEST depth
 * toward the camera in view units without moving the sprite (classic
 * flat-vs-mesh rules); 0 = none. prev_corners enables motion-blur
 * velocity stamping in the prepass; NULL = no own-velocity.
 *
 * Draw order is submission order within a stage; consecutive
 * submissions sharing (texture, blend) batch into one draw. */
typedef struct AeronSceneBillboardDesc {
	AeronTexture*            texture;
	AeronSceneBillboardBlend blend;
	AeronSceneBillboardStage stage;
	float                    corners[4][3];
	float                    uv[4][2];
	float                    colors[4][4];
	const float*             center_color;      /* RGBA or NULL */
	float                    depth_bias_view;
	const float (*prev_corners)[3];             /* [4][3] or NULL */
	/* LENS stage only: the flare SOURCE point (world space) whose
	 * depth-buffer visibility scales the quad's color. All quads of
	 * one flare train share the same anchor so the train fades as a
	 * unit. Ignored by SKY/OVERLAY. */
	float                    anchor_world[3];
} AeronSceneBillboardDesc;

/* Queue one billboard for this frame (reset by AeronScene_Begin).
 * Drawn inside AeronScene_Render at the desc's stage. */
void AeronScene_AddBillboard(AeronScene3D* scene, const AeronSceneBillboardDesc* desc);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_BILLBOARD_H */
