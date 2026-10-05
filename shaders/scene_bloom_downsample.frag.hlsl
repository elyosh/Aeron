/*
 * Bloom downsample (13-tap).
 *
 * Thirteen bilinear taps over a 6x6 source footprint (Jimenez, "Next
 * Generation Post Processing in Call of Duty: Advanced Warfare", 2014):
 * five overlapping 4x4 boxes, the centre one weighted 0.5 and the four
 * corner ones 0.125 each. A compact bright source's energy moves gradually
 * between output texels as it shifts, instead of jumping between them,
 * which keeps the coarse levels from snapping while the camera moves.
 *
 * Pairs with composite_two_rt.vert.hlsl.
 */

cbuffer DownsamplePS : register(b0, space3)
{
    /* xy = 1.0 / source_size (texel size in UV).
     * z = input normalization, w = unused. */
    float4 src_texel;
};

Texture2D<float4> g_src : register(t0, space2);
SamplerState      s_src : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float3 tap(float2 uv, float2 t, float x, float y)
{
    return g_src.Sample(s_src, uv + t * float2(x, y)).rgb;
}

float4 main(VSOut input) : SV_Target
{
    float2 t  = src_texel.xy;
    float2 uv = input.uv;
    float3 a = tap(uv, t, -2.0f, -2.0f), b = tap(uv, t, 0.0f, -2.0f), c = tap(uv, t, 2.0f, -2.0f);
    float3 d = tap(uv, t, -2.0f,  0.0f), e = tap(uv, t, 0.0f,  0.0f), f = tap(uv, t, 2.0f,  0.0f);
    float3 g = tap(uv, t, -2.0f,  2.0f), h = tap(uv, t, 0.0f,  2.0f), i = tap(uv, t, 2.0f,  2.0f);
    float3 j = tap(uv, t, -1.0f, -1.0f), k = tap(uv, t, 1.0f, -1.0f);
    float3 l = tap(uv, t, -1.0f,  1.0f), m = tap(uv, t, 1.0f,  1.0f);
    float3 sum = e * 0.125f + (a + c + g + i) * 0.03125f + (b + d + f + h) * 0.0625f +
                 (j + k + l + m) * 0.125f;
    return float4(sum * src_texel.z, 1.0f);
}
