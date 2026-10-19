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
 * Where this pixel moves much less than the gathered motion (a still
 * cockpit strut beside a passing bolt), taps are also classified by depth
 * against it (McGuire 2012): a tap in front streaks over it by its own
 * motion, but one behind contributes only by this pixel's own motion, so
 * the strut does not take the streak of what passes behind it. Pixels
 * moving with their surroundings (and Low, which gathers along each
 * pixel's own motion) keep the plain weighting and skip the depth reads.
 *
 * Inputs:
 *   t0 color_rt, t1 velocity_rt (per-tap), t2 gather velocity, t3 noise,
 *   t4 scene depth (reverse-Z, render resolution; sampled by UV).
 * Velocity textures store UV displacement; lengths use output pixels.
 * FSR's motion (direct_velocity, direct_gather) is at render resolution
 * and negated; the host binds a point sampler for it, which picks the
 * texel under each output pixel without per-tap integer arithmetic.
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
    uint2 _pad2;
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
Texture2D    g_depth   : register(t4, space2);
SamplerState g_depth_s : register(s4, space2);

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

static float2 load_velocity(float2 uv)
{
    float2 v = g_velocity.SampleLevel(g_vel_s, uv, 0.0f).rg;
    return direct_velocity != 0u ? -v : v;
}

static float2 load_gather(float2 uv)
{
    float2 v = g_gather.SampleLevel(g_gath_s, uv, 0.0f).rg;
    return direct_gather != 0u ? -v : v;
}

/* Relative depth band over which a tap goes from in front of the pixel to
 * behind it, so surfaces at nearly the same depth do not flicker. */
#define MB_SOFT_DEPTH 0.05f

/* How far behind the pixel a tap lies, 0 (in front or level) to 1. Depth is
 * reverse-Z with view depth near/depth, so the depth ratio is the inverse
 * view-depth ratio; the sky (depth 0) lies behind everything. */
static float behind(float pixel_depth, float tap_depth)
{
    if (tap_depth <= 0.0f)
        return pixel_depth > 0.0f ? 1.0f : 0.0f;
    return saturate((pixel_depth / tap_depth - 1.0f) / MB_SOFT_DEPTH);
}

/* Keep the sampling and weighting identical for full groups and the tail.
 * `depth_aware` is a literal at every call, so each gather loop compiles
 * without a per-tap branch. */
static void accumulate_tap(float2 uv, float2 vg, float vglen, float t, bool depth_aware, float pixel_depth,
                          float pixel_vlen, inout float3 mov, inout float wsum)
{
    float2 suv = uv + vg * t;
    float2 vt = load_velocity(suv) * shutter_scale;
    float vtlen = min(length(vt * output_size), max_length_pixels);
    float dist = abs(t) * vglen;
    float w = cone(dist, vtlen);
    if (depth_aware) {
        float back = behind(pixel_depth, g_depth.SampleLevel(g_depth_s, suv, 0.0f).r);
        w = lerp(w, cone(dist, pixel_vlen), back);
    }
    mov += g_color.SampleLevel(g_color_s, suv, 0.0f).rgb * w;
    wsum += w;
}

/* Call with a constant group size and depth_aware so each path unrolls at
 * compile time. Complete groups expose independent fetches; the tail keeps
 * every adaptive tap. */
static void gather_samples(float2 uv, float2 vg, float vglen, float jitter, int N,
                           int group_size, bool depth_aware, float pixel_depth, float pixel_vlen,
                           inout float3 mov, inout float wsum)
{
    int full_taps = N - N % group_size;
    [loop] for (int base = 0; base < full_taps; base += group_size) {
        [unroll] for (int tap = 0; tap < group_size; ++tap) {
            int k = base + tap;
            float  t   = ((float)k + jitter) / (float)N - 0.5f;
            accumulate_tap(uv, vg, vglen, t, depth_aware, pixel_depth, pixel_vlen, mov, wsum);
        }
    }
    [loop] for (int k = full_taps; k < N; ++k) {
        float  t   = ((float)k + jitter) / (float)N - 0.5f;
        accumulate_tap(uv, vg, vglen, t, depth_aware, pixel_depth, pixel_vlen, mov, wsum);
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
    float pixel_vlen  = min(length(load_velocity(i.uv) * shutter_scale * output_size), max_length_pixels);
    bool  depth_aware = pixel_vlen < 0.5f * vglen;
    float pixel_depth = depth_aware ? g_depth.SampleLevel(g_depth_s, i.uv, 0.0f).r : 0.0f;

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
        gather_samples(i.uv, vg, vglen, jitter, N, 4, false, 0.0f, 0.0f, mov, wsum);
    else if (depth_aware)
        gather_samples(i.uv, vg, vglen, jitter, N, 2, true, pixel_depth, pixel_vlen, mov, wsum);
    else
        gather_samples(i.uv, vg, vglen, jitter, N, 2, false, 0.0f, 0.0f, mov, wsum);
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
