/*
 * Bloom upsample (3x3 tent).
 *
 * Nine bilinear taps one SOURCE texel apart, weighted 1-2-1 per axis
 * (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
 * Warfare", 2014). Taps a single coarse texel's neighbourhood smoothly, so a
 * compact bright source does not show the coarse level's texel grid or snap
 * to it as it moves; four taps half a source texel apart reduce to a plain
 * bilinear fetch of the coarse level and do.
 *
 * The RGB output weights the source contribution. Fragment alpha weights
 * the existing destination band through ONE / SRC_ALPHA additive blending.
 */

cbuffer UpsamplePS : register(b0, space3)
{
    /* xy = 1.0 / destination_size, zw = source/destination weights. */
    float4 dst_texel;
    /* xy = 1.0 / source_size. */
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
    float3 sum = g_src.Sample(s_src, input.uv).rgb * 4.0f;
    sum += (g_src.Sample(s_src, input.uv + float2(-t.x, 0.0f)).rgb +
            g_src.Sample(s_src, input.uv + float2( t.x, 0.0f)).rgb +
            g_src.Sample(s_src, input.uv + float2(0.0f, -t.y)).rgb +
            g_src.Sample(s_src, input.uv + float2(0.0f,  t.y)).rgb) * 2.0f;
    sum += g_src.Sample(s_src, input.uv + float2(-t.x, -t.y)).rgb +
           g_src.Sample(s_src, input.uv + float2( t.x, -t.y)).rgb +
           g_src.Sample(s_src, input.uv + float2(-t.x,  t.y)).rgb +
           g_src.Sample(s_src, input.uv + float2( t.x,  t.y)).rgb;
    return float4(sum * (dst_texel.z / 16.0f), dst_texel.w);
}
