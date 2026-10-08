/*
 * Bloom bright-pass — first stage of the dual-filter chain.
 *
 * Averages the four HDR flight-RT texels covered by each half-resolution
 * output texel into bloom mip0. There is no brightness threshold: all light
 * scatters in proportion, as in a lens, and what visibly glows is what is
 * far brighter than its surroundings (emitters, AeronScene_SetEmissiveScale).
 * Pairs with composite_two_rt.vert.
 */

cbuffer BrightPS : register(b0, space3)
{
    /* x, y = inverse source dimensions */
    float4 params;
    /* x = finest-band weight. */
    float4 band_weight;
};

Texture2D<float4> g_src : register(t0, space2);
SamplerState      s_src : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float3 bloom_source(float3 c)
{
    /* Sanitize before the chain: a single NaN/Inf scene pixel (e.g. from a
     * degenerate authored normal) gets box-averaged by the downsample mips
     * and upsampled into a large black square. Drop non-finite samples to
     * zero here so one bad pixel can't poison the whole bloom buffer. This
     * is a catch-all net — the normal NaN itself is fixed at source in the
     * mesh VS, but bloom must stay robust to any NaN source. */
    if (any(isnan(c)) || any(isinf(c)))
        c = float3(0.0f, 0.0f, 0.0f);
    return c;
}

float4 main(VSOut input) : SV_Target
{
    float2 offset = params.xy * 0.5f;
    float3 bloom =
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2(-1.0f, -1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2( 1.0f, -1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2(-1.0f,  1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2( 1.0f,  1.0f)).rgb);
    return float4(bloom * (0.25f * band_weight.x), 1.0f);
}
