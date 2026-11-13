/*
 * Bloom bright-pass — first stage of the dual-filter chain.
 *
 * Filters the HDR flight RT into the half-resolution bloom mip0. Each
 * bilinear tap sits 0.75 source texel from the output texel's centre on
 * each axis, so it mixes two texels per axis with weights 1/4 and 3/4. The
 * four taps together give a 1-3-3-1 tent over 4x4 source texels. A plain
 * average of the 2x2 texels under the output texel has hard edges: around
 * a thin, very bright line, such as a laser bolt, the bloom then showed as
 * a hard-edged band. There is no brightness threshold: all light
 * scatters in proportion, as in a lens, and what visibly glows is what is
 * far brighter than its surroundings (emitters, AeronScene_SetEmissiveScale).
 * Pairs with composite_two_rt.vert.
 */

cbuffer BrightPS : register(b0, space3)
{
    /* x, y = inverse source dimensions */
    float4 params;
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
    float2 offset = params.xy * 0.75f;
    float3 bloom =
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2(-1.0f, -1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2( 1.0f, -1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2(-1.0f,  1.0f)).rgb) +
        bloom_source(g_src.Sample(s_src, input.uv + offset * float2( 1.0f,  1.0f)).rgb);
    return float4(bloom * 0.25f, 1.0f);
}
