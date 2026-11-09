/*
 * Shared blit fragment shader.
 *
 * Samples one texture; applies a tint multiplier and an alpha-weighted
 * bias (used by the cross-fade colour-target replicator to lerp toward
 * fade.rgb on transparent regions, with the bias gated by sample.a so
 * it only paints inside actor coverage). Output is intended for the
 * hardware PMA-over blend.
 *
 * Near-opaque sampled alpha is snapped to 1.0 — BC7-compressed cockpit
 * textures lose ~1% precision in nominally-opaque chrome pixels, and
 * with HDR amplification downstream that residual would let bright
 * scene pixels (laser bolts, explosion cores) leak through "opaque"
 * cockpit chrome via the PMA over blend. Threshold of 0.97 leaves
 * genuine AA gradients (text edges, cross-fade tints) untouched while
 * clamping compression / filter noise to a clean opaque.
 *
 * Paired with either gpu_blit.vert (instanced rect) or
 * gpu_blit4.vert (4-corner free quad) — both emit the same VSOut. */

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
};

float4 main(VSOut input) : SV_Target
{
    /* The bias is applied here because Metal samplers ignore the
     * sampler's mip_lod_bias. */
    float4 s = g_tex.SampleBias(g_smp, input.uv, input.mip_bias);
    /* A list that blends sRGB values reads sRGB values from every texture
     * (Dl2dSource in draw_list2d.c). 1: an sRGB texture, which the GPU has
     * converted to linear. Its stored values, sRGB values premultiplied by
     * alpha, come back exactly. 2: a float texture with linear premultiplied
     * colours, converted to premultiplied sRGB colours. */
    if (input.source > 1.5f)
        s.rgb = s.a > 0.0f ? AeronLinearToSrgb(saturate(s.rgb / s.a)) * s.a : float3(0.0f, 0.0f, 0.0f);
    else if (input.source > 0.5f)
        s.rgb = AeronLinearToSrgb(saturate(s.rgb));
    if (s.a > 0.97f) s.a = 1.0f;

    /* Monochrome replaces RGB with the largest channel, so the tint sets
     * the colour and the texel only sets the brightness. The result stays
     * premultiplied, like s.rgb. */
    float3 src = lerp(s.rgb, max(s.r, max(s.g, s.b)).xxx, input.monochrome);
    float3 rgb = src * input.tint.rgb + input.bias.rgb * s.a;
    /* bias.a is an additive alpha bias (default 0 = no effect).
     * Callers reproducing color-key-blind OPAQUE blits pass 1 so
     * keyed-out (alpha-0) texels still land opaque — a layer beneath
     * must not bleed through content the source engine copies
     * unconditionally. */
    float  a   = saturate(s.a * input.tint.a + input.bias.a);
    return float4(rgb, a);
}
