#ifndef AERON_SCENE_BLOOM_H
#define AERON_SCENE_BLOOM_H

/* HDR bloom with an approximately resolution-independent screen-space radius.
 * The reference is four equally weighted pyramid bands at 2160p. At other
 * heights their weights move between native pyramid levels, keeping gain 4.
 * The pyramid is a 2x2 bright pass, 13-tap downsamples and 3x3 tent
 * upsamples, so compact bright sources stay round and do not snap to the
 * coarse levels' texel grid as they move.
 * The bright pass has no threshold; presentation applies the runtime
 * intensity. Small emitters remain sensitive to source coverage. */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"
struct AeronCommandBuffer;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronSceneBloom AeronSceneBloom;

/* Create the half-resolution pyramid for (rt_w, rt_h). Depth depends on
 * height; unused coarse levels are omitted. Targets use R11G11B10_UFLOAT.
 * Recreate when the source dimensions change. Returns NULL on failure. */
AeronSceneBloom* AeronSceneBloom_Create(int rt_w, int rt_h);

void AeronSceneBloom_Destroy(AeronSceneBloom* b);

/* Run the bright pass and weighted down/up chain. Leaves the
 * accumulated bloom in mip0 (queryable via AeronSceneBloom_ColorRt) so
 * the swapchain composite shader can sample it and fold the additive
 * contribution into its own pass — eliminating a dedicated full-res
 * "Bloom composite" pass on the flight RT.
 *
 * `scissor_max_y` is the lowest Y pixel of the flight RT that bloom
 * is allowed to ORIGINATE FROM (the bright pass clears mip0 below
 * this line to zero). The same Y value is used by the swapchain
 * composite to suppress the bloom contribution below the message
 * bar. Pass a value >= rt_h to disable scissoring (bloom across full
 * RT).
 *
 * `cmd` must NOT have an active render or copy pass on entry. Returns zero
 * when dimensions differ from creation or recording fails. */
int AeronSceneBloom_Apply(AeronSceneBloom* b, struct AeronCommandBuffer* cmd, AeronTexture* flight_color_rt,
						  int rt_w, int rt_h, int scissor_max_y);

/* Borrow the bloom mip0 texture — sampled by the final present pass
 * (scene_tonemap.frag) at fragment slot t1. */
AeronRenderTarget* AeronSceneBloom_ColorRt(const AeronSceneBloom* b);

/* Intensity uniform passed to the present pass: the fraction of the scene's
 * light the bloom scatters. Process-wide runtime knob (default 0.1); 0
 * disables the bloom contribution entirely — hosts may also skip
 * AeronSceneBloom_Apply when it reads 0 to save the chain's GPU cost.
 *
 * There is no brightness threshold: all light scatters in proportion, as
 * in a lens, so what visibly glows is what is far brighter than its
 * surroundings (AeronScene_SetEmissiveScale), and glass, haze or motion
 * blur reduce a glow in proportion. */
float AeronSceneBloom_Intensity(void);
void  AeronSceneBloom_SetIntensity(float v);

#ifdef __cplusplus
}
#endif

#endif
