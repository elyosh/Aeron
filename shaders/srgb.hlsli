#ifndef AERON_SRGB_HLSLI
#define AERON_SRGB_HLSLI

/* Exact piecewise sRGB transfer functions. They preserve values above 1.0;
 * normalized-image consumers must clamp explicitly at their boundary. */
float3 AeronSrgbToLinear(float3 color)
{
    float3 lo = color / 12.92f;
    float3 hi = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(lo, hi, step(0.04045f, color));
}

float3 AeronLinearToSrgb(float3 color)
{
    color = max(color, 0.0f);
    float3 lo = color * 12.92f;
    float3 hi = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return lerp(lo, hi, step(0.0031308f, color));
}

#endif
