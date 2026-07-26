/*
 * Scene batched-billboard fragment shader.
 *
 * Straight atlas sample × interpolated vertex color. Colors are
 * linear RGBA and may exceed 1.0 (HDR emissive boost — TIE explosion
 * / lightning sprites). Per-vertex color also carries the XWA glow
 * fan's core→rim gradient.
 *
 * Alpha-tested then blended: the discard keeps fully-transparent
 * texels from paying the blend op (and keeps additive halos tight on
 * sparse atlas frames). Only SV_Target0 is written — the scene's aux
 * attachment (normal G-buffer) is write-masked by the pipeline.
 */

Texture2D<float4> g_atlas : register(t0, space2);
SamplerState      s_atlas : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 main(VSOut i) : SV_Target
{
    float4 c = g_atlas.Sample(s_atlas, i.uv) * i.color;
    if (c.a < 0.01f)
        discard;
    return c;
}
