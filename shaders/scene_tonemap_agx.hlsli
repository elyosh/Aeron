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

float3 AeronAgxToneMap(float3 color, float max_ev, float eotf_exponent, float look,
                       float punchy_power, float punchy_saturation)
{
    color = mul(AERON_AGX_INSET_MATRIX, max(color, 0.0f));
    color = log2(max(color, 1e-10f));
    color = saturate((color - AERON_AGX_MIN_EV) / (max_ev - AERON_AGX_MIN_EV));
    color = saturate(AeronAgxDefaultContrastApprox(color));
    color = AeronAgxApplyLook(color, look, punchy_power, punchy_saturation);
    color = mul(AERON_AGX_OUTSET_MATRIX, color);
    return pow(max(color, 0.0f),
               float3(eotf_exponent, eotf_exponent, eotf_exponent));
}

#endif
