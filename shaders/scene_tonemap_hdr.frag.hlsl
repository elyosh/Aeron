/*
 * flight_tonemap_hdr.frag — HDR (scRGB-linear) sibling of
 * flight_tonemap.frag.
 *
 * Output target: R16G16B16A16_FLOAT swapchain texture configured with
 * SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR (scRGB on Windows /
 * EDR on macOS). Values are linear Rec.709 with 1.0 == SDR diffuse
 * white. Peak luminance is encoded as a scale > 1.0; the OS / display
 * compositor handles the final encode to the physical HDR signal
 * (HDR10 PQ on Windows, EDR on macOS).
 *
 * Differences vs the SDR variant:
 *   1. AgX's parametric sigmoid exposes its white-point as a uniform
 *      driven by hdr_peak_scale (= peak_nits / sdr_diffuse), so the
 *      [0,1] SDR clamp + pow(1/2.4) encode never apply — HDR signal
 *      above 1.0 is preserved.
 *   2. ACES branch is peak-parameterised — `tonemap_params.z` carries
 *      hdr_peak_scale. The fit anchors scene 1.0 at display 1.0
 *      (scRGB SDR-diffuse) and linearly extends Hill's tail onto the
 *      [1, peak] display headroom; full derivation in tonemap_aces_hdr().
 *   3. No sRGB encode at the end. The swapchain is scRGB-linear F16;
 *      shader-written linear values go straight to the display
 *      pipeline.
 *   4. No saturate() clamp. Values > 1.0 are HDR signal and must
 *      reach the swapchain unclipped.
 *
 * Keep field layout in lockstep with flight_tonemap.frag.hlsl so the
 * host's cbuffer push works against both shaders without per-variant
 * branches.
 */

cbuffer TonemapPS : register(b0, space3)
{
    /* Bloom inputs (identical to SDR variant):
     *   x = intensity multiplier
     *   y = 1.0 / flight_rt_width
     *   z = 1.0 / flight_rt_height
     *   w = bar_y_uv (UV.y at the message-bar top; bloom gated to 0
     *       below this line so the bar doesn't pick up a halo). */
    float4 bloom_params;
    /* Tonemap controls:
     *   x = scene_exposure   (linear pre-tonemap scale)
     *   y = operator id      (0 = AGX, 1 = ACES)
     *   z = hdr_peak_scale   (peak_nits / sdr_diffuse_nits, ≥ 1.0)
     *   w = sdr_to_scrgb     (1.0 on macOS EDR; sdr_diffuse/80 on
     *                         Windows scRGB) */
    float4 tonemap_params;
    /* Fade tint applied AFTER tonemap. The fade is a PMA coverage
     * scalar; in HDR mode the tint.rgb stays at 1.0 (no colour grade)
     * and only the .a coverage matters for the cross-fade with the
     * classic FB underneath. */
    float4 present_tint;
    /* Misc present-pass debug knobs (kept in sync with the SDR
     * variant's layout so the host can push one cbuffer for both):
     *   x = bloom kernel mode (0 = 1-tap, 1 = 4-tap box)
     *   y = parametric AgX EOTF exponent
     *   z = ACES pre-exposure (scene multiplier before the Hill fit)
     *   w = source-coverage weight: 0 = full-frame coverage (flight
     *       present), 1 = coverage from the scene texel's alpha (PiP
     *       targets with a transparent background). */
    float4 present_misc;
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

/* ===== Parametric AgX (HDR-target) ==================================
 *
 * Canonical Troy Sobotka / EaryChow AgX, inlined. Steps:
 *   1. Rec.709 → AgX inset basis (compresses pure-primary colours
 *      toward white so the sigmoid behaves on saturated input).
 *   2. log2 of (max(c, 1e-10)) — operate in stops.
 *   3. Normalise [min_ev .. max_ev] → [0, 1]. The max_ev is the input
 *      stops level that should map to peak output:
 *        SDR: log2(16.5)         ≈ 4.04
 *        HDR: log2(16.5 * peak)  ≈ 4.04 + log2(peak)
 *      where peak = hdr_peak_scale (peak_nits / sdr_diffuse_nits).
 *   4. 6th-order polynomial sigmoid (matches the canonical AgX
 *      Filmic Hejl-Hill-style fit).
 *   5. pow(c, 2.2) — AgX's view-transform EOTF^-1 in display space.
 *   6. AgX outset matrix (inverse of inset; restores saturation
 *      that step 1 compressed away).
 *   7. Multiply by hdr_peak_scale so the sigmoid's [0,1] output is
 *      rescaled to the actual peak the display can show. Without
 *      this rescale every HDR image clips at SDR diffuse — the
 *      headroom would be unused.
 */

static const float3x3 AGX_INSET = {
    0.8424790620f, 0.0784114570f, 0.0791031990f,
    0.0423283037f, 0.8787730269f, 0.0789046050f,
    0.0423826624f, 0.0784617400f, 0.8791556090f
};

/* Pre-computed inverse of AGX_INSET — `(I - 0.117 J)^-1` style. */
static const float3x3 AGX_OUTSET = {
     1.1968790000f, -0.0980209000f, -0.0990297000f,
    -0.0528968000f,  1.1519460000f, -0.0989638000f,
    -0.0529716000f, -0.0980434000f,  1.1510910000f
};

float3 agx_sigmoid(float3 x)
{
    /* 6th-order poly fit of AgX sigmoid in [0,1]. From EaryChow's
     * canonical implementation. */
    float3 x2 = x  * x;
    float3 x4 = x2 * x2;
    return + 15.5f     * x4 * x2
           - 40.14f    * x4 * x
           + 31.96f    * x4
           -  6.868f   * x2 * x
           +  0.4298f  * x2
           +  0.1191f  * x
           -  0.00232f;
}

float3 tonemap_agx_hdr(float3 col, float peak_scale)
{
    /* Canonical AgX log domain — Sobotka's config.ocio "AgX Log
     * (Kraken)" allocates [-12.47393, 4.026069], i.e. 10 stops below
     * mid-grey (log2(0.18) - 10 = -12.47393) to 6.5 stops above
     * (log2(0.18) + 6.5 = 4.026). Anchored on mid-grey, NOT on unit.
     * Previously this constant was -10.0f (10 stops below unit),
     * which pulled mid-grey down to 0.536 in normalised sigmoid input
     * instead of the canonical 0.606 — making the polynomial sigmoid
     * sample the toe of the curve at mid-grey and producing visibly
     * over-contrasted output. */
    const float min_ev = -12.47393f;
    const float max_ev =   4.026069f + log2(max(peak_scale, 1.0f));

    /* Inset. */
    float3 c = mul(AGX_INSET, max(col, 0.0f));

    /* log2 + normalise into [0,1]. */
    c = log2(max(c, 1e-10f));
    c = saturate((c - min_ev) / (max_ev - min_ev));

    /* Sigmoid + EOTF^-1. Exponent is host-controllable via
     * present_misc.y (default 2.2 — Mikamiko/Three.js/Bevy inline-AgX
     * convention). The HDR & Display inspector exposes a slider. */
    c = agx_sigmoid(c);
    c = pow(c, present_misc.y);

    /* Outset. AGX_OUTSET has negative off-diagonal coefficients
     * (it's the inverse of AGX_INSET — the inset compresses
     * saturated primaries toward white, so the outset must
     * expand them back via a matrix that can take values
     * out-of-gamut on pure-primary inputs). For strongly
     * saturated channels (e.g. red HUD bolts in the cockpit)
     * the matrix produces negative values in the other channels.
     * Negative scRGB values are undefined: the swapchain accepts
     * them, but the OS HDR compositor + debug-UI alpha-blends
     * over them turn into black splotches downstream. Clamp to
     * non-negative — matches canonical AgX reference
     * implementations and is harmless for in-gamut colours. */
    c = max(mul(AGX_OUTSET, c), 0.0f);

    /* Rescale the [0,1] sigmoid output to the actual display peak. */
    return c * peak_scale;
}

/* ===== ACES (HDR-target) ============================================
 *
 * Stephen Hill 2017 RRT+ODT fit (the 100-nit sRGB ODT only), with
 * two adaptations so it works for the scRGB-linear HDR swapchain:
 *
 *   1. White-point normalisation. The canonical Hill fit maps scene
 *      1.0 → display ~0.619 — correct for an ACES diffuse-white
 *      contract but wrong for the scRGB convention "1.0 == SDR
 *      diffuse white". We divide RRT(v) by RRT_AT_ONE so scene 1.0
 *      anchors at display 1.0; mid-grey (0.18) anchors near 0.17.
 *
 *   2. Headroom extension. After normalisation Hill's curve still
 *      asymptotes at NORM_ASYMPTOTE ≈ 1.64 — the entire HDR
 *      headroom above 1.64× SDR diffuse would be unused. We map
 *      the normalised range [1, NORM_ASYMPTOTE] linearly onto the
 *      display range [1, peak_scale]. The < 1.0 region is left
 *      untouched, so the perceptually-important SDR range matches
 *      a white-point-anchored SDR ACES path exactly.
 *
 * The knee at v=1.0 has a slope jump (1 → K) that would show as a
 * brightness kink on smooth gradients crossing diffuse white. A
 * C1-smooth quadratic window of width KNEE rounds it off. The
 * smoothing costs a small asymptote undershoot (scene → ∞ lands at
 * ~93% of peak_scale instead of exactly peak) — perceptually
 * unnoticeable.
 *
 * Degenerates correctly:
 *   - peak_scale == 1.0  (SDR fallback through HDR pipeline): K=0,
 *                        extension term subtracts; clamp pins at 1.
 *   - peak_scale >  1.0  (HDR display): K>0, output extends to peak.
 *
 * Does NOT implement a peak-luminance-aware ACES ODT (the segmented
 * 1000/2000/4000-nit Academy transforms). Hill's fit is the 100-nit
 * sRGB ODT specifically; the linear extension is a pragmatic
 * approximation that preserves Hill's character below diffuse and
 * uses headroom proportionally above.
 */

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

/* Pre-computed Hill-fit constants.
 *   RRT_AT_ONE     = rrt_and_odt_fit(1.0)         ≈ 0.6191
 *   NORM_ASYMPTOTE = (1/0.983729) / RRT_AT_ONE    ≈ 1.6421
 *     (Hill's polynomial asymptote 1/0.983729, post-normalisation.) */
static const float RRT_AT_ONE     = 0.619115f;
static const float NORM_ASYMPTOTE = 1.642115f;
/* Knee half-width in normalised-v space. Smooth quadratic transition
 * spans v∈[1.0, 1.0 + KNEE]; outside this window the curve is exactly
 * its asymptotic form. 0.15 keeps the smooth region inside ~25% above
 * SDR diffuse — narrow enough to preserve SDR character, wide enough
 * to hide the slope kink. */
static const float ACES_HDR_KNEE  = 0.15f;

float3 tonemap_aces_hdr(float3 color, float peak_scale)
{
    color = max(color, 0.0f);
    color = mul(ACES_INPUT_MATRIX, color);

    /* White-point-normalised Hill RRT+ODT: scene 1.0 → v == 1.0. */
    float3 v    = rrt_and_odt_fit(color) * (1.0f / RRT_AT_ONE);
    float  peak = max(peak_scale, 1.0f);

    /* Slope multiplier for the linear extension above v=1.0. Maps the
     * Hill tail [1, NORM_ASYMPTOTE] onto display [1, peak]. K=0 when
     * peak == 1 (extension collapses); K=1 when peak == NORM_ASYMPTOTE
     * (extension is identity). For typical HDR peaks (3..8) K is 3..11. */
    float K = (peak - 1.0f) / (NORM_ASYMPTOTE - 1.0f);

    /* C1 soft-relu of (v - 1):
     *   v ≤ 1            → 0
     *   v ∈ [1, 1+KNEE]  → 0.5·KNEE·t² with t = (v-1)/KNEE      (quadratic)
     *   v ≥ 1+KNEE       → (v-1) - 0.5·KNEE                     (linear, slope 1)
     * The piecewise pieces meet with matching value AND slope at both
     * endpoints, so v + (K-1)·above() is C1-continuous everywhere. */
    float3 above_raw = v - 1.0f;
    float3 t         = clamp(above_raw * (1.0f / ACES_HDR_KNEE), 0.0f, 1.0f);
    float3 above     = (0.5f * ACES_HDR_KNEE) * t * t
                     + max(above_raw - ACES_HDR_KNEE, 0.0f);

    /* y = v below the knee, y = 1 + K·(v-1) above. The (K-1) factor
     * collapses to 0 when K=1 (no extension needed) and goes negative
     * when K<1 (peak smaller than Hill's natural asymptote — the
     * extension actively compresses the tail; the final clamp pins
     * the output at peak). */
    v = v + (K - 1.0f) * above;
    v = clamp(v, 0.0f, peak);

    /* ACES_OUTPUT_MATRIX has negative off-diagonals (wide-gamut →
     * Rec.709). Strongly saturated input produces negative channels;
     * clamp to ≥ 0 so we never push negative scRGB into the swapchain
     * (the OS HDR compositor's debug-overlay alpha-blend turns those
     * into black splotches). We do NOT clamp positive values — the
     * matrix can amplify saturated channels above peak by ~1.6×; that
     * overshoot is legal HDR signal and the OS clips at hardware peak. */
    return max(mul(ACES_OUTPUT_MATRIX, v), 0.0f);
}

float4 main(VSOut input) : SV_Target
{
    float4 scene_texel = g_flight.Sample(s_flight, input.uv);
    float3 scene       = scene_texel.rgb;

    /* Bloom kernel — selectable at runtime via present_misc.x (same
     * branch as the SDR variant; see comment there for the rationale).
     * On HDR-output builds present is bandwidth-bound on RGBA16F reads,
     * so the 1-tap mode is the meaningful saving here — 4 taps × half-
     * res RGBA16F dominates the present pass cost. */
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

    float bar_gate = step(input.uv.y, bloom_params.w);
    float3 bloom_contrib = bloom * bloom_params.x * bar_gate;

    /* Compose scene + bloom + exposure, then peak-parameterised
     * tonemap. tonemap_params.z carries hdr_peak_scale; .y picks
     * operator. */
    float3 rgb = (scene + bloom_contrib) * tonemap_params.x;
    float  peak = tonemap_params.z;
    /* Match the host enum: 0 = ACES, 1 = AGX parametric. Both run as
     * peak-parameterised HDR implementations here. ACES gets a
     * pre-exposure boost (present_misc.z) to lift its dark toe up to
     * AgX's; applied before the white-point normalisation inside the
     * fit, so scene 1.0×exposure rides above diffuse white.
     *   < 0.5 → ACES (op 0)
     *   else  → AGX  (op 1) */
    if (tonemap_params.y < 0.5f)
        rgb = tonemap_aces_hdr(rgb * present_misc.z, peak);
    else
        rgb = tonemap_agx_hdr (rgb, peak);

    /* Apply the SDR-to-scRGB scale so the tonemapped output lines up
     * with the user's chosen SDR diffuse white on the scRGB swapchain.
     * On macOS EDR this is 1.0 (Apple's scRGB-1.0 == current SDR
     * brightness convention); on Windows scRGB it's
     * sdr_diffuse_nits / 80. */
    rgb *= tonemap_params.w;

    /* PMA-encode for the cross-fade with the classic FB underneath.
     * No sRGB encode here — F16 scRGB-linear swapchain takes linear
     * values directly. present_misc.w additionally weights coverage
     * by the scene texel's own alpha (PiP targets with transparent
     * background). */
    rgb *= present_tint.rgb;
    float coverage = present_tint.a * lerp(1.0f, scene_texel.a, present_misc.w);
    return float4(rgb * coverage, coverage);
}
