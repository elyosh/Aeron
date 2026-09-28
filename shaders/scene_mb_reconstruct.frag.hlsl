/*
 * Motion-blur reconstruct — velocity-weighted gather (McGuire-style),
 * shared by both tiers.
 *
 * For each pixel we adaptively gather samples of color_rt along a
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
 * Velocity textures store UV displacement; lengths use output pixels.
 * All inputs have one mip level; explicit LOD retains the sampler filtering
 * without requesting implicit derivatives inside the adaptive gather loops.
 */

cbuffer MbReconstructUniforms : register(b0, space3)
{
    float shutter_scale;
    float tap_count;
    float max_length_pixels;
    uint low_quality;
    float2 output_size;
    float2 _pad1;
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
 * (both in pixels). 1 at the centre, ramping to 0 at the motion's reach. */
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
    return g_velocity.SampleLevel(g_vel_s, uv, 0.0f).rg;
}

static float2 load_gather(float2 uv)
{
    if (direct_gather != 0u)
        return -g_gather.Load(int3(velocity_texel(uv), 0)).rg;
    return g_gather.SampleLevel(g_gath_s, uv, 0.0f).rg;
}

/* Keep the sampling and weighting identical for full groups and the tail. */
static void accumulate_tap(float2 uv, float2 vg, float vglen, float t,
                          inout float3 mov, inout float wsum)
{
    float2 suv = uv + vg * t;
    float2 vt = load_velocity(suv) * shutter_scale;
    float vtlen = min(length(vt * output_size), max_length_pixels);
    float w = cone(abs(t) * vglen, vtlen);
    mov += g_color.SampleLevel(g_color_s, suv, 0.0f).rgb * w;
    wsum += w;
}

/* Call with a constant group size so both quality paths unroll at compile time.
 * Complete groups expose independent fetches; the tail keeps every adaptive tap. */
static void gather_samples(float2 uv, float2 vg, float vglen, float jitter, int N,
                           int group_size, inout float3 mov, inout float wsum)
{
    int full_taps = N - N % group_size;
    [loop] for (int base = 0; base < full_taps; base += group_size) {
        [unroll] for (int tap = 0; tap < group_size; ++tap) {
            int k = base + tap;
            float  t   = ((float)k + jitter) / (float)N - 0.5f;
            accumulate_tap(uv, vg, vglen, t, mov, wsum);
        }
    }
    [loop] for (int k = full_taps; k < N; ++k) {
        float  t   = ((float)k + jitter) / (float)N - 0.5f;
        accumulate_tap(uv, vg, vglen, t, mov, wsum);
    }
}

float4 main(VSOut i) : SV_Target0
{
    float3 center = g_color.SampleLevel(g_color_s, i.uv, 0.0f).rgb;

    float2 vg    = load_gather(i.uv) * shutter_scale;
    float  vglen = length(vg * output_size);
    if (vglen > max_length_pixels) {
        vg *= max_length_pixels / vglen;
        vglen = max_length_pixels;
    }
    if (vglen < 1e-3f)
        return float4(center, 1.0f);            /* no motion here */

    float jitter = g_noise.SampleLevel(g_noise_s, frac(i.position.xy * 0.25f), 0.0f).r
                 * 0.5f + 0.5f;

    /* The host supplies resolution-scaled limits and High's budget floor.
     * Adapt to streak length and round up, retaining at least four samples. */
    int N = max(4, (int)ceil(tap_count * (vglen / max_length_pixels)));

    /* Gather the MOVING contribution: each tap is weighted by whether its
     * OWN motion reaches this pixel (background taps ≈ 0). `wsum` is the
     * accumulated coverage — how much moving geometry swept over this
     * pixel during the shutter. A trail pixel near a silhouette hits many
     * source taps (high coverage); further out it hits fewer (low coverage). */
    float3 mov  = 0.0f.xxx;   /* moving-geometry colour (weighted) */
    float  wsum = 0.0f;       /* weight that fed `mov` (for its average) */
    /* Use pairs for High's longer gathers and four-tap groups for Low's
     * smaller sample budgets. This changes scheduling, not the tap count. */
    if (low_quality != 0u)
        gather_samples(i.uv, vg, vglen, jitter, N, 4, mov, wsum);
    else
        gather_samples(i.uv, vg, vglen, jitter, N, 2, mov, wsum);
    if (wsum < 1e-4f)
        return float4(center, 1.0f);             /* nothing moving reached here */

    /* Composite the moving colour OVER this pixel's own (background)
     * colour by coverage, so the streak fades into the background instead
     * of ending as a hard-edged block. A pixel only sees moving samples
     * across ≈ half the gather (one side), so normalise coverage by N/2 →
     * a fully-swept interior reaches 1 (opaque), a trail edge tapers. */
    float3 moving = mov / wsum;
    float  alpha  = saturate(wsum / (0.5f * (float)N));
    return float4(lerp(center, moving, alpha), 1.0f);
}
