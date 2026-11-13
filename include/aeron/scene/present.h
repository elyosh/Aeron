#ifndef AERON_SCENE_PRESENT_H
#define AERON_SCENE_PRESENT_H

/*
 * aeron_scene tonemap/present controls. Process-wide runtime state for
 * the final tonemap pass: operator and AgX-look selection, the parametric
 * AgX EOTF exponent, ACES pre-exposure, and the bloom present kernel.
 */

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { AERON_SCENE_TONEMAP_ACES = 0, AERON_SCENE_TONEMAP_AGX_PARAMETRIC = 1, AERON_SCENE_TONEMAP_COUNT };

enum { AERON_SCENE_AGX_LOOK_BASE = 0, AERON_SCENE_AGX_LOOK_PUNCHY = 1, AERON_SCENE_AGX_LOOK_COUNT };

enum {
	AERON_SCENE_BLOOM_KERNEL_1_TAP = 0,
	AERON_SCENE_BLOOM_KERNEL_4_TAP = 1,
	AERON_SCENE_BLOOM_KERNEL_COUNT
};

typedef struct AeronSceneTonemapSettings {
	int   tonemap_operator;
	int   agx_look;
	float agx_eotf_exponent;
	float agx_punchy_power;
	float agx_punchy_saturation;
	/* The Punchy saturation of very bright pixels: it ramps from
	 * agx_punchy_saturation to agx_highlight_saturation between these EV of
	 * scene luminance. */
	float agx_highlight_saturation;
	float agx_highlight_ev_start;
	float agx_highlight_ev_end;
	/* How much of the brightest pixels keeps its hue (0 to 1). It ramps up
	 * from 0 over the same EV. */
	float agx_highlight_hue;
	float aces_pre_exposure;
} AeronSceneTonemapSettings;

int  AeronScenePresent_TonemapOp(void);
void AeronScenePresent_SetTonemapOp(int op);
void AeronScenePresent_ToggleTonemapOp(void);

int  AeronScenePresent_AgxLook(void);
void AeronScenePresent_SetAgxLook(int look);

/* Punchy-look ASC-CDL controls. Power defaults to 1.1 and is clamped to
 * [0.5, 2.0]; saturation defaults to 1.3 and is clamped to [0.0, 2.0]. */
float AeronScenePresent_AgxPunchyPower(void);
void  AeronScenePresent_SetAgxPunchyPower(float v);
float AeronScenePresent_AgxPunchySaturation(void);
void  AeronScenePresent_SetAgxPunchySaturation(float v);
/* Punchy saturation of very bright pixels, clamped to [0.0, 3.0]; default
 * 2.5. The ramp runs from `start` to `end` EV of scene luminance, -2 to 2 by
 * default; `end` is kept above `start`. */
float AeronScenePresent_AgxHighlightSaturation(void);
void  AeronScenePresent_SetAgxHighlightSaturation(float v);
void  AeronScenePresent_AgxHighlightRange(float* start, float* end);
void  AeronScenePresent_SetAgxHighlightRange(float start, float end);
/* How much of the brightest pixels keeps its hue, clamped to [0.0, 1.0];
 * default 0.6. */
float AeronScenePresent_AgxHighlightHue(void);
void  AeronScenePresent_SetAgxHighlightHue(float v);

/* Parametric AgX tail exponent, clamped to [1.8, 2.6]; default 2.2. */
float AeronScenePresent_EotfExponent(void);
void  AeronScenePresent_SetEotfExponent(float v);

/* ACES pre-exposure, clamped to [1.0, 3.0]; default 1.6. */
float AeronScenePresent_AcesExposure(void);
void  AeronScenePresent_SetAcesExposure(float v);

/* Apply a complete validated renderer configuration. Individual setters
 * remain available for interactive debug tools. */
void AeronScenePresent_ApplySettings(const AeronSceneTonemapSettings* settings);

/* Camera exposure. Scene values are in W/m^2 at 683 lm/W, so a value of 1 is
 * 683 cd/m^2. A camera at ISO 100 and exposure value `ev100` saturates at
 * 1.2 * 2^ev100 cd/m^2, which the exposure maps to 1:
 * 683 / (1.2 * 2^ev100). At AERON_SCENE_UNIT_EXPOSURE_EV100 the exposure
 * is 1. */
#define AERON_SCENE_UNIT_EXPOSURE_EV100 9.152707f
float AeronScenePresent_ExposureFromEv100(float ev100);

int  AeronScenePresent_BloomKernel(void);
void AeronScenePresent_SetBloomKernel(int mode);

/* The tonemap/present chain: SDR + HDR fragment variants prebuilt
 * against `target_format` (the RT the game composes the tonemapped
 * frame into). Draw picks the variant from Aeron_OutputHdrEnabled()
 * and derives the HDR peak scale from the display headroom.
 *
 * The 24-float cbuffer layout matches scene_tonemap(.hdr).frag:
 *   [ 0..3] bloom_params   = (intensity, filter_uv_x, filter_uv_y, bar_y_uv)
 *   [ 4..7] tonemap_params = (exposure, op, hdr_peak, sdr_to_scrgb)
 *   [ 8..11] tint          = (r, g, b, PMA coverage alpha)
 *   [12..15] misc          = (bloom_kernel, eotf_exp, aces_exp,
 *                             src_coverage)
 *   [16..19] agx_params     = (look, punchy_power, punchy_saturation,
 *                              exposure texture bound)
 *   [20..23] agx_highlight  = (highlight_saturation, ev_start, ev_end,
 *                              highlight_hue)
 *   [24..27] debug_view     = (view, max channel, reserved, reserved)
 *   [28..31] debug_weights  = (edge, sky, near, near depth) */
typedef struct AeronScenePresentChain AeronScenePresentChain;

/* A debug view drawn in place of the image, for tuning the exposure. */
typedef enum AeronScenePresentDebugView {
	AERON_SCENE_PRESENT_IMAGE = 0,
	/* The exposed luminance in stops around mid-grey (0.18), one colour per
	 * stop, white from AgX's white up, where the image clips. */
	AERON_SCENE_PRESENT_FALSE_COLOR = 1,
	/* The image meter's weight of each pixel: black where it isn't counted,
	 * blue for the sky, orange for near geometry, green for other geometry,
	 * as bright as the weight. */
	AERON_SCENE_PRESENT_METER_WEIGHT = 2,
	/* The image the tonemap receives: the scene with its bloom, multiplied
	 * by the exposure. There is no tonemap and no clamp, and alpha is 1.
	 * Draw it into a float target to read the values back. */
	AERON_SCENE_PRESENT_BEFORE_TONEMAP = 3,
} AeronScenePresentDebugView;

typedef struct AeronScenePresentDebug {
	AeronScenePresentDebugView view;
	/* AERON_SCENE_PRESENT_METER_WEIGHT: the scene's depth and the meter's
	 * settings (AeronScene_GetMeterDebug). */
	AeronTexture* depth;
	float         near_depth; /* geometry with a greater depth is near */
	float         edge_weight;
	float         sky_weight;
	float         near_weight;
	int           max_channel;
} AeronScenePresentDebug;

AeronScenePresentChain* AeronScenePresentChain_Create(AeronTextureFormat target_format);
void                    AeronScenePresentChain_Destroy(AeronScenePresentChain* chain);

/* The debug view the chain draws from now on; NULL draws the image. */
void AeronScenePresentChain_SetDebug(AeronScenePresentChain* chain, const AeronScenePresentDebug* debug);

/* Record the fullscreen tonemap draw into `pass` (opened on a target of
 * the chain's format). `bloom_tex` NULL => zero bloom contribution
 * (scene is bound in its place to satisfy the sampler slot).
 * `exposure` multiplies the scene and its bloom before the tonemap; see
 * AeronScenePresent_ExposureFromEv100. `exposure_tex`, when not NULL, is
 * a 1x1 texture whose R also multiplies them: the exposure a scene meters
 * on the GPU (AeronScene_ExposureTexture).
 * The four-tap bloom filter samples one pixel away on each axis;
 * (rt_w, rt_h) are the source dimensions.
 * `src_coverage` != 0 weights the PMA coverage by the scene texel's
 * own alpha — for PiP targets with a transparent background; pass 0
 * for a full-frame present. */
void AeronScenePresentChain_Draw(AeronScenePresentChain* chain, AeronRenderPass* pass,
								 AeronTexture* scene_tex, AeronSampler* sampler, AeronTexture* bloom_tex,
								 float bloom_intensity, float exposure, AeronTexture* exposure_tex, int rt_w,
								 int rt_h, float bar_y_uv, const float tint[4], int src_coverage);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_PRESENT_H */
