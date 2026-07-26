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

Texture2D<float4> g_tex : register(t0, space2);
SamplerState      g_smp : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 tint     : TEXCOORD1;
    float4 bias     : TEXCOORD2;
};

float4 main(VSOut input) : SV_Target
{
    float4 s = g_tex.Sample(g_smp, input.uv);
    if (s.a > 0.97f) s.a = 1.0f;

    float3 rgb = s.rgb * input.tint.rgb + input.bias.rgb * s.a;
    /* bias.a is an additive alpha bias (default 0 = no effect).
     * Callers reproducing color-key-blind OPAQUE blits pass 1 so
     * keyed-out (alpha-0) texels still land opaque — a layer beneath
     * must not bleed through content the source engine copies
     * unconditionally. */
    float  a   = saturate(s.a * input.tint.a + input.bias.a);
    return float4(rgb, a);
}
