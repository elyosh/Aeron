#ifndef AERON_SCENE_BLOOM_H
#define AERON_SCENE_BLOOM_H

/*
 * HD bloom post-process for the flight RT.
 *
 * Dual-filter Kawase chain: bright-pass → N down/up levels → additive
 * composite back into the source RT. Targets PS4-class GCN — fewer
 * mip levels (3) and 4-tap kernels keep per-pass bandwidth low.
 *
 * Scope is "flight RT only" — the entrypoint is invoked from the SDL3
 * shell exclusively when the active scene is TIE_SCENE_FLIGHT, so
 * the frontend (cutscene RT) and landru scenes never see bloom. The
 * cockpit message bar at the bottom of the cockpit area is excluded
 * via a Y-scissor on the bright-pass extract and the final composite
 * (chain-internal passes do not need scissoring — the bright pass
 * already zeros the masked rows, and downsampling propagates that
 * forward).
 *
 * Threshold, knee, and intensity are fixed in the implementation.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"
struct AeronCommandBuffer;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronSceneBloom AeronSceneBloom;

/* Create the bloom resources sized to (rt_w, rt_h) — chain mips at
 * 1/2, 1/4, 1/8 of those dimensions. The chain uses an HDR-capable
 * format internally (R11G11B10_UFLOAT) so additive accumulation in
 * the upsample chain doesn't clip per channel; the host doesn't pass
 * a format — the chain's format is independent of the flight RT.
 *
 * Returns NULL on shader compile / pipeline / texture create failure. */
AeronSceneBloom *AeronSceneBloom_Create(int rt_w, int rt_h);

void         AeronSceneBloom_Destroy(AeronSceneBloom *b);

/* Run the bloom chain — bright pass + N down + N up. Leaves the
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
 * when the chain could not be recorded completely. */
int          AeronSceneBloom_Apply(AeronSceneBloom *b,
                                struct AeronCommandBuffer *cmd,
                                AeronTexture *flight_color_rt,
                                int rt_w, int rt_h,
                                int scissor_max_y);

/* Borrow the bloom mip0 texture — sampled by the final present pass
 * (flight_tonemap.frag) at fragment slot t1. */
AeronRenderTarget *AeronSceneBloom_ColorRt(const AeronSceneBloom *b);

/* Intensity uniform passed to the present pass. Process-wide runtime
 * knob (default 0.5); 0 disables the bloom contribution entirely —
 * hosts may also skip AeronSceneBloom_Apply when it reads 0 to save
 * the chain's GPU cost. */
float         AeronSceneBloom_Intensity(void);
void          AeronSceneBloom_SetIntensity(float v);

#ifdef __cplusplus
}
#endif

#endif
