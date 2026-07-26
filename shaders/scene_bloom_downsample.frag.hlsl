/*
 * Bloom downsample (dual-filter, 4-tap).
 *
 * Each output texel is the bilinear-filtered average of four diagonal
 * source samples one source texel away from the destination centre.
 * Hardware bilinear filtering means each tap already averages a 2x2
 * source neighbourhood, so the effective kernel covers ~4x4 source
 * pixels per output — Gaussian-ish at minimal ALU/bandwidth cost.
 *
 * Cheap-enough for PS4 GCN: 4 texture loads + 3 vector adds per
 * fragment, no LDS, no branching. Pairs with composite_two_rt.vert.hlsl.
 */

cbuffer DownsamplePS : register(b0, space3)
{
    /* xy = 1.0 / source_size (texel size in UV).
     * zw = unused. */
    float4 src_texel;
};

Texture2D<float4> g_src : register(t0, space2);
SamplerState      s_src : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target
{
    float2 t = src_texel.xy;
    float3 a = g_src.Sample(s_src, input.uv + t * float2(-1.0f, -1.0f)).rgb;
    float3 b = g_src.Sample(s_src, input.uv + t * float2( 1.0f, -1.0f)).rgb;
    float3 c = g_src.Sample(s_src, input.uv + t * float2(-1.0f,  1.0f)).rgb;
    float3 d = g_src.Sample(s_src, input.uv + t * float2( 1.0f,  1.0f)).rgb;
    return float4((a + b + c + d) * 0.25f, 1.0f);
}
