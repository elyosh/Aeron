/*
 * Bloom upsample (dual-filter, 4-tap tent).
 *
 * Samples the smaller mip at four diagonal offsets one DESTINATION
 * texel away from the centre — same kernel as the downsample, but
 * driven by destination-texel size so the filter half-width matches
 * a single destination pixel. Bilinear filtering on the source
 * makes this an effective 9-pixel tent in source-texel space.
 *
 * The RGB output weights the source contribution. Fragment alpha weights
 * the existing destination band through ONE / SRC_ALPHA additive blending.
 */

cbuffer UpsamplePS : register(b0, space3)
{
    /* xy = 1.0 / destination_size, zw = source/destination weights. */
    float4 dst_texel;
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
    float2 t = dst_texel.xy;
    float3 a = g_src.Sample(s_src, input.uv + t * float2(-1.0f, -1.0f)).rgb;
    float3 b = g_src.Sample(s_src, input.uv + t * float2( 1.0f, -1.0f)).rgb;
    float3 c = g_src.Sample(s_src, input.uv + t * float2(-1.0f,  1.0f)).rgb;
    float3 d = g_src.Sample(s_src, input.uv + t * float2( 1.0f,  1.0f)).rgb;
    float3 sum = (a + b + c + d) * 0.25f * dst_texel.z;
    return float4(sum, dst_texel.w);
}
