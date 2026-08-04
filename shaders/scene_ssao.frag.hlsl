/*
 * Screen-space ambient occlusion — classic random-kernel algorithm,
 * normals sourced from the G-buffer written by the mesh FSes.
 *
 * Inputs:
 *   t0 — flight depth RT (D32_FLOAT, reverse-Z: 1 at near, 0 at far)
 *   t1 — flight normal RT (R16G16_SNORM, octahedral-encoded world
 *        normal — produced by mesh FSes in the geometry sub-pass)
 *   s0 — NEAREST sampler with CLAMP_TO_EDGE (shared)
 *   b0 — SSAOUniforms: scalar projection params + SSAO knobs +
 *        world→view rotation 3×3
 *
 * Output:
 *   RG8 visibility in [0, 1]. R is ambient occlusion; G is main
 *   directional-shadow visibility for opaque forward shading.
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
};

Texture2D    g_depth   : register(t0, space2);
/* Geometric world normal G-buffer (R16G16_SNORM, octahedral). Written by
 * the prepass from world-position derivatives — the true face normal. */
Texture2D    g_normal  : register(t1, space2);
SamplerState g_sampler : register(s0, space2);

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

    float actual = g_depth.SampleLevel(g_sampler, sample_uv, 0.0f).r;
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

float2 main(VSOut i) : SV_Target0
{
    /* A half-resolution pixel centre lies between full-resolution G-buffer
     * pixel centres. Select one concrete pixel from its 2x2 footprint and
     * reconstruct through that pixel's centre; combining a nearest depth
     * with the half-resolution UV creates a receiver that is not on the
     * sampled surface and can alias against the shadow-map texel lattice. */
    uint depth_w, depth_h;
    g_depth.GetDimensions(depth_w, depth_h);
    uint2 depth_size = uint2(depth_w, depth_h);
    uint2 visibility_pixel = uint2(i.position.xy);
    uint2 source_pixel = min(visibility_pixel * 2u + 1u, depth_size - 1u);
    float2 receiver_uv = (float2(source_pixel) + 0.5f) / float2(depth_size);

    float depth = g_depth.Load(int3(source_pixel, 0)).r;
    if (depth <= 0.0f) return float2(1.0f, 1.0f); /* sky never occludes */

    float3 view_pos = view_pos_from(receiver_uv, depth);

    /* Geometric world normal from the G-buffer (written by the prepass at
     * full res — the true face normal, free of the Gouraud tilt and of the
     * grid that per-pixel half-res reconstruction here produced). Decode
     * the octahedral encoding, then rotate world→view for the hemisphere.
     * The R16G16_SNORM samples in [-1, 1], the octahedral output range. */
    float2 enc = g_normal.Load(int3(source_pixel, 0)).rg;
    float3 world_normal = oct_decode(enc);
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

    /* Reconstruct the opaque surface represented by the prepass. Shadow
     * visibility is evaluated once per half-resolution pixel, then shares
     * AO's bilateral passes. Use the geometric G-buffer normal for bias;
     * direct-light orientation is applied later by the material shader. */
    float3 world_pos = world_pos_from_view(view_pos);
    float bias_ndotl = saturate(dot(world_normal, shadow_light_dir.xyz));
    uint  cascade;
    float cascade_blend;
    float shadow_coverage;
    float shadow = directional_shadow_visibility(
        world_pos, world_normal, 1.0f, bias_ndotl, ddx(world_pos), ddy(world_pos),
        float2(source_pixel) + 0.5f, cascade, cascade_blend, shadow_coverage);
    return saturate(float2(ao, shadow));
}
