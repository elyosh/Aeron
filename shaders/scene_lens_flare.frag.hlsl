/* The procedural lens flare (AeronScene_SetLensFlare): the halos and discs
 * of the half-resolution pass plus the hexagons and streaks, as a display
 * value, turned into linear light and scaled by the visible part of the
 * source. */

#include "srgb.hlsli"

Texture2D<float4>        g_smooth   : register(t0, space2);
SamplerState             g_smooth_s : register(s0, space2);
StructuredBuffer<float4> g_elements : register(t1, space2);

#include "scene_lens_flare.hlsli"

float4 main(VSOut i) : SV_Target
{
    /* The half-resolution pass leaves the rest of its image undefined. */
    const float2 uv      = min((i.position.xy - origin.xy) * scale.zw, origin.zw);
    const float3 sum     = g_smooth.SampleLevel(g_smooth_s, uv, 0.0f).rgb + flare_sharp(flare_point(i.position.xy));
    const float3 display = min(sum, 1.0f);
    return float4(AeronSrgbToLinear(display) * (frame.w * i.visible), 0.0f);
}
