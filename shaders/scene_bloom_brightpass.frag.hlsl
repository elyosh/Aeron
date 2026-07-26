/*
 * Bloom bright-pass — first stage of the dual-filter chain.
 *
 * Samples the HDR flight RT at half resolution and extracts pixels
 * above 1.0 into bloom mip0. Curve is a smoothstep gate:
 *   out = c * smoothstep(thr, thr + knee, br)
 * where `br = max(c.r, c.g, c.b)` so single-channel saturates
 * (pure-red bolt at (3, 0.4, 0.4) has br = 3) still bloom.
 *
 * Emissive meshes (laser/missile crafts, engine slots, explosion
 * billboards) are boosted >1.0 host-side; everything else stays
 * ≤ 1.0 and the gate rejects it. Pairs with composite_two_rt.vert.
 */

cbuffer BrightPS : register(b0, space3)
{
    /* x = threshold (0..1)
     * y = knee width (0..1, soft ramp half-width)
     * z, w = unused */
    float4 params;
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
    float3 c = g_src.Sample(s_src, input.uv).rgb;

    /* Sanitize before the chain: a single NaN/Inf scene pixel (e.g. from a
     * degenerate authored normal) gets box-averaged by the downsample mips
     * and upsampled into a large black square. Drop non-finite samples to
     * zero here so one bad pixel can't poison the whole bloom buffer. This
     * is a catch-all net — the normal NaN itself is fixed at source in the
     * mesh VS, but bloom must stay robust to any NaN source. */
    if (any(isnan(c)) || any(isinf(c)))
        c = float3(0.0f, 0.0f, 0.0f);

    float thr  = params.x;
    float knee = max(params.y, 1e-4f);

    float br = max(c.r, max(c.g, c.b));
    float w  = smoothstep(thr, thr + knee, br);
    return float4(c * w, 1.0f);
}
