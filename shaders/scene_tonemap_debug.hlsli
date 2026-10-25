#ifndef AERON_SCENE_TONEMAP_DEBUG_HLSLI
#define AERON_SCENE_TONEMAP_DEBUG_HLSLI

/* The tonemap's debug views (AeronScenePresentDebug), drawn in place of the
 * image. Both return linear display colours. */

#include "scene_tonemap_agx.hlsli"

/* The false colour of an exposed colour: its luminance in stops around
 * mid-grey (0.18), one band per stop from below -5 to +5 and up, and white
 * from AgX's white up, where the image clips. Matches the legend in the
 * debug UI. */
float3 AeronTonemapFalseColor(float3 exposed)
{
    static const float3 bands[12] = {
        float3(0.02f, 0.00f, 0.05f), /* below -5 */
        float3(0.15f, 0.00f, 0.30f), /* -5 to -4 */
        float3(0.00f, 0.00f, 0.60f), /* -4 to -3 */
        float3(0.00f, 0.20f, 1.00f), /* -3 to -2 */
        float3(0.00f, 0.50f, 0.50f), /* -2 to -1 */
        float3(0.00f, 0.40f, 0.10f), /* -1 to 0 */
        float3(0.25f, 0.25f, 0.25f), /* 0 to +1: mid-grey */
        float3(0.30f, 0.70f, 0.00f), /* +1 to +2 */
        float3(1.00f, 1.00f, 0.00f), /* +2 to +3 */
        float3(1.00f, 0.40f, 0.00f), /* +3 to +4 */
        float3(1.00f, 0.00f, 0.00f), /* +4 to +5 */
        float3(1.00f, 0.00f, 0.50f), /* +5 to AgX's white */
    };
    const float luminance = dot(exposed, float3(0.2126f, 0.7152f, 0.0722f));
    const float stops     = log2(max(luminance, 1e-10f) / 0.18f);
    if (stops >= AERON_AGX_MAX_EV - log2(0.18f))
        return float3(1.0f, 1.0f, 1.0f);
    return bands[(int)clamp(floor(stops) + 6.0f, 0.0f, 11.0f)];
}

/* The image meter's weight of a pixel, as scene_exposure_histogram.comp
 * gives it: black where the pixel isn't counted, else blue for the sky,
 * orange for near geometry and green for other geometry, as bright as the
 * weight. `weights` = (edge, sky, near, near depth). */
float3 AeronTonemapMeterWeight(float3 scene, float2 uv, float depth, float4 weights, bool max_channel)
{
    const float v  = max_channel ? max(scene.r, max(scene.g, scene.b)) : dot(scene, float3(0.2126f, 0.7152f, 0.0722f));
    const float ev = log2(max(v, 1e-30f) * 5464.0f);
    if (!(v > 0.0f) || !isfinite(v) || ev < -8.0f)
        return float3(0.0f, 0.0f, 0.0f);
    const float2 offset = uv * 2.0f - 1.0f;
    const float  edge   = saturate(length(offset) * 0.70710678f);
    const bool   sky    = depth <= 0.0f;
    const bool   near   = !sky && depth > weights.w;
    const float  kind   = sky ? weights.y : (near ? weights.z : 1.0f);
    const float  weight = lerp(1.0f, weights.x, edge) * kind;
    const float3 hue    = sky ? float3(0.2f, 0.4f, 1.0f) : (near ? float3(1.0f, 0.5f, 0.1f) : float3(0.2f, 1.0f, 0.2f));
    return hue * max(weight, 0.05f);
}

/* The depth under `uv`, at the depth texture's own size. */
float AeronTonemapDepthAt(Texture2D<float> depth, float2 uv)
{
    uint width, height;
    depth.GetDimensions(width, height);
    const uint2 texel = min(uint2(uv * float2(width, height)), uint2(width, height) - 1u);
    return depth.Load(int3(texel, 0));
}

#endif
