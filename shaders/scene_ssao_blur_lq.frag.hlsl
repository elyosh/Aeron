/*
 * Five-tap separable bilateral blur for the LOW joint visibility tier.
 *
 * Run horizontally and vertically, it reconstructs a 5×5 neighbourhood
 * with 10 physical samples. It provides strong variance reduction while
 * remaining well below the HIGH tier's two nine-tap passes.
 * Projection-scaled depth weighting preserves silhouettes and behaves
 * consistently with view distance.
 *
 * Inputs:
 *   t0 — visibility RT to read (RG8: AO, directional shadow)
 *   t1 — depth RT (reverse-Z; nearest-sampled at the half-res AO uv)
 *   s0 — NEAREST + CLAMP_TO_EDGE (shared)
 *   b0 — SSAOBlurUniforms: direction_uv selects the blur axis; near_z and
 *        projected AO-texel scale match the high-quality separable blur.
 *
 * Output: RG8 visibility. Shadow uses only the centre and immediate
 * neighbours, matching the high tier's contact-preserving footprint.
 */

cbuffer SSAOBlurUniforms : register(b0, space3)
{
    float2 direction_uv;     /* (inv_w, 0) H pass; (0, inv_h) V pass */
    float  near_z;
    float  _pad0;
    float2 view_texel_scale; /* AO-texel view size divided by center view Z */
    float2 _pad1;
};

Texture2D<float2> g_visibility : register(t0, space2);
Texture2D    g_depth   : register(t1, space2);
SamplerState g_visibility_sampler : register(s0, space2);
SamplerState g_depth_sampler      : register(s1, space2);

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
    float2 center_visibility_uv = (float2(center_pixel) + 0.5f) / float2(visibility_size);
    float2 center_depth_uv = (float2(center_source) + 0.5f) / float2(depth_size);

    /* Sky pixels keep their AO untouched — sampling neighbours would
     * pull ship silhouettes into the sky-side output, haloing after the
     * forward FS modulates the scene. Matches the separable blur. */
    float center_depth = g_depth.SampleLevel(g_depth_sampler, center_depth_uv, 0.0f).r;
    if (center_depth <= 0.0f) {
        return g_visibility.SampleLevel(g_visibility_sampler, center_visibility_uv, 0.0f);
    }
    float center_z = near_z / center_depth;

    bool horizontal = abs(direction_uv.x) > 0.0f;
    float axis_texel_world =
        center_z * (horizontal ? view_texel_scale.x : view_texel_scale.y);

    /* Gather offsets -2..2 before estimating the local depth slope. */
    float zk [5];
    float2 visibility[5];
    bool  okk[5];
    [unroll] for (int g = 0; g < 5; g++) {
        int k = g - 2;
        int2 tap_pixel = int2(center_pixel) +
            (horizontal ? int2(k, 0) : int2(0, k));
        tap_pixel = clamp(tap_pixel, int2(0, 0), int2(visibility_size) - 1);
        uint2 tap_source = min(uint2(tap_pixel) * 2u + 1u, depth_size - 1u);
        float2 tap_visibility_uv = (float2(tap_pixel) + 0.5f) / float2(visibility_size);
        float2 tap_depth_uv = (float2(tap_source) + 0.5f) / float2(depth_size);
        float d = g_depth.SampleLevel(g_depth_sampler, tap_depth_uv, 0.0f).r;
        okk[g] = d > 0.0f;
        zk[g] = okk[g] ? near_z / d : 0.0f;
        visibility[g] = g_visibility.SampleLevel(g_visibility_sampler, tap_visibility_uv, 0.0f);
    }

    /* Predict a local plane so tilted continuous surfaces keep their blur
     * footprint while true depth discontinuities reject neighbours. */
    float slope = okk[1] && okk[3] ? (zk[3] - zk[1]) * 0.5f : 0.0f;

    float ao_sum     = 0.0f;
    float weight_sum = 0.0f;
    float shadow_sum = 0.0f;
    float shadow_weight_sum = 0.0f;
    [unroll] for (int t = 0; t < 5; t++) {
        if (!okk[t]) continue;
        float offset = (float)(t - 2);
        float expected = center_z + slope * offset;
        float tap_world = axis_texel_world * max(1.0f, abs(offset));
        float normalized_error =
            abs(zk[t] - expected) / max(tap_world, 1e-6f);
        float w = exp(-normalized_error);
        ao_sum += visibility[t].x * w;
        weight_sum += w;

        int sample_offset = t - 2;
        if (abs(sample_offset) <= 1) {
            float shadow_spatial_weight = sample_offset == 0 ? 2.0f : 1.0f;
            float shadow_weight = w * shadow_spatial_weight;
            shadow_sum += visibility[t].y * shadow_weight;
            shadow_weight_sum += shadow_weight;
        }
    }

    float ao = weight_sum > 0.0f ? ao_sum / weight_sum : visibility[2].x;
    float shadow = shadow_weight_sum > 0.0f
        ? shadow_sum / shadow_weight_sum
        : visibility[2].y;
    return float2(ao, shadow);
}
