/* The image meter's exposure (AeronScene_SetAutoExposure). Reads the
 * frame's histogram and clears it for the next frame, averages the EV100
 * of the weighted pixels between the two percentiles, moves the current EV
 * toward the target, and writes the exposure texture and the state the CPU
 * reads back. */

#define EXPOSURE_BINS 64u

/* Matches ExposureState in exposure.c. */
struct ExposureState
{
    float ev100;
    float target_ev100;
    float metered_ev100;
    float exposure;
    float low_ev100;
    float high_ev100;
    float geometry_share;
    uint  flags; /* 1 = valid, 2 = this frame was metered */
    float histogram[EXPOSURE_BINS];
};

RWTexture2D<float2>               g_exposure : register(u0, space1);
RWStructuredBuffer<uint>          g_bins     : register(u1, space1);
RWStructuredBuffer<ExposureState> g_state    : register(u2, space1);

cbuffer ExposureAdaptUniforms : register(b0, space2)
{
    float ev100;
    float compensation;
    float metering;
    float min_ev100;
    float max_ev100;
    float brighten_speed;
    float darken_speed;
    float dt;
    float low_fraction;
    float high_fraction;
    uint  snap;
    uint  freeze;
};

groupshared float s_weight[EXPOSURE_BINS];
groupshared float s_geometry;
groupshared float s_sky;

float bin_ev(float position) { return -8.0f + position * 0.5f; }

[numthreads(EXPOSURE_BINS, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    s_weight[index] = float(g_bins[index]);
    g_bins[index]   = 0u;
    if (index == 0u) {
        s_geometry                = float(g_bins[EXPOSURE_BINS]);
        s_sky                     = float(g_bins[EXPOSURE_BINS + 1u]);
        g_bins[EXPOSURE_BINS]      = 0u;
        g_bins[EXPOSURE_BINS + 1u] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (index != 0u)
        return;

    /* The state is read and written field by field. A copy of the whole
     * struct, with its array, doesn't compile to DXIL through shadercross. */
    const bool valid = (g_state[0].flags & 1u) != 0u && snap == 0u;

    float total = 0.0f;
    for (uint b = 0u; b < EXPOSURE_BINS; ++b)
        total += s_weight[b];
    const float low  = total * low_fraction;
    const float high = total * high_fraction;

    /* The mean EV of the weight between the percentiles, and the EVs at
     * the percentiles. */
    float sum = 0.0f, inside = 0.0f, below = 0.0f;
    float low_ev = g_state[0].low_ev100, high_ev = g_state[0].high_ev100;
    for (uint i = 0u; i < EXPOSURE_BINS; ++i) {
        const float w = s_weight[i];
        if (w > 0.0f) {
            const float part = max(min(below + w, high) - max(below, low), 0.0f);
            sum += part * bin_ev(float(i) + 0.5f);
            inside += part;
            if (below <= low && below + w > low)
                low_ev = bin_ev(float(i) + (low - below) / w);
            if (below < high && below + w >= high)
                high_ev = bin_ev(float(i) + (high - below) / w);
        }
        below += w;
    }
    const bool  metered    = inside > 0.0f;
    const float metered_ev = metered ? sum / inside : g_state[0].metered_ev100;

    /* With nothing to meter, the target stays where it is. */
    float target = valid ? g_state[0].target_ev100 : clamp(ev100 - compensation, min_ev100, max_ev100);
    if (metered)
        target = clamp(lerp(ev100, metered_ev, metering) - compensation, min_ev100, max_ev100);

    float ev = target;
    if (valid) {
        ev = g_state[0].ev100;
        if (freeze == 0u) {
            /* A higher target EV means a brighter scene. */
            const float speed = target > ev ? darken_speed : brighten_speed;
            ev += (target - ev) * (1.0f - exp(-speed * dt));
        }
    }

    const float exposure       = 683.0f / (1.2f * exp2(ev));
    g_state[0].ev100          = ev;
    g_state[0].target_ev100   = target;
    g_state[0].metered_ev100  = metered_ev;
    g_state[0].exposure       = exposure;
    g_state[0].low_ev100      = low_ev;
    g_state[0].high_ev100     = high_ev;
    g_state[0].geometry_share = s_geometry + s_sky > 0.0f ? s_geometry / (s_geometry + s_sky) : 0.0f;
    g_state[0].flags          = 1u | (metered ? 2u : 0u);
    for (uint h = 0u; h < EXPOSURE_BINS; ++h)
        g_state[0].histogram[h] = total > 0.0f ? s_weight[h] / total : 0.0f;
    g_exposure[uint2(0, 0)] = float2(exposure, ev);
}
