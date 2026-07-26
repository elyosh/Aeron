#ifndef AERON_SCENE_BLEND_RAMP_H
#define AERON_SCENE_BLEND_RAMP_H

/*
 * Crossfade state for the classic <-> remaster view toggle.
 *
 * `target` is the user's chosen end state (0.0 = classic, 1.0 =
 * remaster), flipped by Aeron_BlendRampToggle on each Tab press while the
 * cutscene compositor has a matching bundle. The default after
 * Aeron_BlendRampInit is 1.0 — start on the remaster overlay and let Tab fall
 * back to classic. `alpha` is the current displayed mix, eased toward
 * an effective target each frame via Aeron_BlendRampAdvance.
 *
 * The effective target is supplied by the caller — it's normally
 * `target`, but the FMV-stream defer rule overrides it to 0.0 for
 * frames where the engine is streaming video into the classic FB
 * (the cutscene compositor has no sprite-side replacement for those).
 * Storing only the user intent in AeronBlendRamp lets alpha snap back to
 * `target` automatically when the FMV defer ends.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronBlendRamp {
    float alpha;          /* current displayed mix, 0..1 */
    float target;         /* user intent (0 or 1), set by toggle */
    float tau_seconds;    /* exponential ease time constant */
} AeronBlendRamp;

void  Aeron_BlendRampInit    (AeronBlendRamp *s);
void  Aeron_BlendRampToggle  (AeronBlendRamp *s);
void  Aeron_BlendRampAdvance (AeronBlendRamp *s, int32_t delta_us, float effective_target);

/* True when alpha is within the snap epsilon of 0 or 1 — no fade
 * animation in progress. The main loop skips the offscreen RT path
 * entirely in this case and renders straight to the backbuffer. */
bool  Aeron_BlendRampIsSolid(const AeronBlendRamp *s);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_BLEND_RAMP_H */
