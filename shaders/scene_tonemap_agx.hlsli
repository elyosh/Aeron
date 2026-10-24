#ifndef AERON_SCENE_TONEMAP_AGX_HLSLI
#define AERON_SCENE_TONEMAP_AGX_HLSLI

/* Minimal AgX default-contrast transform for linear Rec.709 input. The
 * matrices are transposed from the column-major GLSL reference so HLSL's
 * mul(matrix, vector) preserves neutral values. */
static const float3x3 AERON_AGX_INSET_MATRIX = float3x3(
    0.842479062253094f,  0.0784335999999992f, 0.0792237451477643f,
    0.0423282422610123f, 0.878468636469772f,  0.0784336f,
    0.0423756549057051f, 0.0784336f,           0.879142973793104f
);

static const float3x3 AERON_AGX_OUTSET_MATRIX = float3x3(
     1.19687900512017f,   -0.0980208811401368f, -0.0990297440797205f,
    -0.0528968517574562f,  1.15190312990417f,   -0.0989611768448433f,
    -0.0529716355144438f, -0.0980434501171241f,  1.15107367264116f
);

static const float AERON_AGX_MIN_EV = -12.47393f;
static const float AERON_AGX_MAX_EV =   4.026069f;

float3 AeronAgxDefaultContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5f    * x4 * x2
           - 40.14f   * x4 * x
           + 31.96f   * x4
           -  6.868f  * x2 * x
           +  0.4298f * x2
           +  0.1191f * x
           -  0.00232f;
}

/* Historical AgX Punchy defaults are an ASC-CDL-style 1.35 power followed
 * by 1.4 saturation. The parameters are runtime-adjustable for visual tuning. */
float3 AeronAgxApplyLook(float3 color, float look, float punchy_power,
                        float punchy_saturation)
{
    if (look < 0.5f)
        return color;

    color = pow(max(color, 0.0f), punchy_power);
    const float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    return luma.xxx + punchy_saturation * (color - luma.xxx);
}

/* AgX of the grey (v, v, v): the matrices keep greys grey, and the look's
 * saturation has no effect on them, so only the curve, the look's power and
 * the EOTF remain. The curve is AeronAgxDefaultContrastApprox on one
 * value. */
float AeronAgxToneMapGrey(float v, float max_ev, float eotf_exponent, float look,
                          float punchy_power)
{
    float x = saturate((log2(max(v, 1e-10f)) - AERON_AGX_MIN_EV) /
                       (max_ev - AERON_AGX_MIN_EV));
    const float x2 = x * x;
    const float x4 = x2 * x2;
    x = saturate(15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x +
                 0.4298f * x2 + 0.1191f * x - 0.00232f);
    if (look >= 0.5f)
        x = pow(x, punchy_power);
    return pow(x, eotf_exponent);
}

/* `highlight` treats very bright pixels, which in flight are emitters. AgX
 * takes them toward white before the look, and the look's saturation then
 * has little colour left to work with.
 *   x = the punchy saturation from EV z up
 *   y, z = EV of scene luminance where the ramp from punchy_saturation to x
 *          starts and ends
 *   w = how much of the result keeps the pixel's hue, reached at EV z
 *       over the same ramp: its brightest channel goes through the AgX
 *       curve, and the other channels keep their ratio to it. */
float3 AeronAgxToneMap(float3 color, float max_ev, float eotf_exponent, float look,
                       float punchy_power, float punchy_saturation, float4 highlight)
{
    color = max(color, 0.0f);

    /* Most pixels are darker than the ramp: they keep the punchy saturation
       and skip the ramp and the second curve. */
    const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float       ramp      = 0.0f;
    if (luminance > exp2(highlight.y))
        ramp = smoothstep(highlight.y, highlight.z, log2(luminance));

    float3 agx = mul(AERON_AGX_INSET_MATRIX, color);
    agx = log2(max(agx, 1e-10f));
    agx = saturate((agx - AERON_AGX_MIN_EV) / (max_ev - AERON_AGX_MIN_EV));
    agx = saturate(AeronAgxDefaultContrastApprox(agx));
    agx = AeronAgxApplyLook(agx, look, punchy_power, lerp(punchy_saturation, highlight.x, ramp));
    agx = mul(AERON_AGX_OUTSET_MATRIX, agx);
    agx = pow(max(agx, 0.0f), float3(eotf_exponent, eotf_exponent, eotf_exponent));

    const float keep = ramp * highlight.w;
    if (keep <= 0.0f)
        return agx;
    const float peak = max(color.r, max(color.g, color.b));
    const float3 hue_kept =
        color / max(peak, 1e-10f) *
        AeronAgxToneMapGrey(peak, max_ev, eotf_exponent, look, punchy_power);
    return lerp(agx, hue_kept, keep);
}

#endif
