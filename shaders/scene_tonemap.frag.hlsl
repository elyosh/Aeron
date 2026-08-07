/*
 * Flight HDR → swapchain tonemap + present.
 *
 * Fullscreen-quad fragment shader for the final present-time draw.
 * Samples the HDR flight RT (which already carries scene + cockpit +
 * HUD composited in linear HDR) plus the bloom mip0, applies exposure
 * + tonemap, multiplies by a fade tint, and emits PMA-encoded output
 * so the hardware PMA-over blend cross-fades the flight overlay onto
 * whatever is already in the swapchain (classic FB, cutscene PMA blit
 * happens after this draw).
 *
 * Cockpit / HUD / PIP are drawn into the same HDR RT BEFORE bloom, so
 * bloom sees the whole diegetic frame — bright HUD elements bloom like
 * the rest of the scene. Cutscene UI stays LDR and composites on top
 * of the tonemapped flight via a separate PMA blit.
 *
 * Two operators selectable at runtime via tonemap_params.y (Cmd+T
 * cycles; debug UI has radio buttons):
 *   0.0 = ACES Fitted (Stephen Hill 2017).
 *   1.0 = AgX parametric — inline polynomial approximation of the
 *        AgX default-contrast sigmoid. Pure ALU, no texture taps.
 *
 * Pairs with composite_two_rt.vert.hlsl — same fullscreen-triangle-
 * strip convention.
 */

#include "srgb.hlsli"
#include "scene_tonemap_agx.hlsli"

/* Interleaved Gradient Noise (Bart Wronski / Jorge Jimenez). Single
 * frac() formula, no LUT, perceptually-better-than-white-noise
 * distribution. Output is in [0, 1]; caller centres on 0 and scales
 * to the desired LSB magnitude. Spatially-coherent enough that the
 * dither pattern isn't visible as static, but high-frequency enough
 * that it blurs into a smooth gradient at viewing distance. */
float ign_dither(float2 pixel)
{
    return frac(52.9829189f *
                frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

cbuffer TonemapPS : register(b0, space3)
{
    /* Bloom inputs:
     *   x = intensity multiplier
     *   y = 1.0 / flight_rt_width
     *   z = 1.0 / flight_rt_height
     *   w = bar_y_uv (UV.y at the message-bar top; bloom gated to 0
     *       below this line so the bar doesn't pick up a halo). */
    float4 bloom_params;
    /* Tonemap controls:
     *   x = scene_exposure (linear pre-tonemap scale)
     *   y = operator id  (0 = ACES Fitted, 1 = AGX)
     *   z, w = reserved. */
    float4 tonemap_params;
    /* Fade tint applied AFTER tonemap. rgb is the linear multiplier;
     * a is the PMA coverage (controls the cross-fade between the
     * flight overlay and whatever is already in the swapchain). For
     * a fully-opaque flight present, pass (1, 1, 1, 1). For
     * fade-to-black on scene switch, pass (k, k, k, k) with k
     * shrinking toward 0. */
    float4 present_tint;
    /* Misc present-pass debug knobs:
     *   x = bloom kernel mode (0 = 1-tap, 1 = 4-tap box)
     *   y = parametric AgX EOTF exponent
     *   z = ACES pre-exposure (scene multiplier before the Hill fit)
     *   w = source-coverage weight: 0 = full-frame coverage (flight
     *       present), 1 = coverage from the scene texel's alpha (PiP
     *       targets with a transparent background). */
    float4 present_misc;
    /* AgX controls:
     *   x = look (0 = base, 1 = punchy)
     *   y = punchy power
     *   z = punchy saturation
     *   w = reserved. */
    float4 agx_params;
};

Texture2D<float4> g_flight  : register(t0, space2);
Texture2D<float4> g_bloom   : register(t1, space2);
SamplerState      s_flight  : register(s0, space2);
SamplerState      s_bloom   : register(s1, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

/* ===== ACES Fitted tonemap (Stephen Hill 2017) =====
 *
 * Industry-standard HDR→SDR fit used by Unreal, Frostbite, id Tech 7
 * etc. Fits the full ACES RRT + sRGB ODT pipeline into a 3×3 input
 * matrix, a rational sigmoid (RRT+ODT_FIT), and a 3×3 output matrix.
 * Hue-preserving by construction, smooth across the full HDR range,
 * identity at zero, asymptote at 1.0. Output is LINEAR sRGB display-
 * referred — unlike AGX's polynomial which bakes in the sRGB OETF.
 * So when this branch runs we write the output directly; the _SRGB
 * swapchain store-side encode is what brings it to display bytes. No
 * srgb_eotf step needed here.
 *
 * Available as a runtime A/B alternative to AGX. Cmd+T toggles the
 * operator. Matrices are row-major in HLSL initialiser order, white
 * preserved (rows sum to 1.0 — verified). */
static const float3x3 ACES_INPUT_MATRIX = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f
);

static const float3x3 ACES_OUTPUT_MATRIX = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f
);

float3 rrt_and_odt_fit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 tonemap_aces_fitted(float3 color)
{
    color = max(color, 0.0f);
    color = mul(ACES_INPUT_MATRIX, color);
    color = rrt_and_odt_fit(color);
    color = mul(ACES_OUTPUT_MATRIX, color);
    return saturate(color);
}

float4 main(VSOut input) : SV_Target
{
    float4 scene_texel = g_flight.Sample(s_flight, input.uv);
    float3 scene       = scene_texel.rgb;

    /* Bloom kernel — selectable at runtime via present_misc.x:
     *   < 0.5 → 1-tap   (single bilinear sample at uv; cheapest)
     *   else  → 4-tap   (±1-mip0-texel diagonal box, adds soft blur)
     * The branch is uniform across the fullscreen quad — no divergence.
     * bloom_mip0 is at half scene-RT resolution (final upsample/filter
     * output of the bloom chain), so 1-tap is already softer than a
     * naive nearest sample. */
    float3 bloom;
    if (present_misc.x < 0.5f) {
        bloom = g_bloom.Sample(s_bloom, input.uv).rgb;
    } else {
        float2 t = bloom_params.yz;
        float3 b0 = g_bloom.Sample(s_bloom, input.uv + t * float2(-1.0f, -1.0f)).rgb;
        float3 b1 = g_bloom.Sample(s_bloom, input.uv + t * float2( 1.0f, -1.0f)).rgb;
        float3 b2 = g_bloom.Sample(s_bloom, input.uv + t * float2(-1.0f,  1.0f)).rgb;
        float3 b3 = g_bloom.Sample(s_bloom, input.uv + t * float2( 1.0f,  1.0f)).rgb;
        bloom = (b0 + b1 + b2 + b3) * 0.25f;
    }

    /* Message-bar gate — zero bloom contribution below bar_y_uv. */
    float bar_gate = step(input.uv.y, bloom_params.w);
    float3 bloom_contrib = bloom * bloom_params.x * bar_gate;

    /* HDR scene + bloom, then exposure, then tonemap.
     *
     * tonemap_params.y picks the operator at runtime (set by the host
     * each frame from flight_gpu_tonemap_op(); Cmd+T flips it live).
     * The branch is uniform over the fullscreen quad so the GPU runs
     * exactly one path per frame — no per-pixel divergence, no
     * measurable perf delta vs hard-coding one operator. Both branches
     * return linear display-referred values. */
    float3 rgb = (scene + bloom_contrib) * tonemap_params.x;
    /* Match the host enum: 0 = ACES Fitted, 1 = AGX parametric.
     * Branch is uniform across the quad. ACES gets a pre-exposure
     * boost (present_misc.z) to lift its dark toe up to AgX's. */
    if (tonemap_params.y < 0.5f)
        rgb = tonemap_aces_fitted(rgb * present_misc.z);
    else
        rgb = AeronAgxToneMap(rgb, AERON_AGX_MAX_EV, present_misc.y, agx_params.x,
                             agx_params.y, agx_params.z);

    /* Perturb the encoded value by half an 8-bit code, then decode so the
     * hardware sRGB store reconstructs that value. One scalar noise sample
     * avoids chroma speckle. */
    float dither = (ign_dither(input.position.xy) - 0.5f) * (1.0f / 255.0f);
    rgb = AeronSrgbToLinear(saturate(AeronLinearToSrgb(saturate(rgb)) + dither));

    /* PMA-encode: rgb pre-multiplied by alpha so the hardware PMA-
     * over blend (src + dst*(1-src.a)) cross-fades the tonemapped
     * flight overlay onto the swapchain. present_tint.a controls the
     * fade coverage; present_tint.rgb is a uniform colour multiplier
     * (typically 1,1,1). present_misc.w additionally weights coverage
     * by the scene texel's own alpha (PiP targets with transparent
     * background). */
    rgb *= present_tint.rgb;
    float coverage = present_tint.a * lerp(1.0f, scene_texel.a, present_misc.w);
    return float4(rgb * coverage, coverage);
}
