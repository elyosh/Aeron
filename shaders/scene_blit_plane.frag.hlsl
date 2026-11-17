/*
 * Blit fragment shader for a 2D draw list drawn on planes in 3D
 * (AeronDrawList_RenderPlanes).
 *
 * It is scene_blit.frag, plus the edge coverage of solid fills. A fill on a
 * plane isn't aligned with the target's pixels, and the target may have no
 * multisampling, so its edges are smoothed here. A fill's texture
 * coordinates go from 0 to 1 across it, and scene_blit_plane.vert draws it
 * one pixel of the drawing larger on each side. The coverage of a pixel is
 * how far inside the fill it is, in pixels of the target.
 *
 * Paired with scene_blit_plane.vert.
 */

#include "srgb.hlsli"

Texture2D<float4> g_tex : register(t0, space2);
SamplerState      g_smp : register(s0, space2);

struct VSOut
{
    float4 position   : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 tint       : TEXCOORD1;
    float4 bias       : TEXCOORD2;
    float  monochrome : TEXCOORD3;
    float  source     : TEXCOORD4;
    float  mip_bias   : TEXCOORD5;
    float  solid      : TEXCOORD6;
};

float4 main(VSOut input) : SV_Target
{
    float4 s = g_tex.SampleBias(g_smp, input.uv, input.mip_bias);
    if (input.source > 1.5f)
        s.rgb = s.a > 0.0f ? AeronLinearToSrgb(saturate(s.rgb / s.a)) * s.a : float3(0.0f, 0.0f, 0.0f);
    else if (input.source > 0.5f)
        s.rgb = AeronLinearToSrgb(saturate(s.rgb));
    if (s.a > 0.97f) s.a = 1.0f;

    float3 src = lerp(s.rgb, max(s.r, max(s.g, s.b)).xxx, input.monochrome);
    float3 rgb = src * input.tint.rgb + input.bias.rgb * s.a;
    float  a   = saturate(s.a * input.tint.a + input.bias.a);

    float coverage = 1.0f;
    if (input.solid > 0.5f)
    {
        float2 inside = min(input.uv, 1.0f - input.uv);
        float2 pixel  = max(fwidth(input.uv), float2(1e-6f, 1e-6f));
        float2 c      = saturate(inside / pixel + 0.5f);
        coverage      = c.x * c.y;
    }
    /* The colour is premultiplied, so the coverage scales it whole. */
    return float4(rgb, a) * coverage;
}
