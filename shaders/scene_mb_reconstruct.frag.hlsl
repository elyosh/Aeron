/*
 * Motion-blur reconstruct — velocity-weighted gather (McGuire-style),
 * shared by both tiers.
 *
 * For each pixel we gather `tap_count` samples of color_rt along a
 * "gather" velocity and weight each tap by a cone of the TAP's own
 * velocity magnitude: a tap only contributes if its object moves enough
 * to have swept over this pixel. The crucial consequence is that
 * off-streak background taps (≈ zero velocity) get ~zero weight, so a
 * bright coherent feature (a laser bolt) keeps its brightness instead of
 * being box-averaged into the dark background — it still crosses the
 * bloom threshold.
 *
 * Tier difference is only the gather velocity source (t2):
 *   Low  — velocity_rt itself (the pixel's own velocity). Keeps moving
 *          features bright; cannot streak onto static background.
 *   High — the NeighborMax velocity (dominant motion in the tile
 *          neighbourhood), so a static pixel next to a fast object
 *          gathers along that object's motion and the object streaks
 *          onto the background.
 *
 * Inputs:
 *   t0 color_rt, t1 velocity_rt (per-tap), t2 gather velocity, t3 noise.
 * Uniform: shutter_scale, tap_count, max_radius (UV clamp = tile size).
 */

cbuffer MbReconstructUniforms : register(b0, space3)
{
    float shutter_scale;
    float tap_count;
    float max_radius;
    float _pad0;
    uint2 velocity_size;
    uint direct_velocity;
    uint direct_gather;
};

Texture2D    g_color   : register(t0, space2);
SamplerState g_color_s : register(s0, space2);
Texture2D    g_velocity: register(t1, space2);
SamplerState g_vel_s   : register(s1, space2);
Texture2D    g_gather  : register(t2, space2);
SamplerState g_gath_s  : register(s2, space2);
Texture2D    g_noise   : register(t3, space2);
SamplerState g_noise_s : register(s3, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

/* How much a sample at `dist` is covered by motion of length `vlen`
 * (both in UV). 1 at the centre, ramping to 0 at the motion's reach. */
static float cone(float dist, float vlen)
{
    return saturate(1.0f - dist / max(vlen, 1e-5f));
}

static int2 velocity_texel(float2 uv)
{
    return int2(min(uint2(saturate(uv) * float2(velocity_size)), velocity_size - 1u));
}

static float2 load_velocity(float2 uv)
{
    if (direct_velocity != 0u)
        return -g_velocity.Load(int3(velocity_texel(uv), 0)).rg;
    return g_velocity.Sample(g_vel_s, uv).rg;
}

static float2 load_gather(float2 uv)
{
    if (direct_gather != 0u)
        return -g_gather.Load(int3(velocity_texel(uv), 0)).rg;
    return g_gather.Sample(g_gath_s, uv).rg;
}

float4 main(VSOut i) : SV_Target0
{
    float3 center = g_color.Sample(g_color_s, i.uv).rgb;

    float2 vg    = load_gather(i.uv) * shutter_scale;
    float  vglen = length(vg);
    if (vglen > max_radius) { vg *= max_radius / vglen; vglen = max_radius; }
    if (vglen < 1e-5f)
        return float4(center, 1.0f);            /* no motion here */

    float jitter = g_noise.Sample(g_noise_s, frac(i.position.xy * 0.25f)).r
                 * 0.5f + 0.5f;

    /* Adaptive tap count: scale with the blur length so the sample
     * SPACING stays constant (= max_radius / tap_count) regardless of how
     * far this pixel blurs. Short/moderate motion then costs only a few
     * taps — most of the win when the whole frame blurs under camera
     * motion — while a full-length smear still gets the full count, so
     * there's no added banding. */
    int N = (int)(tap_count * (vglen / max_radius) + 0.5f);
    N = clamp(N, 4, (int)tap_count);

    /* Gather the MOVING contribution: each tap is weighted by whether its
     * OWN motion reaches this pixel (background taps ≈ 0). `cov` is the
     * accumulated coverage — how much moving geometry swept over this
     * pixel during the shutter. A trail pixel near a silhouette hits many
     * source taps (high cov); further out it hits fewer (low cov). */
    float3 mov  = 0.0f.xxx;   /* moving-geometry colour (weighted) */
    float  wsum = 0.0f;       /* weight that fed `mov` (for its average) */
    float  cov  = 0.0f;       /* coverage accumulator */
    [loop] for (int k = 0; k < N; k++) {
        float  t   = ((float)k + jitter) / (float)N - 0.5f;
        float2 suv = i.uv + vg * t;
        float2 vt  = load_velocity(suv) * shutter_scale;
        float  vtlen = min(length(vt), max_radius);
        float  dist  = abs(t) * vglen;
        float  w     = cone(dist, vtlen);        /* tap's motion reaches here */
        mov  += g_color.Sample(g_color_s, suv).rgb * w;
        wsum += w;
        cov  += w;
    }
    if (wsum < 1e-4f)
        return float4(center, 1.0f);             /* nothing moving reached here */

    /* Composite the moving colour OVER this pixel's own (background)
     * colour by coverage, so the streak fades into the background instead
     * of ending as a hard-edged block. A pixel only sees moving samples
     * across ≈ half the gather (one side), so normalise coverage by N/2 →
     * a fully-swept interior reaches 1 (opaque), a trail edge tapers. */
    float3 moving = mov / wsum;
    float  alpha  = saturate(cov / (0.5f * (float)N));
    return float4(lerp(center, moving, alpha), 1.0f);
}
