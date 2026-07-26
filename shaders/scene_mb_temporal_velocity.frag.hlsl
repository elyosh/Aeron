/* Reconstruct a display-resolution motion-blur velocity field from FSR's
 * paired dilated depth and motion. Reversed depth makes the largest sample in
 * the 2x2 source footprint the nearest surface. FSR stores current-to-previous
 * target UV motion; Aeron's blur uses current-minus-previous UV motion. */

cbuffer MbTemporalVelocityUniforms : register(b0, space3)
{
    float2 source_texel;
    float native_resolution;
    float _pad;
};

Texture2D    g_depth   : register(t0, space2);
SamplerState g_depth_s : register(s0, space2);
Texture2D    g_motion  : register(t1, space2);
SamplerState g_motion_s: register(s1, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float2 main(VSOut i) : SV_Target0
{
    if (native_resolution > 0.5f)
        return -g_motion.SampleLevel(g_motion_s, i.uv, 0.0f).rg;

    float2 source_position = i.uv / source_texel - 0.5f;
    float2 source_base = floor(source_position);
    float2 source_fraction = frac(source_position);
    float2 best_uv = (source_base + 0.5f) * source_texel;
    float best_depth = -1.0f;

    /* Gather returns the same 2x2 footprint used by bilinear sampling in
     * counter-clockwise order from the lower-left texel. Reorder it into
     * the loop's row-major (00, 10, 01, 11) traversal so depth ties retain
     * exactly the same source pixel as the four-sample implementation. */
    float4 source_depths = g_depth.GatherRed(g_depth_s, i.uv).wzxy;

    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            float2 axis_weight = lerp(1.0f.xx - source_fraction, source_fraction, float2(x, y));
            if (axis_weight.x * axis_weight.y <= 1e-5f)
                continue;
            float2 sample_uv = (source_base + float2(x, y) + 0.5f) * source_texel;
            float sample_depth = source_depths[y * 2 + x];
            if (sample_depth > best_depth) {
                best_depth = sample_depth;
                best_uv = sample_uv;
            }
        }
    }
    return -g_motion.SampleLevel(g_motion_s, best_uv, 0.0f).rg;
}
