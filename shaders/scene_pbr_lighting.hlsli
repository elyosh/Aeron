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
    /* Fresnel at grazing incidence: 1 except for KHR_materials_specular
     * dielectrics, where it is specularFactor. */
    float  F90;
    float3 albedo;
};

#ifndef AERON_PBR_LIGHT_UNIFORM_REGISTER
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#endif

/* FS-side mirror of the per-camera light + world-ambient environment.
 * Pushed once per pass by the host. */
cbuffer PbrLightFS : register(AERON_PBR_LIGHT_UNIFORM_REGISTER, space3)
{
    float reserved_sun_gain; /* formerly the sun's diffuse gain */
    float reserved_spec_mul; /* formerly the specular multiplier */
    float debug_isolate_term;
    float reserved_wrap;     /* formerly the wrap-lighting amount */
    float reserved_xvt; /* formerly the XvT flat-shading switch */
    float ssao_intensity;
    float ssao_power;
    float2 reserved_ssao_rt; /* formerly the AO target size (now fs_render_size) */
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
     * contributions per light, with no specular). Unused slots have zero
     * illuminance. dir = surface -> light. */
    float4 fs_extra_dir[3];
    float4 fs_extra_col[3];
    /* Reserved: formerly the point-light knobs (the classic law's, then
     * the specular weight). */
	float4 fs_reserved_point;
	/* Reserved: formerly the diffuse environment, now in the scene
	 * lighting block (b2). Keeps the 368-byte layout games mirror. */
	float4 fs_reserved_environment[4];
};

struct PbrPointLight
{
    float4 position_range;
    float4 color;
    float4 half_axis; /* xyz: a tube's half axis, zero for a sphere */
};
#ifndef AERON_PBR_POINT_LIGHT_REGISTER
#define AERON_PBR_POINT_LIGHT_REGISTER t9
#endif
StructuredBuffer<PbrPointLight> fs_point_lights : register(AERON_PBR_POINT_LIGHT_REGISTER, space2);
/* The fragment-side b2 block is the scene lighting block: clustered-light
 * parameters followed by the image-based lighting environment. */
#define AERON_SCENE_LIGHTING_ENVIRONMENT 1
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
    float4 shadow_params;         /* enabled, count, reserved, debug */
    float4 shadow_camera_pos;    /* xyz position; w reserved */
    float4 shadow_camera_forward; /* xyz forward; w reserved */
    float4 shadow_bias;           /* normal texels, depth texels, reserved, max distance */
    float4 shadow_fade;           /* start, end, inverse atlas width and height */
    float4 shadow_pcss;           /* reserved, tan angular radius, max radius, minimum radius */
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
    atlas_uv = clamp(atlas_uv + offset * shadow_fade.zw,
                     shadow_atlas_clamp[cascade].xy,
                     shadow_atlas_clamp[cascade].zw);
    float tap_receiver_depth =
        saturate(receiver_depth + dot(receiver_depth_per_texel, offset));
    return g_directional_shadow.SampleCmpLevelZero(
        g_directional_shadow_sampler, atlas_uv, tap_receiver_depth);
}

float sample_shadow_raw_depth(uint cascade, float2 local_uv, float2 offset)
{
    float4 scale_bias = shadow_atlas_scale_bias[cascade];
    float2 atlas_uv = local_uv * scale_bias.xy + scale_bias.zw;
    atlas_uv = clamp(atlas_uv + offset * shadow_fade.zw,
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

/* Low-discrepancy disk of the 16 filter taps. */
static const float2 pcss_filter_offsets[16] = {
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
    float2( 0.1352f,  0.1139f)
};

/* `lit` is set when the search finds no blocker and covers the minimum
 * filter footprint: the filter would then compare against unoccluded
 * texels only, so the caller skips it. */
float2 pcss_filter_radius(uint cascade, float2 local_uv,
                          float physical_receiver_depth, float comparison_receiver_depth,
                          float2 physical_depth_per_texel,
                          float2 comparison_depth_per_texel, out bool lit)
{
    lit = false;
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
        lit = all(search_radius >= shadow_pcss.w + 1.0f);
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
                              float2 rotation)
{
    float visibility = 0.0f;
    [unroll] for (int tap = 0; tap < 16; tap++) {
        float2 offset = rotate_shadow_disk_offset(
            pcss_filter_offsets[tap], rotation) * filter_radius;
        visibility += sample_shadow_tap(
            cascade, local_uv, receiver_depth, receiver_depth_per_texel, offset);
    }
    return visibility * (1.0f / 16.0f);
}

/* Contact-hardening PCSS, or with `single_tap` one hardware-filtered
 * comparison. */
float sample_shadow_cascade(uint cascade, float3 world_pos, float3 geometric_normal,
                            float3 world_pos_dx, float3 world_pos_dy,
                            float2 screen_position, bool single_tap)
{
    float4x4 view_proj = shadow_view_proj[cascade];
    float4 physical_clip = mul(view_proj, float4(world_pos, 1.0f));
    /* Move the receiver laterally in light space. Expressing the offset in
     * cascade texels keeps the separation consistent across fits and shadow
     * resolutions without coupling it to the filter width. */
    float3 light_dir = shadow_light_dir.xyz;
    float3 lateral_normal = geometric_normal -
                            light_dir * dot(light_dir, geometric_normal);
    float grazing = 1.0f - saturate(dot(geometric_normal, light_dir));
    float grazing2 = grazing * grazing;
    grazing = grazing2 * grazing2;
    float usable_texels = shadow_atlas_scale_bias[cascade].x /
                          max(shadow_fade.z, 1.0e-9f);
    float2 lateral_texel_vector = float2(
        dot(view_proj[0].xyz, lateral_normal),
        dot(view_proj[1].xyz, lateral_normal)) * (0.5f * usable_texels);
    float lateral_length_sq = dot(lateral_normal, lateral_normal);
    float projected_length_sq = dot(lateral_texel_vector, lateral_texel_vector);
    float directional_world_per_texel = sqrt(
        lateral_length_sq / max(projected_length_sq, 1.0e-12f));
    float3 normal_offset = lateral_normal *
                           (grazing * shadow_bias.x * directional_world_per_texel);
    float3 biased_world = world_pos + normal_offset;
    float4 clip = mul(view_proj, float4(biased_world, 1.0f));
    float3 ndc = clip.xyz / max(abs(clip.w), 1.0e-6f);
    if (any(abs(ndc.xy) > 1.0f.xx) || ndc.z < 0.0f || ndc.z > 1.0f) {
        return 1.0f;
    }
    float2 local_uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    float receiver_depth = saturate(ndc.z + shadow_bias.y *
                                    shadow_split_data[cascade].w);
    float physical_receiver_depth =
        saturate(physical_clip.z / max(abs(physical_clip.w), 1.0e-6f));
    float2 physical_depth_per_texel = float2(0.0f, 0.0f);
    float2 receiver_depth_per_texel = float2(0.0f, 0.0f);
    if (!single_tap) {
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
            /* Near a light-grazing projection the mathematical gradient
             * becomes unbounded and can flip sign under tiny raster or
             * cascade changes. Fade it out using the geometric incidence;
             * smoothstep's quadratic approach to zero suppresses the 1/N.L
             * divergence. The endpoint matches the existing eight-texel
             * maximum useful receiver-plane slope. */
            const float max_receiver_plane_slope = 8.0f;
            float receiver_plane_incidence = abs(dot(geometric_normal, light_dir));
            float receiver_plane_weight = smoothstep(
                0.0f, rcp(max_receiver_plane_slope), receiver_plane_incidence);
            physical_depth_per_texel =
                depth_gradient * shadow_fade.zw * receiver_plane_weight;
            float gradient_limit = max(
                shadow_split_data[cascade].w * max_receiver_plane_slope, 1.0e-7f);
            physical_depth_per_texel = clamp(
                physical_depth_per_texel, -gradient_limit.xx, gradient_limit.xx);
            receiver_depth_per_texel = physical_depth_per_texel;
            receiver_depth_per_texel = clamp(
                receiver_depth_per_texel, -gradient_limit.xx, gradient_limit.xx);
        }
    }
    if (single_tap) {
        return sample_shadow_tap(cascade, local_uv, receiver_depth,
                                 receiver_depth_per_texel, float2(0.0f, 0.0f));
    }
    bool lit;
    float2 filter_radius = pcss_filter_radius(
        cascade, local_uv, physical_receiver_depth, receiver_depth,
        physical_depth_per_texel, receiver_depth_per_texel, lit);
    if (lit) {
        return 1.0f;
    }
    return sample_shadow_pcss_disk(
        cascade, local_uv, receiver_depth, receiver_depth_per_texel,
        filter_radius, shadow_pcss_filter_rotation(screen_position));
}

float directional_shadow_visibility(float3 world_pos, float3 geometric_normal,
                                    float ndotl, float3 world_pos_dx,
                                    float3 world_pos_dy, float2 screen_position,
                                    bool single_tap, out uint cascade_index,
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
                                              geometric_normal,
                                              world_pos_dx, world_pos_dy,
                                              screen_position, single_tap);
    if (cascade_blend > 0.0f) {
        float next_visibility = sample_shadow_cascade(
            cascade_index + 1u, world_pos, geometric_normal,
            world_pos_dx, world_pos_dy, screen_position, single_tap);
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
 * and the light's illuminance.
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

    float3 F = mp.F0 + (mp.F90 - mp.F0) * pow(1.0f - vdoth, 5.0f);

    out_G = G;
    return (D * G) * F / max(4.0f * ndotv, 1e-4f);
}

/* Clustered punctual-light accumulator. Returns the summed diffuse
 * ILLUMINANCE x N.L (the caller applies the Lambert BRDF, albedo x
 * (1-metallic) / pi) and adds the per-light Cook-Torrance specular into
 * `spec_out` (already scaled by spec_intensity).
 *
 * Lights are physical spheres of radius color.w giving
 * win * color / max(d, color.w)^2 with Lambert diffuse, where the
 * (1-(d/r)^4)^2 window ends them at their range r. Their GGX alpha widens
 * by half the sphere's angular radius (alpha + r / 2d), so a light passing
 * close to a glossy surface leaves a highlight of its size rather than a
 * pinpoint; the normalized GGX keeps the energy.
 *
 * A light with a half axis is a tube (a laser streak), shaded by
 * accumulate_tube_light: the scene orders tubes after the spheres and
 * each loop below takes them separately, so spheres pay nothing for them.
 * A tube is the segment centre +- half_axis, color.w thick, with the same
 * total intensity. The window measures from the segment. Diffuse is the
 * closed form of Lambert from a line, 2 / (|L0||L1| + L0.L1) times the
 * mean of N.L at its ends, which becomes N.L / d^2 for a short or distant
 * tube. Specular evaluates the BRDF toward the segment point nearest the
 * reflection ray (the representative point), widened by the thickness
 * as a sphere, and scales it by alpha / (alpha + half the tube's angular
 * length): the stretched highlight reflects about the energy of the
 * unstretched one. */
void accumulate_point_light(uint light_index, float3 N, float3 N_spec, float3 V,
                            float3 world_pos, PbrMaterialParams mp,
                            inout float3 diff, inout float3 spec_out)
{
    PbrPointLight point_light = fs_point_lights[light_index];
    float3 dv = point_light.position_range.xyz - world_pos;
    float  r  = point_light.position_range.w;
    float  d2 = dot(dv, dv);
    if (d2 >= r * r) {
        return;
    }
    float  d      = sqrt(d2);
    float3 L      = dv / max(d, 1e-3f);
    float  q2     = d2 / (r * r);
    float  win    = 1.0f - q2 * q2;
    win           = win * win;
    float  near_d = max(d, point_light.color.w);
    float3 radiance = point_light.color.rgb * (win / (near_d * near_d));
    diff += radiance * saturate(dot(N, L));
    PbrMaterialParams spec_mp = mp;
    float alpha = saturate(mp.roughness * mp.roughness + point_light.color.w / (2.0f * near_d));
    spec_mp.roughness = sqrt(alpha);
    float g_unused;
    spec_out += cook_torrance_spec(N_spec, V, L, spec_mp, g_unused) * radiance * mp.spec_intensity;
}

void accumulate_tube_light(uint light_index, float3 N, float3 N_spec, float3 V,
                           float3 world_pos, PbrMaterialParams mp,
                           inout float3 diff, inout float3 spec_out)
{
    PbrPointLight point_light = fs_point_lights[light_index];
    float3 to_center = point_light.position_range.xyz - world_pos;
    float3 half_axis = point_light.half_axis.xyz;
    float  half_len2 = dot(half_axis, half_axis);
    /* The window measures from the segment's nearest point. */
    float3 dv = to_center + half_axis * clamp(-dot(to_center, half_axis) / half_len2, -1.0f, 1.0f);
    float  r  = point_light.position_range.w;
    float  d2 = dot(dv, dv);
    if (d2 >= r * r) {
        return;
    }
    float  q2       = d2 / (r * r);
    float  win      = 1.0f - q2 * q2;
    win             = win * win;
    float  source_r = point_light.color.w;
    float3 L0       = to_center - half_axis;
    float3 L1       = to_center + half_axis;
    float  l0       = max(length(L0), 1e-3f);
    float  l1       = max(length(L1), 1e-3f);
    float3 radiance = point_light.color.rgb *
                      (win * 2.0f / (l0 * l1 + dot(L0, L1) + 2.0f * source_r * source_r));
    float3 n0       = L0 / l0;
    float3 n1       = L1 / l1;
    diff           += radiance * saturate(0.5f * dot(N, n0 + n1));
    float3 R    = reflect(-V, N_spec);
    float  r_a  = dot(R, half_axis);
    float  ts   = saturate((dot(R, L0) * r_a - dot(L0, half_axis)) /
                           max(2.0f * (half_len2 - r_a * r_a), 1e-6f));
    float3 Ls   = L0 + half_axis * (2.0f * ts);
    float  ls   = max(length(Ls), 1e-3f);
    float  alpha = saturate(mp.roughness * mp.roughness + source_r / (2.0f * max(ls, source_r)));
    PbrMaterialParams spec_mp = mp;
    spec_mp.roughness = sqrt(alpha);
    float  spec_scale = alpha / saturate(alpha + 0.5f * length(n0 - n1));
    float  g_unused;
    spec_out += cook_torrance_spec(N_spec, V, Ls / ls, spec_mp, g_unused) * radiance *
                (spec_scale * mp.spec_intensity);
}

float3 accumulate_point_lights(float3 N, float3 N_spec, float3 V, float3 world_pos,
                               float2 screen_position,
                               PbrMaterialParams mp,
                               inout float3 spec_out)
{
    /* Each light list holds its spheres, then its tubes. */
    float3 diff = float3(0.0f, 0.0f, 0.0f);
    if (fs_cluster_enabled != 0u) {
        uint global_count  = min(fs_cluster_global_count, AERON_CLUSTER_MAX_GLOBAL_LIGHTS);
        uint global_sphere = min(fs_cluster_global_sphere_count, global_count);
        for (uint gi = 0u; gi < global_sphere; ++gi) {
            accumulate_point_light(fs_cluster_global_indices[gi], N, N_spec, V, world_pos,
                                   mp, diff, spec_out);
        }
        for (uint gt = global_sphere; gt < global_count; ++gt) {
            accumulate_tube_light(fs_cluster_global_indices[gt], N, N_spec, V, world_pos,
                                  mp, diff, spec_out);
        }
        uint cluster_index;
        uint2 header = clustered_light_fragment_header(screen_position, world_pos,
                                                        cluster_index);
        uint count   = min(header.x & 0xffffu, AERON_CLUSTER_MAX_LIGHTS);
        uint spheres = count - min(header.x >> 16u, count);
        uint base    = cluster_index * AERON_CLUSTER_MAX_LIGHTS;
        for (uint pi = 0u; pi < spheres; ++pi) {
            accumulate_point_light(fs_cluster_indices[base + pi], N, N_spec, V, world_pos,
                                   mp, diff, spec_out);
        }
        for (uint pt = spheres; pt < count; ++pt) {
            accumulate_tube_light(fs_cluster_indices[base + pt], N, N_spec, V, world_pos,
                                  mp, diff, spec_out);
        }
    } else {
        uint tube_start = min(fs_cluster_tube_start, fs_cluster_point_count);
        for (uint light_index = 0u; light_index < tube_start; ++light_index) {
            accumulate_point_light(light_index, N, N_spec, V, world_pos,
                                   mp, diff, spec_out);
        }
        for (uint tube_index = tube_start; tube_index < fs_cluster_point_count; ++tube_index) {
            accumulate_tube_light(tube_index, N, N_spec, V, world_pos,
                                  mp, diff, spec_out);
        }
    }
    return diff;
}
#endif

#endif /* AERON_SCENE_PBR_LIGHTING_INCLUDED */
