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

enum {
	AERON_SCENE_TONEMAP_ACES           = 0,
	AERON_SCENE_TONEMAP_AGX_PARAMETRIC = 1,
	AERON_SCENE_TONEMAP_COUNT
};

enum {
	AERON_SCENE_AGX_LOOK_BASE   = 0,
	AERON_SCENE_AGX_LOOK_PUNCHY = 1,
	AERON_SCENE_AGX_LOOK_COUNT
};

enum {
	AERON_SCENE_BLOOM_KERNEL_1_TAP = 0,
	AERON_SCENE_BLOOM_KERNEL_4_TAP = 1,
	AERON_SCENE_BLOOM_KERNEL_COUNT
};

int  AeronScenePresent_TonemapOp(void);
void AeronScenePresent_SetTonemapOp(int op);
void AeronScenePresent_ToggleTonemapOp(void);

int  AeronScenePresent_AgxLook(void);
void AeronScenePresent_SetAgxLook(int look);

/* Punchy-look ASC-CDL controls. Power defaults to 1.35 and is clamped to
 * [0.5, 2.0]; saturation defaults to 1.4 and is clamped to [0.0, 2.0]. */
float AeronScenePresent_AgxPunchyPower(void);
void  AeronScenePresent_SetAgxPunchyPower(float v);
float AeronScenePresent_AgxPunchySaturation(void);
void  AeronScenePresent_SetAgxPunchySaturation(float v);

/* Parametric AgX tail exponent, clamped to [1.8, 2.6]; default 2.2. */
float AeronScenePresent_EotfExponent(void);
void  AeronScenePresent_SetEotfExponent(float v);

/* ACES pre-exposure, clamped to [1.0, 3.0]; default 1.6. */
float AeronScenePresent_AcesExposure(void);
void  AeronScenePresent_SetAcesExposure(float v);

int  AeronScenePresent_BloomKernel(void);
void AeronScenePresent_SetBloomKernel(int mode);

/* The tonemap/present chain: SDR + HDR fragment variants prebuilt
 * against `target_format` (the RT the game composes the tonemapped
 * frame into). Draw picks the variant from Aeron_OutputHdrEnabled()
 * and derives the HDR peak scale from the display headroom.
 *
 * The 20-float cbuffer layout matches scene_tonemap(.hdr).frag:
 *   [ 0..3] bloom_params   = (intensity, 2/rt_w, 2/rt_h, bar_y_uv)
 *   [ 4..7] tonemap_params = (exposure, op, hdr_peak, sdr_to_scrgb)
 *   [ 8..11] tint          = (r, g, b, PMA coverage alpha)
 *   [12..15] misc          = (bloom_kernel, eotf_exp, aces_exp,
 *                             src_coverage)
 *   [16..19] agx_params     = (look, punchy_power, punchy_saturation,
 *                              reserved) */
typedef struct AeronScenePresentChain AeronScenePresentChain;

AeronScenePresentChain* AeronScenePresentChain_Create(AeronTextureFormat target_format);
void                    AeronScenePresentChain_Destroy(AeronScenePresentChain* chain);

/* Record the fullscreen tonemap draw into `pass` (opened on a target of
 * the chain's format). `bloom_tex` NULL => zero bloom contribution
 * (scene is bound in its place to satisfy the sampler slot).
 * `src_coverage` != 0 weights the PMA coverage by the scene texel's
 * own alpha — for PiP targets with a transparent background; pass 0
 * for a full-frame present. */
void AeronScenePresentChain_Draw(AeronScenePresentChain* chain, AeronRenderPass* pass,
								 AeronTexture* scene_tex, AeronSampler* sampler,
								 AeronTexture* bloom_tex, float bloom_intensity, int rt_w,
								 int rt_h, float bar_y_uv, const float tint[4],
								 int src_coverage);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_PRESENT_H */
