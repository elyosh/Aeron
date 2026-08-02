/*
 * Shared PBR lighting building blocks for the cooked-glb mesh
 * fragment shader.
 *
 * Provides:
 *   - compact, caller-selected cbuffer declarations.
 *   - PbrMaterialParams struct — function-parameter type used by
 *     cook_torrance_spec; the consumer constructs an instance per
 *     fragment from its per-material data.
 *   - world_ambient(N) — Valve/HL2 ambient-cube fill.
 *   - cook_torrance_spec(N, V, L, mp) — GGX D · Smith-Schlick G ·
 *     Schlick F specular BRDF.
 */

#ifndef AERON_SCENE_PBR_LIGHTING_INCLUDED
#define AERON_SCENE_PBR_LIGHTING_INCLUDED

#ifndef AERON_DIRECTIONAL_SHADOW_ONLY
struct PbrMaterialParams
{
    float  spec_intensity;
    float  roughness;
    float3 F0;
    float3 albedo;
};

#ifndef AERON_PBR_LIGHT_UNIFORM_REGISTER
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#endif

/* FS-side mirror of the per-camera light + world-ambient environment.
 * Pushed once per pass by the host. */
cbuffer PbrLightFS : register(AERON_PBR_LIGHT_UNIFORM_REGISTER, space3)
{
    float light_intensity;
    float global_spec_mul;
    float debug_isolate_term;
    float light_wrap;
    float xvt_flat;
    float ssao_intensity;
    float ssao_power;
    float ssao_rt_w;
    float ssao_rt_h;
    float ssao_direct;
    float spec_geom_adapt;
    float3 fs_camera_pos_world;   float _pad_fs_cam;
    /* Engine ships `light_world` (propagation direction); the snapshot
     * negates it on the way in so `fs_directional_dir` arrives as the
     * "to-light" direction (surface → light) — the standard L for
     * Blinn-PBR. */
    float3 fs_directional_dir;    float _pad_fs_dir;
    float3 sun_color;             float _pad_sun;
    float3 amb_pos_x;             float _pad_amb_px;
    float3 amb_neg_x;             float _pad_amb_nx;
    float3 amb_pos_y;             float _pad_amb_py;
    float3 amb_neg_y;             float _pad_amb_ny;
    float3 amb_pos_z;             float _pad_amb_pz;
    float3 amb_neg_z;             float _pad_amb_nz;
    /* Additional diffuse-only directional lights (XWA scenes carry
     * several backdrop suns/planets; the classic sums plain Lambert
     * contributions per light — no wrap, no specular). Unused slots
     * have zero color. dir = surface -> light. */
    float4 fs_extra_dir[3];
    float4 fs_extra_col[3];
    /* Point-light evaluation knobs: minimum distance, specular weight,
     * diffuse wrap, and per-light contribution cap. */
    float4 fs_point_params;
};

struct PbrPointLight
{
    float4 position_range;
    float4 color;
};
StructuredBuffer<PbrPointLight> fs_point_lights : register(t9, space2);
#include "scene_clustered_lights.hlsli"
#endif

#ifndef AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER
#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b1
#endif

cbuffer DirectionalShadowFS : register(AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER, space3)
{
    row_major float4x4 shadow_view_proj[4];
    float4 shadow_atlas_scale_bias[4];
    float4 shadow_atlas_clamp[4];
    float4 shadow_split_data[4];
    float4 shadow_texel_data[4]; /* world units/texel XY, normalized depth/texel XY */
    float4 shadow_params;         /* enabled, count, filter quality, debug */
    float4 shadow_camera_pos;    /* xyz position; w selects face-normal bias */
    float4 shadow_camera_forward;
    float4 shadow_bias;           /* normal, depth texels, slope, max distance */
    float4 shadow_fade;           /* start, end, inverse atlas size, receiver plane */
    float4 shadow_pcss;           /* enabled, tan angular radius, max radius, minimum radius */
    float4 shadow_pcss_temporal;  /* enabled, FSR temporal phase, reserved, reserved */
    float4 shadow_light_dir;      /* xyz = normalized surface-to-light direction */
};

#ifndef AERON_SHADOW_COMPARISON_TEXTURE_REGISTER
#define AERON_SHADOW_COMPARISON_TEXTURE_REGISTER t5
#define AERON_SHADOW_COMPARISON_SAMPLER_REGISTER s5
#define AERON_SHADOW_DEPTH_TEXTURE_REGISTER t6
#define AERON_SHADOW_DEPTH_SAMPLER_REGISTER s6
#endif

Texture2D<float> g_directional_shadow
    : register(AERON_SHADOW_COMPARISON_TEXTURE_REGISTER, space2);
SamplerComparisonState g_directional_shadow_sampler
    : register(AERON_SHADOW_COMPARISON_SAMPLER_REGISTER, space2);
Texture2D<float> g_directional_shadow_depth
    : register(AERON_SHADOW_DEPTH_TEXTURE_REGISTER, space2);
SamplerState g_directional_shadow_depth_sampler
    : register(AERON_SHADOW_DEPTH_SAMPLER_REGISTER, space2);

float sample_shadow_tap(uint cascade, float2 local_uv, float receiver_depth,
                        float2 receiver_depth_per_texel, float2 offset)
{
    float4 scale_bias = shadow_atlas_scale_bias[cascade];
    float2 atlas_uv = local_uv * scale_bias.xy + scale_bias.zw;
    atlas_uv = clamp(atlas_uv + offset * shadow_fade.z,
                     shadow_atlas_clamp[cascade].xy,
                     shadow_atlas_clamp[cascade].zw);
    float tap_receiver_depth =
        saturate(receiver_depth + dot(receiver_depth_per_texel, offset));
    return g_directional_shadow.SampleCmpLevelZero(
        g_directional_shadow_sampler, atlas_uv, tap_receiver_depth);
}

float2 shadow_filter_phase(uint cascade, float2 local_uv)
{
    float4 scale_bias = shadow_atlas_scale_bias[cascade];
    float2 atlas_uv = local_uv * scale_bias.xy + scale_bias.zw;
    return frac(atlas_uv * rcp(max(shadow_fade.z, 1.0e-9f)) - 0.5f);
}

float sample_shadow_raw_depth(uint cascade, float2 local_uv, float2 offset)
{
    float4 scale_bias = shadow_atlas_scale_bias[cascade];
    float2 atlas_uv = local_uv * scale_bias.xy + scale_bias.zw;
    atlas_uv = clamp(atlas_uv + offset * shadow_fade.z,
                     shadow_atlas_clamp[cascade].xy,
                     shadow_atlas_clamp[cascade].zw);
    return g_directional_shadow_depth.SampleLevel(
        g_directional_shadow_depth_sampler, atlas_uv, 0.0f);
}

float2 shadow_pcss_filter_rotation(float2 screen_position)
{
    if (shadow_pcss_temporal.x == 0.0f) {
        return float2(1.0f, 0.0f);
    }

    /* Fine screen-space noise is reprojected and accumulated by FSR. The
     * golden-angle phase visits a low-discrepancy sequence of orientations. */
    float2 pixel = floor(screen_position);
    float spatial_phase = frac(52.9829189f * frac(
        dot(pixel, float2(0.06711056f, 0.00583715f))));
    float temporal_phase = frac(shadow_pcss_temporal.y * 0.38196601125f);
    float sine;
    float cosine;
    sincos(frac(spatial_phase + temporal_phase) * 6.28318530718f,
           sine, cosine);
    return float2(cosine, sine);
}

float2 rotate_shadow_disk_offset(float2 offset, float2 rotation)
{
    return float2(offset.x * rotation.x - offset.y * rotation.y,
                  offset.x * rotation.y + offset.y * rotation.x);
}

/* Blocker search keeps a centre sample for thin occluders, then distributes
 * the remaining taps from the centre to the edge of the adaptive footprint. */
static const float2 pcss_blocker_offsets[16] = {
    float2( 0.0000f,  0.0000f),
    float2(-0.1904f,  0.1744f),
    float2( 0.0319f, -0.3638f),
    float2( 0.2721f,  0.3549f),
    float2(-0.5085f, -0.0899f),
    float2( 0.4871f, -0.3099f),
    float2(-0.1642f,  0.6108f),
    float2(-0.3149f, -0.6062f),
    float2( 0.6860f,  0.2505f),
    float2(-0.7160f,  0.2956f),
    float2( 0.3461f, -0.7395f),
    float2( 0.2563f,  0.8171f),
    float2(-0.7739f, -0.4485f),
    float2( 0.9092f, -0.1999f),
    float2(-0.5556f,  0.7903f),
    float2(-0.1285f, -0.9917f)
};

/* Progressive low-discrepancy disk: the first 8, 16, and 24 entries each
 * cover the complete disk, so quality changes sample count without changing
 * the requested physical radius. */
static const float2 pcss_filter_offsets[24] = {
    float2(-0.5214f,  0.4776f),
    float2( 0.0437f, -0.4981f),
    float2( 0.5269f,  0.6873f),
    float2(-0.3481f, -0.0616f),
    float2( 0.6670f, -0.4243f),
    float2(-0.1590f,  0.5914f),
    float2(-0.4311f, -0.8301f),
    float2( 0.2348f,  0.0858f),
    float2(-0.6933f,  0.2862f),
    float2( 0.2369f, -0.5063f),
    float2( 0.2698f,  0.8601f),
    float2(-0.3746f, -0.2171f),
    float2( 0.8098f, -0.1780f),
    float2(-0.3804f,  0.5411f),
    float2(-0.1244f, -0.9602f),
    float2( 0.1352f,  0.1139f),
    float2(-0.7282f,  0.0301f),
    float2( 0.3759f, -0.3741f),
    float2(-0.0408f,  0.8829f),
    float2(-0.2533f, -0.3035f),
    float2( 0.8029f,  0.1080f),
    float2(-0.5232f,  0.3640f),
    float2( 0.2089f, -0.9288f),
    float2( 0.1522f,  0.2657f)
};

float2 pcss_filter_radius(uint cascade, float2 local_uv,
                          float physical_receiver_depth, float comparison_receiver_depth,
                          float2 physical_depth_per_texel,
                          float2 comparison_depth_per_texel)
{
    float2 normalized_depth_per_texel =
        max(shadow_texel_data[cascade].zw, 1.0e-9f.xx);
    /* A directional source can only be occluded between the receiver and
     * the light-facing end of the orthographic projection. Limit the search
     * to that physical footprint, then apply the configured penumbra cap. */
    float2 search_radius = min(
        shadow_pcss.z.xx,
        max(1.0f - physical_receiver_depth, 0.0f) *
            shadow_pcss.y / normalized_depth_per_texel);
    float blocker_delta_sum = 0.0f;
    float blocker_count = 0.0f;
    [unroll] for (int tap = 0; tap < 16; tap++) {
        float2 offset = pcss_blocker_offsets[tap] * search_radius;
        float tap_comparison_depth = saturate(
            comparison_receiver_depth + dot(comparison_depth_per_texel, offset));
        float blocker_depth = sample_shadow_raw_depth(cascade, local_uv, offset);
        /* Reversed Z: a blocker closer to the light has greater depth. */
        if (blocker_depth > tap_comparison_depth) {
            float tap_physical_depth = saturate(
                physical_receiver_depth + dot(physical_depth_per_texel, offset));
            blocker_delta_sum += max(blocker_depth - tap_physical_depth, 0.0f);
            blocker_count += 1.0f;
        }
    }
    if (blocker_count == 0.0f) {
        return shadow_pcss.w.xx;
    }

    float2 blocker_distance_texels =
        (blocker_delta_sum / blocker_count) /
        normalized_depth_per_texel;
    float2 penumbra_radius = blocker_distance_texels * shadow_pcss.y;
    return clamp(penumbra_radius, shadow_pcss.w.xx, shadow_pcss.z.xx);
}

float sample_shadow_pcss_disk(uint cascade, float2 local_uv, float receiver_depth,
                              float2 receiver_depth_per_texel, float2 filter_radius,
                              int quality, float2 rotation)
{
    int tap_count = quality == 1 ? 8 : (quality == 2 ? 16 : 24);
    float visibility = 0.0f;
    [unroll] for (int tap = 0; tap < 24; tap++) {
        if (tap < tap_count) {
            float2 offset = rotate_shadow_disk_offset(
                pcss_filter_offsets[tap], rotation) * filter_radius;
            visibility += sample_shadow_tap(
                cascade, local_uv, receiver_depth, receiver_depth_per_texel, offset);
        }
    }
    return visibility / (float)tap_count;
}

/* Pair adjacent weighted texels into one bilinear comparison. This evaluates
 * separable tent kernels with 4/9/16 comparisons instead of 9/25/49. */
float sample_shadow_tent3(uint cascade, float2 local_uv, float receiver_depth,
                          float2 receiver_depth_per_texel, float filter_radius)
{
    float2 phase = shadow_filter_phase(cascade, local_uv);
    float filter_scale = clamp(filter_radius, 0.5f, 16.0f) * 0.5f;
    float2 weights_x = float2(3.0f - 2.0f * phase.x, 1.0f + 2.0f * phase.x);
    float2 weights_y = float2(3.0f - 2.0f * phase.y, 1.0f + 2.0f * phase.y);
    float2 offsets_x = float2(
        -1.0f + (2.0f - phase.x) / weights_x.x - phase.x,
         1.0f + phase.x / weights_x.y - phase.x) * filter_scale;
    float2 offsets_y = float2(
        -1.0f + (2.0f - phase.y) / weights_y.x - phase.y,
         1.0f + phase.y / weights_y.y - phase.y) * filter_scale;

    float visibility = 0.0f;
    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            visibility += sample_shadow_tap(
                cascade, local_uv, receiver_depth, receiver_depth_per_texel,
                float2(offsets_x[x], offsets_y[y])) * weights_x[x] * weights_y[y];
        }
    }
    return visibility * (1.0f / 16.0f);
}

float sample_shadow_tent5(uint cascade, float2 local_uv, float receiver_depth,
                          float2 receiver_depth_per_texel, float filter_radius)
{
    float2 phase = shadow_filter_phase(cascade, local_uv);
    float filter_scale = clamp(filter_radius, 0.5f, 16.0f) * 0.5f;
    float3 weights_x = float3(
        3.0f - 2.0f * phase.x, 5.0f, 1.0f + 2.0f * phase.x);
    float3 weights_y = float3(
        3.0f - 2.0f * phase.y, 5.0f, 1.0f + 2.0f * phase.y);
    float3 offsets_x = float3(
        -2.0f + (2.0f - phase.x) / weights_x.x - phase.x,
                 (2.0f + phase.x) / weights_x.y - phase.x,
         2.0f + phase.x / weights_x.z - phase.x) * filter_scale;
    float3 offsets_y = float3(
        -2.0f + (2.0f - phase.y) / weights_y.x - phase.y,
                 (2.0f + phase.y) / weights_y.y - phase.y,
         2.0f + phase.y / weights_y.z - phase.y) * filter_scale;

    float visibility = 0.0f;
    [unroll] for (int y = 0; y < 3; y++) {
        [unroll] for (int x = 0; x < 3; x++) {
            visibility += sample_shadow_tap(
                cascade, local_uv, receiver_depth, receiver_depth_per_texel,
                float2(offsets_x[x], offsets_y[y])) * weights_x[x] * weights_y[y];
        }
    }
    return visibility * (1.0f / 81.0f);
}

float sample_shadow_tent7(uint cascade, float2 local_uv, float receiver_depth,
                          float2 receiver_depth_per_texel, float filter_radius)
{
    float2 phase = shadow_filter_phase(cascade, local_uv);
    float filter_scale = clamp(filter_radius, 0.5f, 16.0f) * 0.5f;
    float4 weights_x = float4(
        3.0f - 2.0f * phase.x, 7.0f - 2.0f * phase.x,
        5.0f + 2.0f * phase.x, 1.0f + 2.0f * phase.x);
    float4 weights_y = float4(
        3.0f - 2.0f * phase.y, 7.0f - 2.0f * phase.y,
        5.0f + 2.0f * phase.y, 1.0f + 2.0f * phase.y);
    float4 offsets_x = float4(
        -3.0f + (2.0f - phase.x) / weights_x.x - phase.x,
        -1.0f + (4.0f - phase.x) / weights_x.y - phase.x,
         1.0f + (2.0f + phase.x) / weights_x.z - phase.x,
         3.0f + phase.x / weights_x.w - phase.x) * filter_scale;
    float4 offsets_y = float4(
        -3.0f + (2.0f - phase.y) / weights_y.x - phase.y,
        -1.0f + (4.0f - phase.y) / weights_y.y - phase.y,
         1.0f + (2.0f + phase.y) / weights_y.z - phase.y,
         3.0f + phase.y / weights_y.w - phase.y) * filter_scale;

    float visibility = 0.0f;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) {
            visibility += sample_shadow_tap(
                cascade, local_uv, receiver_depth, receiver_depth_per_texel,
                float2(offsets_x[x], offsets_y[y])) * weights_x[x] * weights_y[y];
        }
    }
    return visibility * (1.0f / 256.0f);
}

float sample_shadow_cascade(uint cascade, float3 world_pos, float3 geometric_normal,
                            float bias_ndotl, float3 world_pos_dx, float3 world_pos_dy,
                            float2 screen_position)
{
    float4x4 view_proj = shadow_view_proj[cascade];
    float4 physical_clip = mul(view_proj, float4(world_pos, 1.0f));
    float3 biased_world = world_pos + geometric_normal * shadow_bias.x;
    float4 clip = mul(view_proj, float4(biased_world, 1.0f));
    float3 ndc = clip.xyz / max(abs(clip.w), 1.0e-6f);
    if (any(abs(ndc.xy) > 1.0f.xx) || ndc.z < 0.0f || ndc.z > 1.0f) {
        return 1.0f;
    }
    float2 local_uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    float slope = 1.0f + shadow_bias.z * (1.0f - saturate(bias_ndotl));
    float receiver_depth = saturate(ndc.z + shadow_bias.y *
                                    shadow_split_data[cascade].w * slope);
    int quality = clamp((int)round(shadow_params.z), 0, 3);
    float physical_receiver_depth =
        saturate(physical_clip.z / max(abs(physical_clip.w), 1.0e-6f));
    float2 physical_depth_per_texel = float2(0.0f, 0.0f);
    float2 receiver_depth_per_texel = float2(0.0f, 0.0f);
    if (quality > 0 && (shadow_fade.w > 0.0f || shadow_pcss.x != 0.0f)) {
        float2 local_uv_dx = float2(
            dot(view_proj[0].xyz, world_pos_dx) * 0.5f,
            -dot(view_proj[1].xyz, world_pos_dx) * 0.5f);
        float2 local_uv_dy = float2(
            dot(view_proj[0].xyz, world_pos_dy) * 0.5f,
            -dot(view_proj[1].xyz, world_pos_dy) * 0.5f);
        float2 uv_dx = local_uv_dx * shadow_atlas_scale_bias[cascade].xy;
        float2 uv_dy = local_uv_dy * shadow_atlas_scale_bias[cascade].xy;
        float depth_dx = dot(view_proj[2].xyz, world_pos_dx);
        float depth_dy = dot(view_proj[2].xyz, world_pos_dy);
        float determinant = uv_dx.x * uv_dy.y - uv_dx.y * uv_dy.x;
        if (abs(determinant) > 1.0e-12f) {
            float inverse_determinant = rcp(determinant);
            float2 depth_gradient = float2(
                uv_dy.y * depth_dx - uv_dx.y * depth_dy,
                uv_dx.x * depth_dy - uv_dy.x * depth_dx) * inverse_determinant;
            physical_depth_per_texel = depth_gradient * shadow_fade.z;
            /* Near a light-grazing projection the mathematical gradient
             * becomes unbounded. Cap it to eight light-space texels so
             * the correction cannot erase an entire local shadow. */
            float gradient_limit = max(shadow_split_data[cascade].w * 8.0f, 1.0e-7f);
            physical_depth_per_texel = clamp(
                physical_depth_per_texel, -gradient_limit.xx, gradient_limit.xx);
            receiver_depth_per_texel = physical_depth_per_texel * shadow_fade.w;
            receiver_depth_per_texel = clamp(
                receiver_depth_per_texel, -gradient_limit.xx, gradient_limit.xx);
        }
    }
    if (quality == 0) {
        return sample_shadow_tap(cascade, local_uv, receiver_depth,
                                 receiver_depth_per_texel, float2(0.0f, 0.0f));
    }
    if (shadow_pcss.x != 0.0f && shadow_pcss.y > 0.0f) {
        float2 filter_radius = pcss_filter_radius(
            cascade, local_uv, physical_receiver_depth, receiver_depth,
            physical_depth_per_texel, receiver_depth_per_texel);
        float2 filter_rotation = shadow_pcss_filter_rotation(screen_position);
        return sample_shadow_pcss_disk(
            cascade, local_uv, receiver_depth, receiver_depth_per_texel,
            filter_radius, quality, filter_rotation);
    }
    float filter_radius = clamp(shadow_camera_forward.w, 0.5f, 3.0f);
    if (quality == 1) {
        return sample_shadow_tent3(
            cascade, local_uv, receiver_depth, receiver_depth_per_texel, filter_radius);
    }
    if (quality == 2) {
        return sample_shadow_tent5(
            cascade, local_uv, receiver_depth, receiver_depth_per_texel, filter_radius);
    }
    return sample_shadow_tent7(
        cascade, local_uv, receiver_depth, receiver_depth_per_texel, filter_radius);
}

float directional_shadow_visibility(float3 world_pos, float3 geometric_normal,
                                    float ndotl, float bias_ndotl, float3 world_pos_dx,
                                    float3 world_pos_dy, float2 screen_position,
                                    out uint cascade_index,
                                    out float cascade_blend, out float shadow_coverage)
{
    cascade_index = 4u;
    cascade_blend = 0.0f;
    shadow_coverage = 0.0f;
    if (shadow_params.x == 0.0f) {
        return 1.0f;
    }
    float view_depth = dot(world_pos - shadow_camera_pos.xyz,
                           shadow_camera_forward.xyz);
    uint cascade_count = min((uint)shadow_params.y, 4u);
    if (cascade_count == 0u || view_depth < 0.0f || view_depth > shadow_bias.w) {
        return 1.0f;
    }
    [unroll] for (uint c = 0u; c < 4u; c++) {
        if (c < cascade_count && view_depth <= shadow_split_data[c].y) {
            cascade_index = c;
            break;
        }
    }
    if (cascade_index >= cascade_count) {
        return 1.0f;
    }
    if (cascade_index + 1u < cascade_count &&
        view_depth > shadow_split_data[cascade_index].z) {
        cascade_blend = saturate(
            (view_depth - shadow_split_data[cascade_index].z) /
            max(shadow_split_data[cascade_index].y -
                shadow_split_data[cascade_index].z, 1.0e-4f));
    }
    shadow_coverage = 1.0f;
    if (view_depth > shadow_fade.x) {
        shadow_coverage = 1.0f - saturate(
            (view_depth - shadow_fade.x) /
            max(shadow_fade.y - shadow_fade.x, 1.0e-4f));
    }
    /* Keep cascade debug bands independent of surface orientation. The
     * directional contribution is zero here, so no shadow lookup is needed. */
    if (ndotl <= 0.0f) {
        return 1.0f;
    }
    float visibility = sample_shadow_cascade(cascade_index, world_pos,
                                              geometric_normal, bias_ndotl,
                                              world_pos_dx, world_pos_dy,
                                              screen_position);
    if (cascade_blend > 0.0f) {
        float next_visibility = sample_shadow_cascade(
            cascade_index + 1u, world_pos, geometric_normal, bias_ndotl,
            world_pos_dx, world_pos_dy, screen_position);
        visibility = lerp(visibility, next_visibility, cascade_blend);
    }
    return lerp(1.0f, visibility, shadow_coverage);
}

#ifndef AERON_DIRECTIONAL_SHADOW_ONLY
/* World-axis ambient irradiance for a unit-length surface normal.
 * Squared components naturally sum to 1, so the blend stays energy-
 * preserving regardless of N's orientation. Valve / HL2 ambient
 * cube formulation. */
float3 world_ambient(float3 N)
{
    float3 n2 = N * N;
    float3 amb_x = (N.x >= 0.0f) ? amb_pos_x : amb_neg_x;
    float3 amb_y = (N.y >= 0.0f) ? amb_pos_y : amb_neg_y;
    float3 amb_z = (N.z >= 0.0f) ? amb_pos_z : amb_neg_z;
    return n2.x * amb_x + n2.y * amb_y + n2.z * amb_z;
}

/* Cook-Torrance specular BRDF D·G·F / (4·N·L·N·V), with the N·L from
 * the rendering equation absorbed into the denominator so the
 * contribution becomes D·G·F / (4·N·V).
 *
 *   D — GGX / Trowbridge-Reitz NDF.
 *   G — Smith × Smith with Schlick-GGX (Karis k for direct lighting).
 *   F — Schlick Fresnel evaluated on V·H.
 *
 * Roughness is on a perceptual 0..1 scale; α = roughness² puts artist
 * values on a more linear lobe-sharpness curve. The ε on N·V avoids a
 * divide-by-zero at silhouettes. Caller multiplies in `spec_intensity`
 * and `global_spec_mul` and gates with whatever spec_gate it derives
 * from its own diffuse path.
 *
 * `out_G` returns the Smith-Smith G product so the caller's debug-
 * isolate path can visualise it as a grayscale grid. */
float3 cook_torrance_spec(float3 N, float3 V, float3 L,
                           PbrMaterialParams mp,
                           out float out_G)
{
    float3 H = normalize(L + V);
    float ndotl = saturate(dot(N, L));
    float ndotv = saturate(dot(N, V));
    float ndoth = saturate(dot(N, H));
    float vdoth = saturate(dot(V, H));

    float alpha  = mp.roughness * mp.roughness;
    float alpha2 = alpha * alpha;

    float d_denom = ndoth * ndoth * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / (3.14159265f * d_denom * d_denom);

    float k   = (alpha + 1.0f) * (alpha + 1.0f) * (1.0f / 8.0f);
    float G_L = ndotl / (ndotl * (1.0f - k) + k);
    float G_V = ndotv / (ndotv * (1.0f - k) + k);
    float G   = G_L * G_V;

    float3 F = mp.F0 + (1.0f - mp.F0) * pow(1.0f - vdoth, 5.0f);

    out_G = G;
    return (D * G) * F / max(4.0f * ndotv, 1e-4f);
}

/* Clustered punctual-light accumulator. Returns the summed diffuse
 * RADIANCE (caller multiplies by albedo x (1-metallic)) and adds the
 * per-light Cook-Torrance specular into `spec_out` (already scaled by
 * spec_intensity x spec_mul x fs_point_params.y).
 *
 * Attenuation is the CLASSIC XWA curve, not inverse-square: the
 * original contributes intensity * 0.5 / d in lit-color units with a
 * very long visible tail (its cull radius is a selection heuristic,
 * not a shading range), and the light-pool size is art direction. So:
 * atten = win(d/r) * 0.5 / max(d, min_d), where the (1-(d/r)^4)^2
 * window only trims the sub-1% tail (hosts derive r from the source
 * intensity). Diffuse keeps directional shaping the classic lacked,
 * softened toward half-Lambert by the wrap knob so grazing hulls
 * under a passing bolt stay lit like the original. The per-light cap
 * mirrors the classic lit-color saturation (hue-preserving) — engine
 * glow sources saturate by design in the original law. */
void accumulate_point_light(uint light_index, float3 N, float3 N_spec, float3 V,
                            float3 world_pos, PbrMaterialParams mp, float spec_mul,
                            inout float3 diff, inout float3 spec_out)
{
    float  min_d = max(fs_point_params.x, 1.0f);
    float  wrap  = fs_point_params.z;
    float  cap   = fs_point_params.w;
    PbrPointLight point_light = fs_point_lights[light_index];
    float3 dv = point_light.position_range.xyz - world_pos;
    float  r  = point_light.position_range.w;
    float  d2 = dot(dv, dv);
    if (d2 >= r * r) {
        return;
    }
    float  d   = sqrt(d2);
    float3 L   = dv / max(d, 1e-3f);
    float  q2  = d2 / (r * r);
    float  win = 1.0f - q2 * q2;
    win        = win * win;
    float3 radiance = point_light.color.rgb * (win * 0.5f / max(d, min_d));
    if (cap > 0.0f) {
        float m = max(radiance.r, max(radiance.g, radiance.b));
        if (m > cap) {
            radiance *= cap / m;
        }
    }
    float ndotl = dot(N, L);
    float shape = lerp(saturate(ndotl), saturate(ndotl * 0.5f + 0.5f), wrap);
    diff += radiance * shape;
    if (fs_point_params.y > 0.0f) {
        float g_unused;
        spec_out += cook_torrance_spec(N_spec, V, L, mp, g_unused) * radiance *
                    (mp.spec_intensity * spec_mul * fs_point_params.y);
    }
}

float3 accumulate_point_lights(float3 N, float3 N_spec, float3 V, float3 world_pos,
                               float2 screen_position,
                               PbrMaterialParams mp, float spec_mul,
                               inout float3 spec_out)
{
    float3 diff = float3(0.0f, 0.0f, 0.0f);
    if (fs_cluster_enabled != 0u) {
        uint global_count = min(fs_cluster_global_count, AERON_CLUSTER_MAX_GLOBAL_LIGHTS);
        for (uint gi = 0u; gi < global_count; ++gi) {
            accumulate_point_light(fs_cluster_global_indices[gi], N, N_spec, V, world_pos,
                                   mp, spec_mul, diff, spec_out);
        }
        uint cluster_index;
        uint2 header = clustered_light_fragment_header(screen_position, world_pos,
                                                        cluster_index);
        uint count = min(header.x, AERON_CLUSTER_MAX_LIGHTS);
        for (uint pi = 0u; pi < count; ++pi) {
            uint light_index = fs_cluster_indices[
                cluster_index * AERON_CLUSTER_MAX_LIGHTS + pi];
            accumulate_point_light(light_index, N, N_spec, V, world_pos,
                                   mp, spec_mul, diff, spec_out);
        }
    } else {
        for (uint light_index = 0u; light_index < fs_cluster_point_count; ++light_index) {
            accumulate_point_light(light_index, N, N_spec, V, world_pos,
                                   mp, spec_mul, diff, spec_out);
        }
    }
    return diff;
}
#endif

#endif /* AERON_SCENE_PBR_LIGHTING_INCLUDED */
