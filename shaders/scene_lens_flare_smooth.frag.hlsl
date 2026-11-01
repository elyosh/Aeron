/* The lens flare's halos and discs, at half resolution: their sum as a
 * display value, before the clip at 1, which the full-resolution pass adds
 * to the other shapes. */

StructuredBuffer<float4> g_elements : register(t0, space2);

#include "scene_lens_flare.hlsli"

float4 main(VSOut i) : SV_Target
{
    return float4(flare_smooth(flare_point(i.position.xy)), 0.0f);
}
