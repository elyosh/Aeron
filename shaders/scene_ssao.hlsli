/*
 * Half-resolution visibility: screen-space ambient occlusion (classic
 * random-kernel algorithm, normals sourced from the G-buffer written by the
 * prepass) and the opaque receivers' directional shadow.
 *
 * Two passes share this file. scene_ssao.frag writes AO (when enabled) and
 * the cascade shadow; with AERON_SSAO_RECEIVER_LOCAL_PASS,
 * scene_ssao_receiver_local.frag then rewrites the shadow (G only) of the
 * pixels the prepass flagged as receiver-local (the cockpit) from that map,
 * which the first pass skips while it is active.
 *
 * Inputs:
 *   t0 — flight depth RT (D32_FLOAT, reverse-Z: 1 at near, 0 at far)
 *   t1 — flight normal RT (R16G16_SNORM, octahedral-encoded world
 *        normal — produced by mesh FSes in the geometry sub-pass)
 *   s0 — NEAREST sampler with CLAMP_TO_EDGE (shared)
 *   b0 — SSAOUniforms: scalar projection params + SSAO knobs +
 *        world→view rotation 3×3 + pass switches
 *   b1, t2/t3 — the directional shadow: cascades in the first pass, the
 *        receiver-local map in the second
 *
 * Output:
 *   RGBA8 visibility in [0, 1]. R is ambient occlusion; G is main
 *   directional-shadow visibility for opaque forward shading; B, with probe
 *   occlusion (occupancy_dims.w), what a cone along the mirror direction
 *   meets in the occupancy; else, while the key light is in the probes
 *   (visibility_params.z, the distance of 1 m), its shadow 1 m out along
 *   the normal, for surfaces in shadow turning away from it; else G
 *   (scene_pbr_mesh_impl.hlsli's probe light).
 *
 * Coordinate space: works in view space ("eye space") using
 * mat4_perspective_reverse_z_xy's projection. The G-buffer normal is
 * in WORLD space; we multiply by the world→view rotation 3×3 to land
 * in view space, then hemisphere-sample around it.
 *
 * Pipeline:
 *   1. Sample depth. depth == 0 (reverse-Z far plane = sky) → return 1.
 *   2. Sample normal RT, octahedral decode, transform world → view.
 *   3. Reconstruct view-space position via the scalar projection params.
 *   4. Tangent basis around the normal, rotated per-pixel procedurally.
 *   5. For each of N tangent-space hemisphere offsets:
 *        sample_view = view_pos + radius * (TBN × offset).
 *        Forward-project sample_view → sample_uv.
 *        Compare actual surface view-Z against sample's view-Z.
 *        Range-attenuate so far samples don't false-positive on
 *        silhouettes.
 *   6. Visibility = 1 - average occlusion × intensity.
 */

#ifndef AERON_SSAO_RECEIVER_LOCAL_PASS
#define AERON_SSAO_RECEIVER_LOCAL_PASS 0
#endif

#include "octahedral_normal.hlsli"

/* The ordinary material shader uses t5/t6. The joint half-resolution
 * visibility pass keeps depth/normal at t0/t1 and binds the same shadow
 * resources contiguously at t2/t3. */
#define AERON_DIRECTIONAL_SHADOW_ONLY
#define AERON_SHADOW_COMPARISON_TEXTURE_REGISTER t2
#define AERON_SHADOW_COMPARISON_SAMPLER_REGISTER s2
#define AERON_SHADOW_DEPTH_TEXTURE_REGISTER t3
#define AERON_SHADOW_DEPTH_SAMPLER_REGISTER s3
#include "scene_pbr_lighting.hlsli"

cbuffer SSAOUniforms : register(b0, space3)
{
    /* Scalar projection params — same values mat4_perspective_reverse_z_xy
     * encodes into the projection matrix. tan_h_half = 1/m[0] etc. */
    float tan_h_half;
    float tan_v_half;
    float near_z;
    float proj_y_offset;      /* m[6] — reticle-aligned NDC Y bias */
    /* SSAO knobs. radius/bias are in view-space units. */
    float radius_view;
    float bias_view;
    float intensity;
    /* 1.0 = LOW tier: 8 taps; 0.0 = HIGH tier: 16 taps. */
    float low_quality;
    float proj_x_offset;      /* m[2] — off-center NDC X bias */
    /* Screen-space radius clamp (host: AeronScenePostDesc.ssao_*). Fractions
     * of the NDC half-width; 0 = disabled. min raises the effective radius on
     * distant surfaces (footprint floor) to kill the aliasing grid; max caps
     * it on near surfaces to keep the finite tap set dense. */
    float min_screen_frac;
    float max_screen_frac;
    /* Per-pixel kernel-radius jitter amount [0,1]. Breaks the discrete taps
     * into blur-smoothable noise so a large footprint does not resolve into
     * separate offset occlusions. 0 = off. */
    float sample_jitter;
    /* World→view rotation 3×3, padded to vec4 rows for HLSL cbuffer
     * 16-byte slot alignment (host packs as float[3][4] to match).
     * Transforms the G-buffer world normal into view space. */
    float4 view_rot[3];
    /* x = AO enabled (else R is 1), y = receiver-local pixels skipped (the
     * second pass writes their shadow), z = 1 m in scene units while the
     * key light is in the probes (B), else 0, w reserved. */
    float4 visibility_params;
    /* Probe occlusion (probe_occlusion.c): world to occupancy cells, and the
     * occupancy's cells along x, y, z, w = 1 while it applies (B). */
    float4 occupancy_rows[3];
    float4 occupancy_dims;
    /* Up to four boxes in occupancy cells holding the occluders (lo, hi;
     * lo.w = 1 when used): the cone runs only through them. */
    float4 occupancy_boxes[8];
};

Texture2D    g_depth   : register(t0, space2);
/* Geometric world normal G-buffer (R16G16_SNORM, octahedral). Written by
 * the prepass from world-position derivatives — the true face normal. */
Texture2D    g_normal  : register(t1, space2);
SamplerState g_depth_sampler  : register(s0, space2);
SamplerState g_normal_sampler : register(s1, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

/* Hemisphere sample offsets in tangent space (z = up-along-normal).
 * 16 points distributed on a Vogel disk (golden-angle increment) +
 * cosine-weighted hemisphere lift + quadratic radius bias. No axis
 * alignment by construction — the golden angle is irrational w.r.t.
 * π so no sample lands exactly on a cardinal direction. Avoids the
 * anisotropy that a hand-coded or Hammersley-base-2 kernel can have
 * (where π/2-multiples in the second sequence put many samples on
 * the x-axis).
 *
 * LOW has its own 8-point construction over the same full radial interval.
 * Taking the first half of the high kernel would retain only its short-radius
 * samples, changing AO scale and average darkness instead of only variance. */
#define KERNEL_COUNT 16
static const float3 KERNEL_HIGH[KERNEL_COUNT] = {
    float3( 0.0064f,  0.0165f, 0.0984f),
    float3(-0.0285f, -0.0141f, 0.0990f),
    float3( 0.0440f, -0.0128f, 0.1065f),
    float3(-0.0330f,  0.0544f, 0.1202f),
    float3(-0.0170f, -0.0853f, 0.1390f),
    float3( 0.0944f,  0.0696f, 0.1620f),
    float3(-0.1546f,  0.0168f, 0.1880f),
    float3( 0.1336f, -0.1524f, 0.2158f),
    float3( 0.0054f,  0.2595f, 0.2438f),
    float3(-0.2253f, -0.2366f, 0.2703f),
    float3( 0.4041f,  0.0271f, 0.2932f),
    float3(-0.3868f,  0.3091f, 0.3097f),
    float3( 0.0920f, -0.5904f, 0.3162f),
    float3( 0.3947f,  0.5937f, 0.3068f),
    float3(-0.8170f, -0.2017f, 0.2707f),
    float3( 0.8642f, -0.4715f, 0.1768f),
};

/* Independent N=8 Vogel/cosine hemisphere using the same quadratic
 * 0.1..1.0 radius progression as KERNEL_HIGH. */
#define KERNEL_LOW_COUNT 8
static const float3 KERNEL_LOW[KERNEL_LOW_COUNT] = {
    float3( 0.0091f,  0.0233f, 0.0968f),
    float3(-0.0460f, -0.0227f, 0.1067f),
    float3( 0.0931f, -0.0271f, 0.1438f),
    float3(-0.0911f,  0.1500f, 0.1990f),
    float3(-0.0574f, -0.2898f, 0.2605f),
    float3( 0.3737f,  0.2744f, 0.3126f),
    float3(-0.6821f,  0.0741f, 0.3296f),
    float3( 0.6392f, -0.7273f, 0.2500f),
};

float3 view_pos_from(float2 uv, float depth)
{
    float ndc_x = uv.x * 2.0f - 1.0f;
    float ndc_y = 1.0f - uv.y * 2.0f;
    float eye_z = near_z / depth;
    float eye_x = (ndc_x - proj_x_offset) * tan_h_half * eye_z;
    float eye_y = -(ndc_y - proj_y_offset) * tan_v_half * eye_z;
    return float3(eye_x, eye_y, eye_z);
}

bool project_view(float3 eye, out float2 out_uv)
{
    if (eye.z <= 0.0f) { out_uv = float2(0, 0); return false; }
    float ndc_x = eye.x / (tan_h_half * eye.z) + proj_x_offset;
    float ndc_y = -eye.y / (tan_v_half * eye.z) + proj_y_offset;
    out_uv = float2(ndc_x * 0.5f + 0.5f, 0.5f - ndc_y * 0.5f);
    return true;
}

float evaluate_kernel_sample(float3 view_pos, float3 tangent, float3 bitangent,
                             float3 view_normal, float3 kernel_sample,
                             float radius, float bias)
{
    float3 offset = kernel_sample * radius;
    float3 sample_view = view_pos
                       + tangent * offset.x
                       + bitangent * offset.y
                       + view_normal * offset.z;

    float2 sample_uv;
    if (!project_view(sample_view, sample_uv))
        return 0.0f;
    if (any(sample_uv < 0.0f) || any(sample_uv > 1.0f))
        return 0.0f;

    float actual = g_depth.SampleLevel(g_depth_sampler, sample_uv, 0.0f).r;
    if (actual <= 0.0f)
        return 0.0f;

    float3 actual_view = view_pos_from(sample_uv, actual);
    float occluded = actual_view.z < sample_view.z - bias ? 1.0f : 0.0f;
    /* Smoothstep range check (LearnOpenGL convention): full weight within
     * radius, smoothly attenuating beyond to avoid silhouette cutoffs. */
    float range = smoothstep(
        0.0f, 1.0f,
        radius / max(abs(actual_view.z - view_pos.z), 1e-6f));
    return occluded * range;
}

float3 world_pos_from_view(float3 view_pos)
{
    float3 world_offset;
    world_offset.x = dot(float3(view_rot[0].x, view_rot[1].x, view_rot[2].x), view_pos);
    world_offset.y = dot(float3(view_rot[0].y, view_rot[1].y, view_rot[2].y), view_pos);
    world_offset.z = dot(float3(view_rot[0].z, view_rot[1].z, view_rot[2].z), view_pos);
    return shadow_camera_pos.xyz + world_offset;
}

/* Reconstruct the opaque surface represented by the prepass. Shadow
 * visibility is evaluated once per half-resolution pixel, then shares
 * AO's bilateral passes. Use the geometric G-buffer normal for bias;
 * direct-light orientation is applied later by the material shader. */
float receiver_shadow(float3 world_pos, float3 world_pos_dx, float3 world_pos_dy,
                      float3 world_normal, uint2 source_pixel)
{
    uint  cascade;
    float cascade_blend;
    float shadow_coverage;
    return directional_shadow_visibility(
        world_pos, world_normal, 1.0f, world_pos_dx, world_pos_dy,
        float2(source_pixel) + 0.5f, false, cascade, cascade_blend, shadow_coverage);
}

/* The key light's shadow 1 m out along the normal, for a surface in its
 * shadow turning away from it: what covers the space in front of the
 * surface, not its own curve. One comparison; the blur softens it. */
float key_shadow(float3 world_pos, float3 world_normal, float shadow, uint2 source_pixel)
{
    if (visibility_params.z <= 0.0f || shadow >= 1.0f || dot(world_normal, shadow_light_dir.xyz) >= 0.7f)
        return shadow;
    uint  cascade;
    float cascade_blend;
    float shadow_coverage;
    return directional_shadow_visibility(world_pos + world_normal * visibility_params.z, shadow_light_dir.xyz,
                                         1.0f, float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f),
                                         float2(source_pixel) + 0.5f, true, cascade, cascade_blend,
                                         shadow_coverage);
}

#if !AERON_SSAO_RECEIVER_LOCAL_PASS
Texture3D<float> g_occupancy : register(t4, space2);
SamplerState g_occupancy_sampler : register(s4, space2);

/* What a cone of 8 degrees along the mirror direction meets in the probe
 * occlusion's occupancy (cells of 0.25 m): 16 steps from 0.45 m to 40 m,
 * growing with the distance and the cone's width, each reading the mip as
 * wide as the cone. 1: nothing in the way. */
float occlusion_cone(float3 world_pos, float3 world_normal)
{
    float3 R  = reflect(normalize(world_pos - shadow_camera_pos.xyz), world_normal);
    float4 wp = float4(world_pos, 1.0f);
    float3 p0 = float3(dot(occupancy_rows[0], wp), dot(occupancy_rows[1], wp), dot(occupancy_rows[2], wp));
    float3 d  = normalize(float3(dot(occupancy_rows[0].xyz, R), dot(occupancy_rows[1].xyz, R),
                                 dot(occupancy_rows[2].xyz, R)));
    float3 n  = normalize(float3(dot(occupancy_rows[0].xyz, world_normal), dot(occupancy_rows[1].xyz, world_normal),
                                 dot(occupancy_rows[2].xyz, world_normal)));
    p0 += n * 0.2f; /* 5 cm off the surface */
    /* Only across the occluders' boxes the ray passes. */
    float3 inv   = 1.0f / select(abs(d) > 1.0e-6f, d, select(d >= 0.0f, 1.0e-6f, -1.0e-6f));
    float  enter = 1.0e30f;
    float  leave = -1.0e30f;
    [unroll] for (uint b = 0u; b < 4u; ++b) {
        if (occupancy_boxes[2u * b].w == 0.0f) continue;
        float3 ta = (occupancy_boxes[2u * b].xyz - p0) * inv;
        float3 tb = (occupancy_boxes[2u * b + 1u].xyz - p0) * inv;
        float  e  = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z));
        float  l  = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z));
        if (l >= max(e, 1.8f) && e <= 160.0f) {
            enter = min(enter, e);
            leave = max(leave, l);
        }
    }
    if (leave < 0.0f) return 1.0f;
    const float tan_half = 0.1405f;
    const float growth   = pow(160.0f / 1.8f, 1.0f / 15.0f);
    float       t        = max(enter, 1.8f);
    float       alpha    = 0.0f;
    [loop] for (uint k = 0u; k < 16u; ++k) {
        float  width = max(2.0f * t * tan_half, 1.0f);
        float3 p     = p0 + d * t;
        if (t > leave || any(p < 0.0f) || any(p > occupancy_dims.xyz)) break;
        float occupied = g_occupancy.SampleLevel(g_occupancy_sampler, p / occupancy_dims.xyz, min(log2(width), 4.0f));
        alpha += (1.0f - alpha) * occupied;
        if (alpha > 0.99f) break;
        t = max(t + 0.75f * width, t * growth);
    }
    return 1.0f - alpha;
}
#endif

float4 main(VSOut i) : SV_Target0
{
    /* A half-resolution pixel centre lies between full-resolution G-buffer
     * pixel centres. Select one concrete pixel from its 2x2 footprint and
     * reconstruct through that pixel's centre; combining a nearest depth
     * with the half-resolution UV creates a receiver that is not on the
     * sampled surface and can alias against the shadow-map texel lattice. */
    uint depth_w, depth_h;
    g_depth.GetDimensions(depth_w, depth_h);
    uint2 depth_size = uint2(depth_w, depth_h);
    uint2 visibility_size = (depth_size + 1u) / 2u;
    uint2 visibility_pixel = min(uint2(i.uv * float2(visibility_size)), visibility_size - 1u);
    uint2 source_pixel = min(visibility_pixel * 2u + 1u, depth_size - 1u);
    float2 receiver_uv = (float2(source_pixel) + 0.5f) / float2(depth_size);

    float depth = g_depth.SampleLevel(g_depth_sampler, receiver_uv, 0.0f).r;
    if (depth <= 0.0f) return float4(1.0f, 1.0f, 1.0f, 1.0f); /* sky never occludes */

    float3 view_pos = view_pos_from(receiver_uv, depth);

    /* Geometric world normal from the G-buffer (written by the prepass at
     * full res — the true face normal, free of the Gouraud tilt and of the
     * grid that per-pixel half-res reconstruction here produced). Decode
     * the octahedral encoding, then rotate world→view for the hemisphere.
     * The R16G16_SNORM samples in [-1, 1], the octahedral output range. */
    float2 enc = g_normal.SampleLevel(g_normal_sampler, receiver_uv, 0.0f).rg;
    float3 world_normal = oct_decode(enc);
    bool receiver_local = oct_flag(enc);
    /* Derivatives before any per-pixel branch. */
    float3 world_pos = world_pos_from_view(view_pos);
    float3 world_pos_dx = ddx(world_pos);
    float3 world_pos_dy = ddy(world_pos);
#if AERON_SSAO_RECEIVER_LOCAL_PASS
    if (!receiver_local) discard;
    return float4(0.0f, receiver_shadow(world_pos, world_pos_dx, world_pos_dy, world_normal, source_pixel), 0.0f,
                  0.0f);
#else
    float shadow = 1.0f;
    if (!receiver_local || visibility_params.y == 0.0f) {
        shadow = receiver_shadow(world_pos, world_pos_dx, world_pos_dy, world_normal, source_pixel);
    }
    float key = occupancy_dims.w != 0.0f ? occlusion_cone(world_pos, world_normal)
                                         : key_shadow(world_pos, world_normal, shadow, source_pixel);
    if (visibility_params.x == 0.0f) return float4(1.0f, shadow, key, 1.0f);
    float3 view_normal;
    view_normal.x = dot(view_rot[0].xyz, world_normal);
    view_normal.y = dot(view_rot[1].xyz, world_normal);
    view_normal.z = dot(view_rot[2].xyz, world_normal);
    view_normal = normalize(view_normal);

    bool low = low_quality != 0.0f;
    float rotation_noise;
    float radius_noise;
    if (low) {
        /* Match the stochastic sequence to LOW's 5x5 resolve. Both linear
         * transforms are invertible modulo five, so every 5x5 neighbourhood
         * contains each of the 25 rotation and radius strata exactly once.
         * The bilateral resolve therefore cancels the pattern on continuous
         * surfaces instead of turning white-noise imbalance into broad
         * screen-fixed clumps. */
        uint2 tile = (uint2)i.position.xy % 5u;
        uint rotation_index =
            ((tile.x + 2u * tile.y) % 5u)
            + 5u * ((2u * tile.x + tile.y) % 5u);
        uint radius_index =
            ((2u * tile.x + tile.y) % 5u)
            + 5u * ((tile.x + tile.y) % 5u);
        rotation_noise = ((float)rotation_index + 0.5f) * (1.0f / 25.0f);
        radius_noise = ((float)radius_index + 0.5f) * (1.0f / 25.0f);
    } else {
        /* Preserve the approved high-quality distribution exactly. Its wider
         * separable denoiser removes this high-frequency IGN pattern. */
        rotation_noise = frac(52.9829189f *
                              frac(dot(i.position.xy,
                                       float2(0.06711056f, 0.00583715f))));
        uint jh = (uint)i.position.x * 3266489917u
                + (uint)i.position.y * 668265263u;
        jh ^= jh >> 15; jh *= 0x2c1b3c6du;
        jh ^= jh >> 12; jh *= 0x297a2d39u;
        jh ^= jh >> 15;
        radius_noise = (float)jh * (1.0f / 4294967296.0f);
    }
    float ang = rotation_noise * 6.2831853f;       /* 2π */
    float3 random_vec = float3(cos(ang), sin(ang), 0.0f);
    float3 T = normalize(random_vec - view_normal *
                                       dot(random_vec, view_normal));
    float3 B = cross(view_normal, T);

    int taps = low ? KERNEL_LOW_COUNT : KERNEL_COUNT;

    /* Screen-space radius clamp.
     *
     * The kernel radius is in WORLD/view units, so its projected screen
     * footprint is radius_view / (tan_h_half * view_z): it shrinks with
     * distance. Below ~1 texel every tap lands in the same depth texel, so
     * the discrete kernel/rotation aliases against the texel grid as a fixed
     * pattern (the grid on distant hulls). Above a few texels the field is
     * well sampled; too far above it, the finite tap set separates into
     * visibly offset occlusion copies on near surfaces.
     *
     * Clamp the EFFECTIVE radius so its projected footprint stays in a sane
     * band, distance-adaptively:
     *   - min_screen_frac raises the radius on distant surfaces (footprint
     *     floor) — this is what removes the grid. 0 = off.
     *   - max_screen_frac caps it on near surfaces (footprint ceiling) to
     *     keep the taps dense. 0 = off (near behaviour unchanged).
     * Fractions are of the NDC half-width; footprint_texels ~= frac * ao_w.
     * bias/range scale with the effective radius so the tuned ratio holds. */
    float r_eff = radius_view;
    if (min_screen_frac > 0.0f)
        r_eff = max(r_eff, min_screen_frac * tan_h_half * view_pos.z);
    if (max_screen_frac > 0.0f)
        r_eff = min(r_eff, max_screen_frac * tan_h_half * view_pos.z);

    /* Per-pixel radius jitter spreads each discrete tap into a radial band,
     * avoiding visibly separate offset occlusion copies on near surfaces.
     * LOW uses the matched radius stratum above; HIGH retains its approved
     * integer-mixed white-noise distribution. */
    r_eff *= 1.0f + sample_jitter * (radius_noise - 0.5f); /* [1-j/2, 1+j/2] */

    float bias_eff = bias_view * (r_eff / radius_view);

    float occlusion = 0.0f;
    if (low) {
        [loop] for (int k = 0; k < KERNEL_LOW_COUNT; k++) {
            occlusion += evaluate_kernel_sample(
                view_pos, T, B, view_normal, KERNEL_LOW[k], r_eff, bias_eff);
        }
    } else {
        [loop] for (int k = 0; k < KERNEL_COUNT; k++) {
            occlusion += evaluate_kernel_sample(
                view_pos, T, B, view_normal, KERNEL_HIGH[k], r_eff, bias_eff);
        }
    }

    float ao = 1.0f - (occlusion / (float)taps) * intensity;
    return saturate(float4(ao, shadow, key, 1.0f));
#endif
}
