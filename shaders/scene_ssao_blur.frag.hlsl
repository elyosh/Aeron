/*
 * Separable bilateral blur for the joint AO/shadow visibility RT.
 *
 * Nine physical taps per axis, run as H then V. The endpoint taps have
 * half weight, for a total weight of eight samples; depth weighting
 * preserves silhouettes.
 *
 * Why the half-weight endpoints:
 *   - Offsets -4..4 are centered, so the blur does not translate AO by
 *     half a texel per pass as the former -4..3 kernel did.
 *   - Their total spatial weight remains eight, preserving the former
 *     variance-reduction strength without biasing either direction.
 *
 * Inputs:
 *   t0 — visibility RT to read (RG8: AO, directional shadow)
 *   t1 — depth RT (full-res; nearest-sampled at the half-res AO uv)
 *   s0 — NEAREST + CLAMP_TO_EDGE (shared)
 *
 * Output: RG8 visibility. AO uses the complete nine-tap footprint; shadow
 * visibility uses the already-fetched centre and immediate neighbours so
 * contact shadows are denoised without inheriting AO's broad blur.
 */

cbuffer SSAOBlurUniforms : register(b0, space3)
{
    float2 direction_uv;     /* (inv_w, 0) H pass; (0, inv_h) V pass */
    float  near_z;
    float  _pad0;
    /* View-space width/height of one AO texel, divided by center view Z.
     * Multiplying by center_z gives a local, projection-derived world scale. */
    float2 view_texel_scale;
    float2 _pad1;
};

Texture2D<float2> g_visibility : register(t0, space2);
Texture2D    g_depth   : register(t1, space2);
SamplerState g_sampler : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float2 main(VSOut i) : SV_Target0
{
    uint visibility_w, visibility_h;
    uint depth_w, depth_h;
    g_visibility.GetDimensions(visibility_w, visibility_h);
    g_depth.GetDimensions(depth_w, depth_h);
    uint2 visibility_size = uint2(visibility_w, visibility_h);
    uint2 depth_size = uint2(depth_w, depth_h);
    uint2 center_pixel = min(uint2(i.position.xy), visibility_size - 1u);
    uint2 center_source = min(center_pixel * 2u + 1u, depth_size - 1u);

    /* Sky pixels keep AO=1.0 directly. Sampling neighbours would
     * pull ship silhouettes into the sky-side output, producing
     * visible haloes after the apply pass modulates the scene RT. */
    float center_depth = g_depth.Load(int3(center_source, 0)).r;
    if (center_depth <= 0.0f) {
        return g_visibility.Load(int3(center_pixel, 0));
    }
    float center_z = near_z / center_depth;
    bool horizontal = abs(direction_uv.x) > 0.0f;
    float axis_texel_world =
        center_z * (horizontal ? view_texel_scale.x : view_texel_scale.y);

    /* Centered trapezoidal kernel: offsets -4..4, with half-weight
     * endpoints. Gather first so the depth slope can be estimated before
     * weighting. */
    float zk [9];
    float2 visibility[9];
    bool  okk[9];
    [unroll] for (int g = 0; g < 9; g++) {
        int    k  = g - 4;
        int2 tap_pixel = int2(center_pixel) +
            (horizontal ? int2(k, 0) : int2(0, k));
        tap_pixel = clamp(tap_pixel, int2(0, 0), int2(visibility_size) - 1);
        uint2 tap_source = min(uint2(tap_pixel) * 2u + 1u, depth_size - 1u);
        float  d  = g_depth.Load(int3(tap_source, 0)).r;
        okk[g]    = (d > 0.0f);
        zk [g]    = okk[g] ? near_z / d : 0.0f;
        visibility[g] = g_visibility.Load(int3(tap_pixel, 0));
    }

    /* Gradient-corrected bilateral: estimate the view-space depth slope
     * along the blur axis from the immediate neighbours (k = ±1) and
     * weight each tap by its deviation from that predicted plane rather
     * than from the centre depth. A flat-but-tilted surface then keeps
     * full weights across the whole kernel, so the blur spans the full
     * noise-tile period and cancels the rotation pattern (a plain
     * depth-difference weight collapses the window on grazing surfaces,
     * leaving the 4×4 tile as a visible grid). Genuine depth steps still
     * deviate from the plane and are preserved. */
    float slope = 0.0f;
    if (okk[3] && okk[5]) slope = (zk[5] - zk[3]) * 0.5f;  /* k=+1 vs k=-1 */

    float ao_sum     = 0.0f;
    float weight_sum = 0.0f;
    float shadow_sum = 0.0f;
    float shadow_weight_sum = 0.0f;
    [unroll] for (int t = 0; t < 9; t++) {
        if (!okk[t]) continue;            /* skip sky neighbours */
        float expected = center_z + slope * (float)(t - 4);
        float endpoint_weight = (t == 0 || t == 8) ? 0.5f : 1.0f;
        float tap_world = axis_texel_world * max(1.0f, (float)abs(t - 4));
        float normalized_error =
            abs(zk[t] - expected) / max(tap_world, 1e-6f);
        float w = endpoint_weight * exp(-normalized_error);
        ao_sum     += visibility[t].x * w;
        weight_sum += w;

        int offset = t - 4;
        if (abs(offset) <= 1) {
            float shadow_spatial_weight = offset == 0 ? 2.0f : 1.0f;
            float shadow_weight = w * shadow_spatial_weight;
            shadow_sum += visibility[t].y * shadow_weight;
            shadow_weight_sum += shadow_weight;
        }
    }

    float ao = weight_sum > 0.0f ? ao_sum / weight_sum : visibility[4].x;
    float shadow = shadow_weight_sum > 0.0f
        ? shadow_sum / shadow_weight_sum
        : visibility[4].y;
    return float2(ao, shadow);
}
