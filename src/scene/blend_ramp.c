/*
 * Crossfade state — exponential ease toward an effective target.
 *
 * tau_seconds is the time-constant of an exponential decay; the fade
 * settles to within ~5% of the target in 3 tau and within ~1% in 5
 * tau. With tau = 0.16 s the visible transition is ~500 ms — slow
 * enough to read as a deliberate compare-the-versions crossfade
 * rather than a snappy mode switch. The shell can override this by
 * writing g_blend.tau_seconds after Aeron_BlendRampInit if a different cadence
 * is wanted (no CLI flag yet — add when there's a second consumer).
 *
 * Snap epsilon (0.005) keeps Aeron_BlendRampIsSolid stable so the main loop
 * can use it as the gate for "skip the offscreen RT path entirely".
 * Without it floating-point residue would keep the fade machinery
 * engaged forever.
 */

#include "aeron/scene/blend_ramp.h"

#include <math.h>

#define BLEND_SNAP_EPSILON 0.005f

void Aeron_BlendRampInit(AeronBlendRamp *s)
{
    /* Default to the remaster overlay: alpha=1 means "show cutscene RT,
     * hide classic FB" the moment a cutscene bundle activates. The
     * effective-target gate in the main loop still forces alpha→0 when
     * no bundle is bound or an FMV stream is deferring, so this only
     * takes effect for scenes that actually have a remaster. */
    s->alpha       = 1.0f;
    s->target      = 1.0f;
    s->tau_seconds = 0.32f;
}

void Aeron_BlendRampToggle(AeronBlendRamp *s)
{
    s->target = (s->target >= 0.5f) ? 0.0f : 1.0f;
}

void Aeron_BlendRampAdvance(AeronBlendRamp *s, int32_t delta_us, float effective_target)
{
    if (effective_target < 0.0f) effective_target = 0.0f;
    if (effective_target > 1.0f) effective_target = 1.0f;

    float dt = (float)delta_us * 1.0e-6f;
    if (dt <= 0.0f)
        return;

    /* Standard one-pole exponential ease. The blend factor is
     * 1 - exp(-dt/tau): proportion of the remaining distance to
     * close this tick. Tick-rate independent — short or long dt
     * both produce the same total settling time. */
    float k = 1.0f - expf(-dt / s->tau_seconds);
    s->alpha += (effective_target - s->alpha) * k;

    /* Snap-to-endpoint when within epsilon. Keeps Aeron_BlendRampIsSolid
     * latching cleanly and avoids per-frame work after the fade
     * has visually completed. */
    if (effective_target <= 0.0f && s->alpha < BLEND_SNAP_EPSILON)
        s->alpha = 0.0f;
    else if (effective_target >= 1.0f && s->alpha > 1.0f - BLEND_SNAP_EPSILON)
        s->alpha = 1.0f;
}

bool Aeron_BlendRampIsSolid(const AeronBlendRamp *s)
{
    return s->alpha <= BLEND_SNAP_EPSILON ||
           s->alpha >= 1.0f - BLEND_SNAP_EPSILON;
}
