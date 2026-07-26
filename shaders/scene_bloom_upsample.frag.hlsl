/*
 * Bloom upsample (dual-filter, 4-tap tent).
 *
 * Samples the smaller mip at four diagonal offsets one DESTINATION
 * texel away from the centre — same kernel as the downsample, but
 * driven by destination-texel size so the filter half-width matches
 * a single destination pixel. Bilinear filtering on the source
 * makes this an effective 9-pixel tent in source-texel space.
 *
 * Output is multiplied by `intensity` for the FINAL composite pass
 * (additive into the flight RT) — chain-internal upsamples set
 * intensity = 1.0. The pipeline's blend state (SRC=ONE, DST=ONE, ADD)
 * carries the add, so this shader just produces the sample.
 */

cbuffer UpsamplePS : register(b0, space3)
{
    /* xy = 1.0 / destination_size (destination texel size in UV).
     * z  = intensity multiplier (1.0 for chain-internal upsamples;
     *      `intensity` for the final composite back into the flight RT).
     * w  = unused. */
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
    /* Output PMA so the additive blend (SRC=ONE) accumulates onto the
     * destination cleanly. Alpha kept at 0 so flight-RT alpha (used by
     * the swapchain compositor for PMA-over math) is not perturbed. */
    return float4(sum, 0.0f);
}
