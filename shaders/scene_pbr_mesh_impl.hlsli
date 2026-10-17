/*
 * Fragment shader for the glTF (PBR) mesh path.
 *
 * Shading model: Cook-Torrance specular + Lambert/HL2 ambient cube.
 * Per-channel atlas samples use fractional UVs and SampleGrad. Texture
 * maps follow the glTF spec:
 *   baseColor          (sRGB)   — sampled and tinted by base_color_factor.
 *   normal             (UNORM)  — tangent-space; sample RG (z = +1 derived).
 *   metallic-roughness (UNORM)  — B = metallic, G = roughness (glTF spec).
 *   emissive           (sRGB)   — tinted by emissive_factor and HDR-scaled
 *                                  by emissive_strength.
 *
 * Material resolution per fragment:
 *   1. Vertex carries prim_id (flat, no interpolation).
 *   2. The selected row of the mesh-owned variant map returns mat_idx
 *      (UINT32_MAX = no material → factor-only path).
 *   3. Mesh-owned g_materials[mat_idx] yields sub-rects + factors +
 *      flags.
 *
 * Fragment resource map:
 *   b0 space3 — DirectionalShadowFS (scene-owned)
 *   b1 space3 — PbrLightFS          (shared lighting and tuning)
 *   b2 space3 — scene lighting block (clustered lights + environment)
 *   t12 space2 — material storage (after twelve sampled textures)
 *   t13 space2 — packed variant-map storage
 *   t14 space2 — scene point-light storage
 *   t15 space2 — cluster headers
 *   t16 space2 — cluster light indices
 *   t17 space2 — mesh screen rectangles (scene_pbr_screen.hlsli)
 *   t18 space2 — scene screen frame
 *
 * Texture / sampler slots:
 *   t0/s0 — base_color atlas    (sRGB)
 *   t1/s1 — normal atlas        (UNORM)
 *   t2/s2 — metallic_rough atlas (UNORM)
 *   t3/s3 — emissive atlas      (sRGB)
 *   t4/s4 — SSAO
 *   t5/s5 — comparison shadow depth
 *   t6/s6 — raw shadow depth for PCSS blocker search
 *   t7/s7 — diffuse environment cube (Lambert irradiance / pi)
 *   t8/s8 — specular environment cube (GGX roughness mips)
 *   t9/s9 — split-sum BRDF table (N.V, perceptual roughness) -> (A, B)
 *   t10/s10 — screen capture, graphics (premultiplied, sRGB)
 *   t11/s11 — screen capture, text (premultiplied, sRGB)
 */

/* Variants: AERON_PBR_ANIM 1 adds material animation (material_anim.hlsli):
 * the effect atlas at t12/t13 and the animation records in storage, which
 * moves every storage register up by two. AERON_PBR_SCREENS 0 drops the screen code, which the renderer
 * draws with its own pipeline (a mesh's screen primitives). AERON_PBR_ALPHA_BLEND
 * 1 is the variant for blended materials. AERON_PBR_ENVIRONMENT 0 drops the
 * image-based lighting, for scenes without an environment. AERON_PBR_PROBES 1
 * (screenless) adds the drawn object's probe set: reflection probes and an
 * irradiance grid in place of the screen textures (t10, t11) and its set
 * buffer at storage slot 4. AERON_PBR_MIRROR 1 (with the probes) shades the
 * floor's mirrored view (planar_reflection.c), which the floor blurs: no
 * visibility pass, floor reflection or point lights, and the probe light
 * the surface takes in place of the probes' reflection. All keep this
 * shader's cost for ordinary materials down: code chosen by a uniform costs
 * time even where it does not run. */
#ifndef AERON_PBR_SCREENS
#define AERON_PBR_SCREENS 1
#endif
#ifndef AERON_PBR_ENVIRONMENT
#define AERON_PBR_ENVIRONMENT 1
#endif
#ifndef AERON_PBR_ALPHA_BLEND
#define AERON_PBR_ALPHA_BLEND 0
#endif
#ifndef AERON_PBR_ANIM
#define AERON_PBR_ANIM 0
#endif
#ifndef AERON_PBR_PROBES
#define AERON_PBR_PROBES 0
#endif
#ifndef AERON_PBR_MIRROR
#define AERON_PBR_MIRROR 0
#endif

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
/* Samplers first, then storage buffers: the probe variants' occlusion grid
 * and floor reflection follow the material (and `_anim` effect) textures
 * and move the buffers up two. */
#if AERON_PBR_ANIM && AERON_PBR_PROBES
#define AERON_PBR_OCCLUSION_TEXTURE_REGISTER t14
#define AERON_PBR_OCCLUSION_SAMPLER_REGISTER s14
#define AERON_PBR_PLANAR_TEXTURE_REGISTER t15
#define AERON_PBR_PLANAR_SAMPLER_REGISTER s15
#define AERON_PBR_MATERIAL_REGISTER t16
#define AERON_PBR_VARIANT_REGISTER t17
#define AERON_PBR_POINT_LIGHT_REGISTER t18
#define AERON_CLUSTER_DATA_REGISTER t19
#define AERON_PBR_PROBE_SET_REGISTER t20
#define AERON_PBR_SCREEN_RECT_REGISTER t21
#define AERON_PBR_SCREEN_FRAME_REGISTER t22
#define AERON_PBR_ANIM_RECORD_REGISTER t23
#elif AERON_PBR_ANIM
#define AERON_PBR_MATERIAL_REGISTER t14
#define AERON_PBR_VARIANT_REGISTER t15
#define AERON_PBR_POINT_LIGHT_REGISTER t16
#define AERON_CLUSTER_DATA_REGISTER t17
#define AERON_PBR_PROBE_SET_REGISTER t18
#define AERON_PBR_SCREEN_RECT_REGISTER t19
#define AERON_PBR_SCREEN_FRAME_REGISTER t20
#define AERON_PBR_ANIM_RECORD_REGISTER t21
#elif AERON_PBR_PROBES
#define AERON_PBR_OCCLUSION_TEXTURE_REGISTER t12
#define AERON_PBR_OCCLUSION_SAMPLER_REGISTER s12
#define AERON_PBR_PLANAR_TEXTURE_REGISTER t13
#define AERON_PBR_PLANAR_SAMPLER_REGISTER s13
#define AERON_PBR_MATERIAL_REGISTER t14
#define AERON_PBR_VARIANT_REGISTER t15
#define AERON_PBR_POINT_LIGHT_REGISTER t16
#define AERON_CLUSTER_DATA_REGISTER t17
#define AERON_PBR_PROBE_SET_REGISTER t18
#define AERON_PBR_SCREEN_RECT_REGISTER t19
#define AERON_PBR_SCREEN_FRAME_REGISTER t20
#else
#define AERON_PBR_MATERIAL_REGISTER t12
#define AERON_PBR_VARIANT_REGISTER t13
#define AERON_PBR_POINT_LIGHT_REGISTER t14
#define AERON_CLUSTER_DATA_REGISTER t15
#define AERON_PBR_PROBE_SET_REGISTER t16
#define AERON_PBR_SCREEN_RECT_REGISTER t17
#define AERON_PBR_SCREEN_FRAME_REGISTER t18
#endif
#include "scene_pbr_lighting.hlsli"
#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "srgb.hlsli"
#include "scene_pbr_material_alpha.hlsli"
#if !AERON_PBR_PROBES
#include "scene_pbr_screen.hlsli"
#endif

Texture2DArray g_normal     : register(t1, space2);
SamplerState g_normal_sampler : register(s1, space2);
Texture2DArray g_mr         : register(t2, space2);
SamplerState g_mr_sampler   : register(s2, space2);
Texture2DArray g_emissive   : register(t3, space2);
SamplerState g_em_sampler   : register(s3, space2);
/* Half-res RGBA8 visibility: R = SSAO, G = denoised main directional
 * shadow, B = the key light's shadow 1 m out (scene_ssao.hlsli). A 1×1
 * white placeholder keeps all unoccluded when the joint visibility pass is
 * unavailable. */
Texture2D<float4> g_ao      : register(t4, space2);
SamplerState g_ao_sampler   : register(s4, space2);
TextureCube<float4> g_environment : register(t7, space2);
SamplerState g_environment_sampler : register(s7, space2);
TextureCube<float4> g_specular_environment : register(t8, space2);
SamplerState g_specular_environment_sampler : register(s8, space2);
Texture2D<float2> g_brdf_lut : register(t9, space2);
SamplerState g_brdf_lut_sampler : register(s9, space2);

#if AERON_PBR_ANIM
/* Effect atlas (layer, emissive) and the mesh's animation records. */
Texture2DArray g_effect_layer : register(t12, space2);
SamplerState g_effect_layer_sampler : register(s12, space2);
Texture2DArray g_effect_emissive : register(t13, space2);
SamplerState g_effect_emissive_sampler : register(s13, space2);
StructuredBuffer<uint4> g_anim_records : register(AERON_PBR_ANIM_RECORD_REGISTER, space2);
/* The instance's animation state (AeronScenePbrAnimBlock). */
cbuffer PbrAnimFS : register(b3, space3)
{
    float4 fs_anim;          /* x: animation time (seconds), y: effect page size (texels) */
    uint4  fs_anim_state;    /* x: state mask, y: seed, z: damage stage, w: energy */
    float4 fs_anim_energy;   /* x: energy start (seconds), yzw: unused */
    float4 fs_anim_onset[8]; /* state onsets (seconds), 4 per float4 */
};
#define AERON_ANIM_RECORD(i) g_anim_records[i]
#include "material_anim.hlsli"

/* Samples the image of a frame record. */
float4 anim_frame_sample(Texture2DArray tex, SamplerState s, uint4 frame, float2 uv)
{
    float4 rect = float4(frame.y & 0xFFFFu, frame.y >> 16, frame.z & 0xFFFFu, frame.z >> 16) / fs_anim.y;
    return atlas_sample(tex, s, uv, rect, frame.x & 0xFFFu, (frame.x >> 12) & 0xFu);
}

/* Samples the image of a frame record at `uv` in [0, 1] with explicit
 * gradients, limited to the image's mips as atlas_sample does. */
float4 anim_frame_sample_grad(Texture2DArray tex, SamplerState s, uint4 frame, float2 uv, float2 dx, float2 dy)
{
    float4 rect = float4(frame.y & 0xFFFFu, frame.y >> 16, frame.z & 0xFFFFu, frame.z >> 16) / fs_anim.y;
    uint width, height, layers, levels;
    tex.GetDimensions(0, width, height, layers, levels);
    float2 size = float2(width, height);
    dx *= rect.zw;
    dy *= rect.zw;
    float2 tdx    = dx * size;
    float2 tdy    = dy * size;
    float  lod    = 0.5f * log2(max(max(dot(tdx, tdx), dot(tdy, tdy)), 1e-20f));
    float  shrink = exp2(min((float)((frame.x >> 12) & 0xFu) - lod, 0.0f));
    return tex.SampleGrad(s, float3(rect.xy + uv * rect.zw, (float)(frame.x & 0xFFFu)), dx * shrink, dy * shrink);
}

/* The hull damage at `uv` for the instance's stage: rgb multiplies the
 * colour, a the gloss (roughness, metallic and specular); 1 outside the
 * damage rectangle and without damage. The rectangle applies where the
 * wrapped UV falls inside it, sampling the stage image across it. */
float4 anim_damage(uint4 anim, float2 uv)
{
    float2 uv_dx = ddx(uv);
    float2 uv_dy = ddy(uv);
    if (fs_anim_state.z == 0u || anim.z == 0u) return float4(1.0f, 1.0f, 1.0f, 1.0f);
    uint alternative = material_anim_damage_alternative(anim.z & 0xFFFFu, anim.z >> 16, fs_anim_state.y,
                                                        (anim.w >> 8) & 0xFFFu);
    uint4 frame = g_anim_records[alternative + min(fs_anim_state.z, AERON_ANIM_DAMAGE_STAGES) - 1u];
    if ((frame.x & 0xFFFu) == AERON_ANIM_NO_LAYER) return float4(1.0f, 1.0f, 1.0f, 1.0f);
    MaterialAnimRect r = material_anim_damage_rect(alternative, fs_anim_state.y, (anim.w >> 20) & 0xFFFu);
    float2 t = frac(1.0f + frac(uv));
    if (t.x < r.x0 || t.x > r.x1 || t.y < r.y0 || t.y > r.y1) return float4(1.0f, 1.0f, 1.0f, 1.0f);
    float2 range = float2(r.x1 - r.x0, r.y1 - r.y0);
    return anim_frame_sample_grad(g_effect_layer, g_effect_layer_sampler, frame,
                                  saturate((t - float2(r.x0, r.y0)) / range), uv_dx / range, uv_dy / range);
}

/* The instance's energy effect (fs_anim_state.w: 1 shields down, 2
 * tractor, 3 jamming) as emission at the model-frame position: its frame,
 * started at fs_anim_energy.x, projected on the three model planes and
 * blended by normalize(|n|^4), tiled by the effect's size, the projection
 * turned and offset at random per start. Zero when the material does not
 * receive that effect. */
float3 anim_energy(uint4 anim, float3 model_pos, float3 model_normal)
{
    float3 pos_dx = ddx(model_pos);
    float3 pos_dy = ddy(model_pos);
    uint   energy = fs_anim_state.w;
    uint   first  = g_anim_records[0].x;
    if (energy == 0u || energy > 3u || first == 0u || ((anim.w >> (energy - 1u)) & 1u) == 0u)
        return float3(0.0f, 0.0f, 0.0f);
    uint book  = first + (energy - 1u) * 2u;
    uint frame = material_anim_frame(book, fs_anim.x - fs_anim_energy.x);
    if (frame == 0u || (g_anim_records[frame].x & 0xFFFu) == AERON_ANIM_NO_LAYER)
        return float3(0.0f, 0.0f, 0.0f);
    float strength = asfloat(g_anim_records[book + 1u].x);
    float inv_tile = 1.0f / max(asfloat(g_anim_records[book + 1u].y), 1e-4f);
    uint  h        = material_anim_hash(fs_anim_state.y, asuint(fs_anim_energy.x));
    float angle    = material_anim_unorm(h) * 6.28318531f;
    float3 offset  = float3(material_anim_unorm(material_anim_hash(h, 1u)),
                            material_anim_unorm(material_anim_hash(h, 2u)),
                            material_anim_unorm(material_anim_hash(h, 3u)));
    float s, c;
    sincos(angle, s, c);
    /* A turn about the model's up axis, the same for the normal. */
    float3x3 turn = float3x3(c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f);
    float3 p  = mul(turn, model_pos) * inv_tile + offset;
    float3 dx = mul(turn, pos_dx) * inv_tile;
    float3 dy = mul(turn, pos_dy) * inv_tile;
    float3 w  = pow(abs(mul(turn, model_normal)), 4.0f);
    w /= max(w.x + w.y + w.z, 1e-6f);
    uint4 f = g_anim_records[frame];
    float3 e = w.x * anim_frame_sample_grad(g_effect_emissive, g_effect_emissive_sampler, f, frac(p.yz), dx.yz, dy.yz).rgb +
               w.y * anim_frame_sample_grad(g_effect_emissive, g_effect_emissive_sampler, f, frac(p.xz), dx.xz, dy.xz).rgb +
               w.z * anim_frame_sample_grad(g_effect_emissive, g_effect_emissive_sampler, f, frac(p.xy), dx.xy, dy.xy).rgb;
    return e * strength;
}

/* The playing frame record of a layer's flipbook (0: none, or a frame
 * missing from its library), and the flipbook record: the first one whose
 * state is active, timed from when its state last became set. */
uint anim_layer_frame(uint4 anim, uint layer, out uint flipbook)
{
    flipbook = material_anim_select(anim.y & 0xFFFFu, anim.y >> 16, layer, fs_anim_state.x);
    uint state = g_anim_records[flipbook].x & 0x1Fu;
    float onset = state == AERON_ANIM_STATE_ALWAYS ? 0.0f : fs_anim_onset[state >> 2u][state & 3u];
    uint frame = flipbook != 0u ? material_anim_frame(flipbook, fs_anim.x - onset) : 0u;
    return frame != 0u && (g_anim_records[frame].x & 0xFFFu) != AERON_ANIM_NO_LAYER ? frame : 0u;
}
#endif


/* Where to read prefiltered radiance for a surface of `roughness`: the
 * mirror direction R pulled toward the normal as roughness grows, since most
 * of a rough surface's reflected light comes from around its normal
 * (Lagarde and de Rousiers 2014, "Moving Frostbite to PBR"). */
float3 specular_dominant_direction(float3 N, float3 R, float roughness)
{
    float a = roughness * roughness;
    return lerp(N, R, (1.0f - a) * (sqrt(1.0f - a) + a));
}

#if AERON_PBR_PROBES
/* ===== Probe set ======================================================
 * The object's reflection probes (a cube array, probe k in layer k), its
 * irradiance grid (3D, slab f holds the light toward +x, -x, +y, -y, +z,
 * -z for f = 0 to 5) and its set buffer: header, probe records, cell words
 * (AeronEnvironmentPackage in environment.h). Probes and grid live in the
 * model frame (metres); a cube direction is (d.x, d.z, d.y) of a model
 * direction. */
TextureCubeArray<float4> g_probe_cubes : register(t10, space2);
SamplerState g_probe_cubes_sampler : register(s10, space2);
Texture3D<float4> g_probe_grid : register(t11, space2);
SamplerState g_probe_grid_sampler : register(s11, space2);
StructuredBuffer<uint4> g_probe_set : register(AERON_PBR_PROBE_SET_REGISTER, space2);
/* The probe occlusion grid (probe_occlusion.c): per cell, the share of the
 * probe irradiance past the occluders on the faces toward +x, -x, +y, -y,
 * then in the right half toward +z, -z. */
Texture3D<float4> g_probe_occlusion : register(AERON_PBR_OCCLUSION_TEXTURE_REGISTER, space2);
SamplerState g_probe_occlusion_sampler : register(AERON_PBR_OCCLUSION_SAMPLER_REGISTER, space2);
/* The floor's reflection (planar_reflection.c) at half resolution, with
 * mips: the mirrored view's colour times its coverage and the coverage on
 * the left; the reflected point's height above the floor in metres times
 * the coverage, and the coverage, on the right. */
Texture2D<float4> g_planar_reflection : register(AERON_PBR_PLANAR_TEXTURE_REGISTER, space2);
SamplerState g_planar_reflection_sampler : register(AERON_PBR_PLANAR_SAMPLER_REGISTER, space2);

/* Level `level` of the floor's reflection around `t` (first-level texels of
 * its left half; plus `offset` for the right), as a cubic B-spline from
 * four bilinear taps, so a coarse level magnified stays smooth; x stays
 * inside the half. */
float4 planar_bspline(float2 t, float level, float offset, float2 dims)
{
    float  scale = exp2(level);
    float2 p     = t / scale - 0.5f;
    float2 i     = floor(p);
    float2 f     = p - i;
    float2 w0    = (1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f;
    float2 w1    = (4.0f - 6.0f * f * f + 3.0f * f * f * f) / 6.0f;
    float2 w3    = f * f * f / 6.0f;
    float2 g0    = w0 + w1;
    float2 g1    = 1.0f - g0;
    float2 h0    = (i - 0.5f + w1 / g0) * scale;
    float2 h1    = (i + 1.5f + w3 / g1) * scale;
    float  lo    = 0.5f * scale;
    float  hi    = 0.5f * dims.x - 0.5f * scale;
    h0.x         = clamp(h0.x, lo, hi) + offset;
    h1.x         = clamp(h1.x, lo, hi) + offset;
    return g0.y * (g0.x * g_planar_reflection.SampleLevel(g_planar_reflection_sampler, h0 / dims, level) +
                   g1.x * g_planar_reflection.SampleLevel(g_planar_reflection_sampler, float2(h1.x, h0.y) / dims,
                                                          level)) +
           g1.y * (g0.x * g_planar_reflection.SampleLevel(g_planar_reflection_sampler, float2(h0.x, h1.y) / dims,
                                                          level) +
                   g1.x * g_planar_reflection.SampleLevel(g_planar_reflection_sampler, h1 / dims, level));
}

/* What the floor at model point `pm` (geometric model normal `nm`, `view`
 * metres from the eye) reflects of what stands on it, at screen position
 * `pixel`: radiance times coverage, and the coverage. Nothing off the
 * floor. The level widens with the GGX lobe (about 1.3 alpha either side of
 * the mirror direction) as seen from the eye: alpha times the reflected
 * point's height over the length of the path to it. */
float4 planar_reflection(float2 pixel, float3 pm, float3 nm, float view, float roughness)
{
    if (fs_planar_params.z == 0.0f || nm.z < 0.9f || abs(pm.z - fs_planar_params.x) > fs_planar_params.y ||
        any(abs(pm.xy - fs_planar_rect.xy) > fs_planar_rect.zw))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 t = pixel * 0.5f; /* half-resolution texels */
    if (any(t < fs_planar_screen.xy) || any(t > fs_planar_screen.zw))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float width, height, levels;
    g_planar_reflection.GetDimensions(0, width, height, levels);
    float2 dims       = float2(width, height);
    float  half_width = 0.5f * width;
    /* The mean height of what the pixel reflects: smooth, a coarse tap.
     * What it leaves uncovered takes the coarsest level's, which reaches as
     * far as the widest blur, so the blur fades out instead of being cut. */
    float2 h = g_planar_reflection.SampleLevel(g_planar_reflection_sampler,
                                               float2(clamp(t.x, 1.0f, half_width - 1.0f) + half_width, t.y) / dims,
                                               1.0f).rg;
    float2 wide = planar_bspline(t, fs_planar_params.w - 1.0f, half_width, dims).rg;
    if (wide.y <= 1.0e-4f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float rise        = h.x + (1.0f - h.y) * wide.x / wide.y;
    float alpha       = roughness * roughness;
    float angle       = 1.3f * alpha * rise / max(view + rise, 1.0e-3f);
    float texel_angle = 4.0f * fs_cluster_tan_v_half / fs_render_size.y;
    float lod         = clamp(log2(max(2.0f * angle / texel_angle, 1.0f)), 0.0f, fs_planar_params.w - 1.0f);
    /* The first level as it is, sharp; the coarser ones smooth. */
    float  level = floor(lod);
    float4 a     = level < 0.5f ? g_planar_reflection.SampleLevel(g_planar_reflection_sampler,
                                                                   float2(clamp(t.x, 0.5f, half_width - 0.5f), t.y) / dims, 0.0f)
                                : planar_bspline(t, level, 0.0f, dims);
    if (lod - level <= 0.0f)
        return a;
    return lerp(a, planar_bspline(t, level + 1.0f, 0.0f, dims), lod - level);
}

/* The share of the probe irradiance for model normal `nm` that reaches model
 * point `pm` past the occluders: the grid's faces toward the normal,
 * weighted by its squared components, one cell out along the normal, where
 * the cells the filter blends lie outside the occluder the surface belongs
 * to. */
float probe_occlusion_diffuse(float3 pm, float3 nm)
{
    float3 dims = fs_occlusion_dims.xyz;
    float3 t    = clamp((pm - fs_occlusion_origin.xyz) / fs_occlusion_dims.w + nm, 0.5f, dims - 0.5f);
    float3 uvw  = float3(t.x / (2.0f * dims.x), t.y / dims.y, t.z / dims.z);
    float4 xy   = g_probe_occlusion.SampleLevel(g_probe_occlusion_sampler, uvw, 0.0f);
    float2 z    = g_probe_occlusion.SampleLevel(g_probe_occlusion_sampler, uvw + float3(0.5f, 0.0f, 0.0f), 0.0f).xy;
    float3 w    = nm * nm;
    return saturate(w.x * (nm.x >= 0.0f ? xy.x : xy.y) + w.y * (nm.y >= 0.0f ? xy.z : xy.w) +
                    w.z * (nm.z >= 0.0f ? z.x : z.y));
}

float3 probe_model_point(float3 world_pos)
{
    float4 p = float4(world_pos, 1.0f);
    return float3(dot(fs_world_to_model[0], p), dot(fs_world_to_model[1], p), dot(fs_world_to_model[2], p));
}

float3 probe_model_direction(float3 world_direction)
{
    return normalize(float3(dot(fs_world_to_model[0].xyz, world_direction),
                            dot(fs_world_to_model[1].xyz, world_direction),
                            dot(fs_world_to_model[2].xyz, world_direction)));
}

/* The grid at model point `pm`: its texture coordinates within a slab and
 * how much it applies, 1 inside the grid's box, falling to 0 one point
 * spacing outside it (the outer points' light holds up to the box). */
float probe_grid_point(float3 pm, out float2 uv, out float z)
{
    float3 r = float3(fs_probe_params.zw, fs_grid_scale.w);
    float3 g = pm * fs_grid_scale.xyz + fs_grid_offset.xyz;
    float3 w = saturate(min(g + 1.5f, r + 1.5f - g));
    uv       = g.xy / r.xy;
    z        = clamp(g.z, 0.5f, r.z - 0.5f);
    return w.x * w.y * w.z;
}

/* The grid's irradiance / pi for the model normal `nm` at (uv, z), from the
 * faces toward it weighted by its squared components; in `mean`, the
 * luminance of the mean radiance. Within a slab: filtering never mixes
 * faces. */
float3 probe_grid_light(float2 uv, float z, float3 nm, out float mean)
{
    float  rz     = fs_grid_scale.w;
    float3 slab   = select(nm >= 0.0f, float3(0.0f, 2.0f, 4.0f), float3(1.0f, 3.0f, 5.0f));
    float3 w      = nm * nm;
    float4 face_x = g_probe_grid.SampleLevel(g_probe_grid_sampler, float3(uv, (slab.x * rz + z) / (6.0f * rz)), 0.0f);
    float4 face_y = g_probe_grid.SampleLevel(g_probe_grid_sampler, float3(uv, (slab.y * rz + z) / (6.0f * rz)), 0.0f);
    float4 face_z = g_probe_grid.SampleLevel(g_probe_grid_sampler, float3(uv, (slab.z * rz + z) / (6.0f * rz)), 0.0f);
    mean          = face_x.a;
    return w.x * face_x.rgb + w.y * face_y.rgb + w.z * face_z.rgb;
}

/* Irradiance / pi from the grid at model point `pm` for the model normal
 * `nm`, and in `weight` how much it applies. Also gives the luminance the
 * probes' reflections are scaled by (probe_specular), and in `light` the
 * luminance of the irradiance alone. */
float3 probe_grid_diffuse(float3 pm, float3 nm, out float weight, out float luminance, out float light)
{
    luminance = 0.0f;
    light     = 0.0f;
    float2 uv;
    float  z;
    weight = probe_grid_point(pm, uv, z);
    if (weight <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float  mean;
    float3 e = max(probe_grid_light(uv, z, nm, mean), 0.0f);
    light    = dot(e, float3(0.2126f, 0.7152f, 0.0722f));
    /* The light toward the normal plus the mean light: steadier than the
     * first alone where both are small. The probe records hold the same at
     * the probes' centres. */
    luminance = light + max(mean, 0.0f);
    return e * fs_probe_strengths.x;
}

/* The luminance of the grid's irradiance / pi at model point `pm` for the
 * model normal `nm`. */
float probe_grid_luminance(float3 pm, float3 nm)
{
    float2 uv;
    float  z, mean;
    if (probe_grid_point(pm, uv, z) <= 0.0f) return 0.0f;
    return dot(max(probe_grid_light(uv, z, nm, mean), 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

/* The far hit of the ray o + t d (o inside) with a box or ellipsoid of
 * half extent `half` around the origin; a point far along d when it misses,
 * which keeps the direction d. */
float3 probe_parallax(float3 o, float3 d, float3 half, bool ellipsoid)
{
    if (ellipsoid) {
        float3 os = o / half, ds = d / half;
        float  a = dot(ds, ds), b = dot(os, ds), c = dot(os, os) - 1.0f;
        float  disc = b * b - a * c;
        if (disc < 0.0f) return o + d * 1.0e4f;
        return o + d * ((-b + sqrt(disc)) / a);
    }
    float3 t = max((half - o) / d, (-half - o) / d);
    return o + d * min(min(t.x, t.y), t.z);
}

/* Prefiltered radiance from the probes of the cell holding `pm`, smallest
 * first, each taking the coverage the smaller ones left; returns the
 * coverage, which the environment's cube completes. A probe's reflection
 * darkens where the grid's light (`grid_luminance` for the model normal
 * `nm`) is darker than at the probe's centre, where it was captured: a
 * probe sees the room from the middle, not from a dark corner. */
float probe_specular(float3 pm, float3 rm, float roughness, float3 nm, float grid_luminance,
                     float grid_weight, out float3 radiance)
{
    radiance = float3(0.0f, 0.0f, 0.0f);
    uint4  dims = g_probe_set[1];
    float4 head = asfloat(g_probe_set[0]);
    int3   cell = (int3)floor((pm - head.xyz) * head.w);
    if (any(cell < 0) || any(cell >= (int3)dims.xyz)) return 0.0f;
    uint index = ((uint)cell.z * dims.y + (uint)cell.y) * dims.x + (uint)cell.x;
    uint word  = g_probe_set[2u + 6u * dims.w + (index >> 2u)][index & 3u];
    float3 w_nm = nm * nm;
    float coverage = 0.0f;
    float lod      = roughness * (fs_probe_params.y - 1.0f);
    [loop] for (uint k = 0u; k < 4u && coverage < 1.0f; ++k) {
        uint probe = (word >> (8u * k)) & 0xFFu;
        if (probe == 0u) break;
        uint   base     = 2u + 6u * (probe - 1u);
        float4 center   = asfloat(g_probe_set[base]);
        uint4  shape    = g_probe_set[base + 1u];
        uint4  parallax = g_probe_set[base + 2u];
        float3 room     = asfloat(g_probe_set[base + 3u].xyz);
        float3 l = (pm - center.xyz) * asfloat(shape.xyz);
        float  g = (parallax.w & 1u) != 0u ? length(l) : max(max(abs(l.x), abs(l.y)), abs(l.z));
        float  w = saturate((1.0f - g) * center.w);
        w = w * w * (3.0f - 2.0f * w);  /* no kink where probes meet */
        float  share = max(w - coverage, 0.0f);
        if (share <= 0.0f) continue;
        /* The ray from the point to the parallax volume, seen from where
         * the cube was captured. */
        float3 d = probe_parallax(pm - room, rm, asfloat(parallax.xyz), (parallax.w & 2u) != 0u) + room -
                   center.xyz;
        float4 capture  = asfloat(g_probe_set[base + 4u]); /* +x, -x, +y, -y */
        float4 vertical = asfloat(g_probe_set[base + 5u]); /* +z, -z, the mean */
        float  captured = w_nm.x * (nm.x >= 0.0f ? capture.x : capture.y) +
                         w_nm.y * (nm.y >= 0.0f ? capture.z : capture.w) +
                         w_nm.z * (nm.z >= 0.0f ? vertical.x : vertical.y) + max(vertical.z, 0.0f);
        float  scale    = captured > 1.0e-5f ? lerp(1.0f, saturate(grid_luminance / captured), grid_weight) : 1.0f;
        radiance += share * scale * g_probe_cubes.SampleLevel(g_probe_cubes_sampler,
                                                      float4(d.x, d.z, d.y, (float)shape.w), lod).rgb;
        coverage += share;
    }
    radiance = max(radiance, 0.0f) * fs_probe_strengths.y;
    return coverage;
}
#endif

float3 environment_local(float3 world_direction)
{
	return float3(dot(world_direction, fs_environment_basis[0].xyz),
				  dot(world_direction, fs_environment_basis[1].xyz),
				  dot(world_direction, fs_environment_basis[2].xyz));
}

float3 detailed_environment(float3 world_direction)
{
	if (fs_environment_params.x <= 0.0f) return 0.0f;
	return max(g_environment.SampleLevel(g_environment_sampler, normalize(environment_local(world_direction)),
										 0.0f).rgb,
			   0.0f) * fs_environment_params.x;
}


struct FSOut
{
    float4 color : SV_Target0;
};

FSOut main(PbrForwardVSOut i, bool is_front : SV_IsFrontFace)
{
    FSOut _out;

    /* ===== Material resolution =================================== */
    /* Fields are read where used (g_materials[mat]): keeping the whole
     * record live cost registers and spills across the shader. */
    const uint mat = pbr_material_slot(
        i.prim_id, i.variant_row_base, i.variant_group_count,
        i.material_count);

#if AERON_PBR_SCREENS
    /* ===== Hologram screens (unlit, straight alpha) ===============
     * Hidden screens return transparent black instead of discarding: they
     * are always blended (the loader keeps them in the blend range), where
     * it leaves the target unchanged, and a discard anywhere in this shader
     * would cost every opaque draw its early depth and hidden-surface
     * removal. */
    if ((g_materials[mat].screen_flags & SCREEN_HOLOGRAM) != 0u) {
        ScreenFrame holo_frame = screen_load_frame();
        if (holo_frame.holo_fade <= 0.01f) {
            _out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return _out;
        }
        _out.color = screen_hologram(g_materials[mat], i.uv, holo_frame);
        _out.color.rgb *= fs_emission.x;
        return _out;
    }
#endif

    /* ===== Base color ============================================ */
    float4 albedo_tex = pbr_sample_base_color(g_materials[mat], i.uv);
#if AERON_PBR_ALPHA_MASK
    pbr_apply_alpha_mask(albedo_tex.a * g_materials[mat].base_color_factor.a,
                         g_materials[mat].metal_rough.z);
#endif
#if AERON_PBR_ANIM
    /* ===== Flipbooks and hull damage ==============================
     * colour: the frame over the colour; damage: its factor multiplies
     * the result; illumination: base = c·k + layer_rgb over that colour c,
     * and its emission replaces the material's. Without
     * an illumination frame, a colour flipbook's unlit weight u splits the
     * result as the static bake does: base c·(1 − u), emission c·u in place
     * of the material's. A layer without a playing frame keeps the static
     * material. A colour frame also covers a screen's display (below). */
    bool   anim_emission = false;
    bool   anim_base = false; /* albedo below is the composite, factor included */
    bool   anim_screen_frame = false;
    float  anim_screen_cover = 0.0f;
    float3 anim_emissive = float3(0.0f, 0.0f, 0.0f);
    float  anim_gloss    = 1.0f;
    uint4  anim = g_materials[mat].anim;
    float3 anim_energy_rgb = anim_energy(anim, i.model_pos, i.model_normal);
    if (anim.y != 0u || anim.z != 0u) {
        uint colour_book = 0u, lit_book = 0u;
        uint colour_frame = anim.y != 0u ? anim_layer_frame(anim, AERON_ANIM_LAYER_COLOUR, colour_book) : 0u;
        uint lit_frame    = anim.y != 0u ? anim_layer_frame(anim, AERON_ANIM_LAYER_ILLUMINATION, lit_book) : 0u;
        float4 damage     = anim_damage(anim, i.uv);
        anim_gloss        = damage.a;
        /* Frames paint over the base colour, its factor included. */
        float3 c = albedo_tex.rgb * g_materials[mat].base_color_factor.rgb;
        if (colour_frame != 0u) {
            float4 frame = anim_frame_sample(g_effect_layer, g_effect_layer_sampler, g_anim_records[colour_frame],
                                             i.uv);
            c = lerp(c, frame.rgb, frame.a);
            anim_screen_frame = true;
            anim_screen_cover = frame.a;
        }
        /* Damage darkens the colour under the illumination layer. */
        c *= damage.rgb;
        float unlit = colour_frame != 0u
            ? float((g_anim_records[colour_book].x >> AERON_ANIM_FLIPBOOK_UNLIT_SHIFT) & 0xFFu) * (1.0f / 255.0f)
            : 0.0f;
        if (lit_frame == 0u && unlit > 0.0f) {
            anim_emissive = c * unlit;
            anim_emission = true;
            c *= 1.0f - unlit;
        }
        if (lit_frame != 0u) {
            float4 layer = anim_frame_sample(g_effect_layer, g_effect_layer_sampler, g_anim_records[lit_frame],
                                             i.uv);
            c = c * layer.a + layer.rgb;
            uint4 lit_emissive = g_anim_records[lit_frame + 1u];
            anim_emissive = anim_frame_sample(g_effect_emissive, g_effect_emissive_sampler, lit_emissive,
                                              i.uv).rgb * asfloat(lit_emissive.w);
            anim_emission = true;
        }
        albedo_tex.rgb = c;
        anim_base      = true;
    }
#endif
    float3 albedo = albedo_tex.rgb;
#if AERON_PBR_ANIM
    if (!anim_base)
#endif
        albedo *= g_materials[mat].base_color_factor.rgb;

#if AERON_PBR_SCREENS
    /* ===== Transparent screens ===================================
     * Unlit (no normal or shading): the cover, when present,
     * over the screen, with alpha from the display luminance. A playing
     * colour frame covers the display as on opaque screens (below); its
     * unlit part is in its emission. */
    if ((g_materials[mat].screen_flags & SCREEN_TRANSPARENT) != 0u) {
        ScreenFrame screen_frame = screen_load_frame();
        if ((screen_frame.flags & SCREEN_FRAME_HIDE_TRANSPARENT) != 0u &&
            (g_materials[mat].screen_flags & SCREEN_ALWAYS_VISIBLE) == 0u) {
            _out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return _out;
        }
        float3 screen  = screen_evaluate(g_materials[mat], i.uv, screen_frame);
        bool   cover   = (g_materials[mat].screen_flags & SCREEN_COVER) != 0u;
        bool   surface = cover; /* the cover, or a frame in its place */
        float  open    = cover ? 1.0f - albedo_tex.a : 1.0f;
        float3 glow    = float3(0.0f, 0.0f, 0.0f);
        if (cover && (g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
            glow = pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat], GLTF_ATLAS_EMISSIVE,
                                      g_materials[mat].emissive_rect, i.uv).rgb *
                   g_materials[mat].emissive_packed.rgb * g_materials[mat].emissive_packed.a;
        }
#if AERON_PBR_ANIM
        if (anim_screen_frame) {
            surface = true;
            open    = cover ? open * (1.0f - anim_screen_cover) : 0.0f;
            if (anim_emission)
                glow = anim_emissive;
        }
#endif
        float3 color = (surface ? albedo + glow : float3(0.0f, 0.0f, 0.0f)) + screen * open;
        /* Alpha from the unscaled display, so the scale changes brightness
         * only. */
        float luma = dot(AeronLinearToSrgb(saturate(color)), float3(0.333f, 0.333f, 0.333f));
        _out.color = float4(color * fs_emission.x, saturate(4.0f * luma));
        return _out;
    }
#endif

    /* ===== Normal vector ========================================= */
    float  side_sign = is_front ? 1.0f : -1.0f;
    float3 N_geom = normalize(i.world_normal) * side_sign;
    float3 world_pos_dx = ddx(i.world_pos);
    float3 world_pos_dy = ddy(i.world_pos);
    float3 N_face = N_geom;
#if AERON_PBR_DEBUG_VIEWS
    if ((i.receive_shadow != 0u && i.screen_shadow == 0u) ||
        spec_geom_adapt != 0.0f) {
#endif
        float3 face_cross = cross(world_pos_dx, world_pos_dy);
        float face_length_sq = dot(face_cross, face_cross);
        if (face_length_sq > 1.0e-12f) {
            N_face = face_cross * rsqrt(face_length_sq);
            N_face = (dot(N_face, N_geom) < 0.0f) ? -N_face : N_face;
        }
#if AERON_PBR_DEBUG_VIEWS
    }
#endif
    float3 N = N_geom;
    if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_NORMAL) != 0u) {
        float3 ntex = pbr_sample_channel(g_normal, g_normal_sampler, g_materials[mat],
                                         GLTF_ATLAS_NORMAL, g_materials[mat].normal_rect,
                                         i.uv).rgb * 2.0f - 1.0f;
        float  nz   = sqrt(saturate(1.0f - dot(ntex.xy, ntex.xy)));
        float3 nt   = float3(ntex.xy, max(ntex.z, nz));
        /* Perturb in the authored front-face frame, then flip the result
         * with the face: seen from behind, the surface normal is the
         * front normal reversed, tangential part included. */
        float3 N_front = N_geom * side_sign;
        float3 T    = normalize(i.world_tangent);
        float3 B    = normalize(cross(N_front, T)) * i.tangent_sign;
        N = normalize(T * nt.x + B * nt.y + N_front * nt.z) * side_sign;
    }

    /* ===== View + light ========================================== */
#if AERON_PBR_PROBES
    /* The floor's mirrored view (planar_reflection.c) looks from its own
     * camera. */
    float3 eye = fs_planar_eye.w != 0.0f ? fs_planar_eye.xyz : fs_camera_pos_world;
#else
    float3 eye = fs_camera_pos_world;
#endif
    float3 V = normalize(eye - i.world_pos);
    float3 L = fs_directional_dir;
    float  ndotl = saturate(dot(N, L));

    /* Lights carry illuminance: the Lambert BRDF is albedo / pi, as for
     * the environment's irradiance, consistent with the GGX specular. */
    const float inv_pi = 0.31830989f;
    float  lambert_term = ndotl * inv_pi;
    float3 ambient_rgb  = world_ambient(N);
    /* Kept apart from the ambient so specular image-based lighting can
     * take its Fresnel share (1 - F_ss) below. */
#if AERON_PBR_ENVIRONMENT
    float3 environment_diffuse_rgb = detailed_environment(N);
#else
    float3 environment_diffuse_rgb = float3(0.0f, 0.0f, 0.0f);
#endif
#if AERON_PBR_PROBES
    /* Inside the grid, its baked light replaces the environment and the
     * world ambient, which it already holds; they fade back in at its
     * border. */
    float3 probe_point = probe_model_point(i.world_pos);
    float  grid_weight, grid_luminance, grid_light;
    float3 grid_rgb = probe_grid_diffuse(probe_point, probe_model_direction(N), grid_weight, grid_luminance,
                                         grid_light);
    /* Ship occlusion (AeronScene_SetProbeOcclusion): the probe light the
     * occluders inside the grid leave this point. */
    bool  probe_occlusion = fs_occlusion_origin.w != 0.0f;
    float occlusion_diffuse =
        probe_occlusion ? probe_occlusion_diffuse(probe_point, probe_model_direction(N)) : 1.0f;
    environment_diffuse_rgb = lerp(environment_diffuse_rgb, grid_rgb * occlusion_diffuse, grid_weight);
    ambient_rgb *= 1.0f - grid_weight;
#endif
    uint shadow_cascade = 4u;
    float shadow_cascade_blend = 0.0f;
    float shadow_coverage = 0.0f;
    /* The half-resolution visibility pass: R is SSAO, which occludes
     * indirect (ambient) light only (pow() applies the contrast knob, then
     * the intensity lerp blends toward unoccluded; intensity 0 → ao 1); G is
     * the directional shadow of opaque receivers. Blended surfaces lie over
     * the depth layer it describes: no lookup. */
    float ao = 1.0f;
    float screen_shadow_visibility = 1.0f;
    /* B: the key light's shadow 1 m out, or with probe occlusion what the
     * mirror direction's cone meets (scene_ssao.hlsli). */
    float screen_key_visibility = 1.0f;
#if AERON_PBR_MIRROR
    /* The mirrored view has no visibility pass of its own. */
    bool read_visibility = false;
#elif AERON_PBR_PROBES
    bool read_visibility = ssao_intensity > 0.0f || i.screen_shadow != 0u || probe_occlusion;
#else
    bool read_visibility = ssao_intensity > 0.0f || i.screen_shadow != 0u;
#endif
    if (!AERON_PBR_ALPHA_BLEND && read_visibility) {
        float4 visibility = g_ao.Sample(g_ao_sampler, i.position.xy * fs_render_size.zw);
        screen_shadow_visibility = visibility.y;
        screen_key_visibility = visibility.z;
        if (ssao_intensity > 0.0f) {
            ao = lerp(1.0f, pow(visibility.x, ssao_power), ssao_intensity);
        }
    }

    float shadow_visibility = 1.0f;
    if (i.receive_shadow != 0u) {
        if (!AERON_PBR_ALPHA_BLEND && i.screen_shadow != 0u) {
            shadow_visibility = screen_shadow_visibility;
        } else {
            /* The shadow scales only the sun's diffuse and specular terms.
             * Both are zero when N and N_face (and so N_spec, their
             * blend) face away from the light: skip the lookup. */
            float shadow_ndotl = max(dot(N, L), dot(N_face, L));
            /* Opaque receivers read the screen pass, so outside the debug
             * variant (whose cascade view needs this path) only blended
             * materials come here: one comparison, as a soft penumbra is not
             * discernible through canopy glass. Compiling the filter out
             * keeps it from costing the whole shader. */
            bool shadow_single_tap = !AERON_PBR_DEBUG_VIEWS || AERON_PBR_ALPHA_BLEND ||
                                     (g_materials[mat].flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u;
            shadow_visibility = directional_shadow_visibility(
                i.world_pos, N_face, shadow_ndotl,
                world_pos_dx, world_pos_dy, i.position.xy, shadow_single_tap,
                shadow_cascade, shadow_cascade_blend, shadow_coverage);
        }
    }

    /* AO occludes ambient (with the multi-bounce fit below); it occludes
     * the direct diffuse only by `ssao_direct` (0 = physically correct
     * ambient-only, 1 = direct fully occluded). Specular / emissive / local
     * lights are untouched. */
    float ao_direct = lerp(1.0f, ao, ssao_direct);
    /* Additional directionals: plain Lambert, diffuse only (the XWA
     * classic sums max(0, N.L) * color per light with no wrap and no
     * specular; zero-color slots contribute nothing). */
    float3 extra_rgb = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int ed = 0; ed < 3; ed++) {
        extra_rgb += saturate(dot(N, fs_extra_dir[ed].xyz)) * inv_pi * fs_extra_col[ed].rgb;
    }
    /* Ambient light keeps more of itself in creases on bright surfaces,
     * which reflect light back into them: the multi-bounce fit of Jimenez
     * et al. 2016 ("Practical Real-Time Strategies for Accurate Indirect
     * Occlusion"), per channel with the albedo. */
    float3 ao_a = 2.0404f * albedo - 0.3324f;
    float3 ao_b = -4.7951f * albedo + 0.6417f;
    float3 ao_c = 2.7552f * albedo + 0.6903f;
    float3 ao_ambient = max(ao, ((ao * ao_a + ao_b) * ao + ao_c) * ao);
    /* A probe set is treated as sealed. Inside its grid, the probes contain
     * all the light (of a hangar or a bay), so the directional lights fade
     * out with the grid weight. When the key light is in the probes' bake
     * (`key_shadow_in_probes`), a key light shadow inside the grid means
     * that an object is between the light's side (a hangar ceiling) and the
     * surface. The shadow removes `key_probe_shadow` of the probe light that
     * comes from that side, whichever way the surface faces. That light is
     * the surface's whole grid light minus the light from the opposite side
     * (the floor), of which the surface sees the share (1 - N.L) / 2.
     * Reflections are reduced by the same amount. For a surface that faces
     * away from the light, the shadow is read 1 m out along its normal (the
     * visibility pass's B, see AeronScene_SetProbeKeyShadow). This measures
     * what covers the space in front of the surface, not the surface's own
     * curve, which does not block its view. It avoids a hard line where a
     * hull turns away from the light. */
    float3 key_color = sun_color;
#if AERON_PBR_PROBES
    key_color *= 1.0f - grid_weight;
    extra_rgb *= 1.0f - grid_weight;
    float key_occlusion = 1.0f;
    /* Ship occlusion replaces the key light shadow's cut. */
    if (key_shadow_in_probes != 0.0f && !probe_occlusion) {
        float key_visibility = shadow_visibility;
        float facing         = saturate(dot(N, L) * 2.5f - 0.75f); /* 0 below N.L 0.3, 1 above 0.7 */
        if (key_shadow_facing == 0.0f && !AERON_PBR_ALPHA_BLEND && i.screen_shadow != 0u) {
            key_visibility = lerp(screen_key_visibility, shadow_visibility, facing);
        }
        /* Without its shadow facing, the key light's share of the grid's
         * light: what the grid holds from the side away from the light. */
        float from_key_side = saturate(dot(N, L));
        if (key_shadow_facing == 0.0f) {
            float away    = probe_grid_luminance(probe_point, -probe_model_direction(fs_directional_dir));
            from_key_side = saturate(1.0f - away * 0.5f * (1.0f - dot(N, L)) / max(grid_light, 1.0e-4f));
        }
        key_occlusion = 1.0f - key_probe_shadow * grid_weight * (1.0f - key_visibility) * from_key_side;
        environment_diffuse_rgb *= key_occlusion;
    }
#endif
    float3 base_rgb = albedo *
        ((key_color * lambert_term * shadow_visibility + extra_rgb) * ao_direct + ambient_rgb * ao_ambient);

    /* ===== Metallic-roughness ==================================== */
    PbrMaterialParams mp;
    mp.spec_intensity = 1.0f;
    mp.roughness      = g_materials[mat].metal_rough.y;
    mp.F0             = float3(0.04f, 0.04f, 0.04f);
    mp.F90            = 1.0f;
    mp.albedo         = albedo;
    float metallic = g_materials[mat].metal_rough.x;
    if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS) != 0u) {
        float3 mr = pbr_sample_channel(g_mr, g_mr_sampler, g_materials[mat],
                                       GLTF_ATLAS_METALLIC_ROUGHNESS,
                                       g_materials[mat].mr_rect, i.uv).rgb;
        mp.roughness = mr.g * g_materials[mat].metal_rough.y;
        metallic     = mr.b * g_materials[mat].metal_rough.x;
    }
#if AERON_PBR_DEBUG_VIEWS
    if (fs_debug_material.x != 0.0f) {
        metallic     = fs_debug_material.y;
        mp.roughness = fs_debug_material.z;
    }
#endif
    float specular_factor = g_materials[mat].specular.a;
#if AERON_PBR_ANIM
    /* Damage dulls the surface: the gloss factor raises roughness and
     * scales metallic and specular. */
    mp.roughness = 1.0f - (1.0f - mp.roughness) * anim_gloss;
    metallic *= anim_gloss;
    specular_factor *= anim_gloss;
#endif
    mp.roughness = max(mp.roughness, 0.045f);
    /* KHR_materials_specular: the dielectric F0 is scaled by the colour
     * factor and both dielectric Fresnel ends by specularFactor; the
     * defaults (1, 1) give the plain metallic-roughness model. */
    float3 f0_dielectric = min(0.04f * g_materials[mat].specular.rgb, 1.0f) * specular_factor;
    mp.F0  = lerp(f0_dielectric, albedo, metallic);
    mp.F90 = lerp(specular_factor, 1.0f, metallic);
    base_rgb *= (1.0f - metallic);

    /* ===== Cook-Torrance specular ================================
     * Geometric-aware shading-normal adaptation. The Gouraud / normal-
     * mapped shading normal N can tilt toward or below the view horizon
     * on low-poly legacy meshes (the 1998 OPT normals deviate up to ~60°
     * from the face), where it is untrustworthy. There the microfacet
     * term D·G·F/(4·N·V) holds a finite grazing value as N·V→0⁺ and then
     * `saturate(N·V)` clips it to zero — a hard N·V=0 ring on a flat face.
     *
     * Fix: derive the geometric face normal from screen-space derivatives
     * of world_pos and blend N toward it as N·V→0. Where the shading
     * normal is sub-horizon (untrusted) specular reflects the real flat
     * surface instead of ringing; where the geometry itself grazes (true
     * silhouette) the geometric normal grazes too, so a legitimate
     * Fresnel rim survives. Specular only — diffuse/ambient keep N. */
    float3 N_spec = N;
#if AERON_PBR_DEBUG_VIEWS
    if (spec_geom_adapt != 0.0f) {
        N_spec = normalize(lerp(N_face, N, smoothstep(0.0f, 0.2f, dot(N, V))));
    }
#else
    N_spec = normalize(lerp(N_face, N, smoothstep(0.0f, 0.2f, dot(N, V))));
#endif

    /* No explicit N·L gate: cook_torrance_spec's Smith G_L term already
     * drives specular to zero as N·L→0, so the dark side is handled
     * without a redundant smoothstep on the terminator. */
    /* ===== Image-based lighting ================================
     * Split-sum specular from the GGX-prefiltered cube (mip = roughness
     * x (mips - 1)), read in the dominant direction, and the BRDF table,
     * occluded by SSAO with Lagarde's N.V-dependent approximation so
     * creases do not reflect the open sky.
     * The diffuse environment keeps the energy specular does not take.
     * Not shadowed: the directional shadow covers the analytic light only.
     * Without a specular cube both terms are as before. */
    float3 spec_ibl_rgb = float3(0.0f, 0.0f, 0.0f);
    float3 environment_diffuse_weight = float3(1.0f, 1.0f, 1.0f);
#if AERON_PBR_PROBES
    /* The probes cover what they can; the environment's cube, when there
     * is one, the rest. */
    float3 R_ibl = specular_dominant_direction(N_spec, reflect(-V, N_spec), mp.roughness);
    /* Reflections pointing into the surface fade out. */
    float horizon = min(1.0f + dot(R_ibl, N_face), 1.0f);
#if AERON_PBR_MIRROR
    /* Blurred in the floor, the probes' reflection is not worth its cost:
     * the light the surface takes from the grid (and outside it, the
     * environment) stands in for it. */
    float3 probe_radiance = environment_diffuse_rgb;
    float  probe_coverage = 1.0f;
#else
    float3 probe_radiance;
    float  probe_coverage = probe_specular(probe_point, probe_model_direction(R_ibl), mp.roughness,
                                           probe_model_direction(N), grid_luminance, grid_weight, probe_radiance);
    probe_radiance *= key_occlusion;
    if (probe_occlusion) {
        /* Glossy surfaces see what the visibility pass's cone along the
         * mirror direction meets (its B), rough ones the diffuse occlusion,
         * weighed by the lobe's width (roughness squared). Toward the floor,
         * the cubes show it lit as if the occluders were not there: it
         * takes the floor's own occlusion where the ray meets it. */
        float3 rm         = probe_model_direction(R_ibl);
        float  reflection = lerp(screen_key_visibility, occlusion_diffuse, saturate(mp.roughness * mp.roughness));
        if (rm.z < -0.05f) {
            float3 hit = probe_point + rm * max((fs_occlusion_origin.z - probe_point.z) / rm.z, 0.0f);
            reflection *= probe_occlusion_diffuse(hit, float3(0.0f, 0.0f, 1.0f));
        }
        probe_radiance *= reflection;
    }
    /* On the reflecting floor, what stands on it in place of the probes
     * where it covers them. */
    float4 planar = planar_reflection(i.position.xy, probe_point, probe_model_direction(N_face),
                                      length(probe_model_point(eye) - probe_point), mp.roughness);
    probe_radiance = probe_radiance * (1.0f - planar.a) + planar.rgb;
    probe_coverage = lerp(probe_coverage, 1.0f, planar.a);
#endif
    bool   environment_specular = fs_environment_params.y > 0.0f && fs_environment_params.z >= 1.0f;
    if (probe_coverage > 0.0f || environment_specular) {
        float  ndotv_ibl = saturate(dot(N_spec, V));
        float3 prefiltered = probe_radiance;
        if (environment_specular && probe_coverage < 1.0f) {
            prefiltered += (1.0f - probe_coverage) * fs_environment_params.y *
                           max(g_specular_environment.SampleLevel(
                                   g_specular_environment_sampler, normalize(environment_local(R_ibl)),
                                   mp.roughness * (fs_environment_params.z - 1.0f)).rgb, 0.0f);
        }
        float2 split = g_brdf_lut.SampleLevel(g_brdf_lut_sampler, float2(ndotv_ibl, mp.roughness), 0.0f);
        float3 F_ss = mp.F0 * split.x + mp.F90 * split.y;
        float  specular_occlusion =
            saturate(pow(ndotv_ibl + ao, exp2(-16.0f * mp.roughness - 1.0f)) - 1.0f + ao);
        spec_ibl_rgb = prefiltered * F_ss * specular_occlusion * horizon * horizon;
        environment_diffuse_weight = 1.0f - F_ss;
    }
#else
    if (AERON_PBR_ENVIRONMENT && fs_environment_params.y > 0.0f && fs_environment_params.z >= 1.0f) {
        float  ndotv_ibl = saturate(dot(N_spec, V));
        float3 R = specular_dominant_direction(N_spec, reflect(-V, N_spec), mp.roughness);
        float horizon = min(1.0f + dot(R, N_face), 1.0f);
        float3 prefiltered = g_specular_environment.SampleLevel(
            g_specular_environment_sampler, normalize(environment_local(R)),
            mp.roughness * (fs_environment_params.z - 1.0f)).rgb;
        float2 split = g_brdf_lut.SampleLevel(g_brdf_lut_sampler, float2(ndotv_ibl, mp.roughness), 0.0f);
        float3 F_ss = mp.F0 * split.x + mp.F90 * split.y;
        float  specular_occlusion =
            saturate(pow(ndotv_ibl + ao, exp2(-16.0f * mp.roughness - 1.0f)) - 1.0f + ao);
        spec_ibl_rgb = max(prefiltered, 0.0f) * F_ss * fs_environment_params.y * specular_occlusion * horizon *
                       horizon;
        environment_diffuse_weight = 1.0f - F_ss;
    }
#endif
    float3 environment_diffuse_term =
        albedo * (1.0f - metallic) * environment_diffuse_rgb * environment_diffuse_weight * ao_ambient;
    base_rgb += environment_diffuse_term;

    float  G_term;
    float3 spec_brdf = cook_torrance_spec(N_spec, V, L, mp, G_term);
    float3 spec_rgb  = spec_brdf
                     * (key_color * mp.spec_intensity)
                     * shadow_visibility;

    /* Clustered punctual lights (laser bolts, explosions, engine
     * glows): windowed inverse-square spheres and tubes, Lambert diffuse
     * and per-light specular. Untouched by AO. */
#if !AERON_PBR_MIRROR
    if (fs_cluster_point_count > 0u) {
        float3 point_diff = accumulate_point_lights(N, N_spec, V, i.world_pos,
                                                    i.position.xy, mp, spec_rgb);
        base_rgb += albedo * (1.0f - metallic) * inv_pi * point_diff;
    }
#endif

    float3 lit = base_rgb + spec_rgb + spec_ibl_rgb;

#if AERON_PBR_DEBUG_VIEWS
    if (fs_cluster_debug_view != 0u && fs_cluster_enabled != 0u) {
        _out.color = float4(clustered_light_debug_color(i.position.xy, i.world_pos), 1.0f);
        return _out;
    }
    if (shadow_params.w != 0.0f && i.receive_shadow != 0u) {
        const float3 cascade_colors[4] = {
            float3(1.0f, 0.35f, 0.35f),
            float3(0.35f, 1.0f, 0.35f),
            float3(0.35f, 0.55f, 1.0f),
            float3(1.0f, 0.75f, 0.25f)
        };
        const float3 no_cascade_color = float3(0.12f, 0.12f, 0.12f);
        uint cascade_count = min((uint)shadow_params.y, 4u);
        float3 cascade_color = no_cascade_color;
        if (shadow_cascade < cascade_count) {
            cascade_color = cascade_colors[shadow_cascade];
            if (shadow_cascade + 1u < cascade_count) {
                cascade_color = lerp(cascade_color, cascade_colors[shadow_cascade + 1u],
                                     shadow_cascade_blend);
            }
            cascade_color = lerp(no_cascade_color, cascade_color, shadow_coverage);
        }
        _out.color = float4(cascade_color, 1.0f);
        return _out;
    }
#endif

    int isolate = 0;
#if AERON_PBR_DEBUG_VIEWS
    /* Modes 1-3 substitute one lit composition term and still run
     * through emissive multiplication below. Modes 4+ are pure
     * diagnostic visualisations of geometric quantities; they
     * bypass emissive so the raw value is visible. */
    isolate = (int)round(debug_isolate_term);
    if      (isolate == 1) lit = base_rgb;
    else if (isolate == 2) lit = spec_rgb;
    else if (isolate == 3) lit = G_term.xxx;
    else if (isolate == 4) lit = saturate(dot(N, V)).xxx;
    else if (isolate == 5) lit = saturate(dot(N, L)).xxx;
    else if (isolate == 6) lit = N * 0.5f + 0.5f;
    else if (isolate == 7) lit = V * 0.5f + 0.5f;
    else if (isolate == 8) lit = (dot(N, V) * 0.5f + 0.5f).xxx;
    else if (isolate == 9) lit = environment_diffuse_term;
    else if (isolate == 10) lit = spec_ibl_rgb;
    if (isolate >= 4) { _out.color = float4(lit, 1.0f); return _out; }
#endif

    /* ===== Emissive ==============================================
     * A positive per-instance base-color override represents semantic
     * emission that is not authored in the material (XWA runtime-OPT
     * projectiles). It replaces lighting and material emission while
     * retaining the base texture's alpha-blended soft edge below.
     * Otherwise the material's emission adds to the lit surface. */
    if (isolate == 0 && i.base_color_emissive_strength > 0.0f) {
        lit = albedo * (i.base_color_emissive_strength * fs_emission.x);
#if AERON_PBR_ANIM
    } else if (anim_emission) {
        lit += anim_emissive * (i.emissive_mul * fs_emission.x);
#endif
    } else {
        float3 emissive = g_materials[mat].emissive_packed.rgb;
        if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
            emissive *= pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat], GLTF_ATLAS_EMISSIVE,
                                           g_materials[mat].emissive_rect, i.uv).rgb;
        }
        lit += emissive * (g_materials[mat].emissive_packed.a * i.emissive_mul * fs_emission.x);
    }

#if AERON_PBR_ANIM
    /* ===== Energy effect ==========================================
     * Additive emission over the lit surface, scaled like the material's. */
    if (isolate == 0) {
        lit += anim_energy_rgb * (i.emissive_mul * fs_emission.x);
    }
#endif

#if AERON_PBR_SCREENS
    /* ===== Opaque screens =========================================
     * Unlit screen radiance, shown where the cover is transparent (all of
     * the surface without a cover). A playing colour frame covers the
     * display by its coverage over a cover's window, and replaces the
     * display of a screen without a cover. */
    if (g_materials[mat].screen_count != 0u && isolate == 0) {
        float3 screen = screen_evaluate(g_materials[mat], i.uv, screen_load_frame());
        bool   cover  = (g_materials[mat].screen_flags & SCREEN_COVER) != 0u;
        float  open   = cover ? 1.0f - albedo_tex.a : 1.0f;
#if AERON_PBR_ANIM
        if (anim_screen_frame)
            open = cover ? open * (1.0f - anim_screen_cover) : 0.0f;
#endif
        lit += screen * fs_emission.x * open;
    }
#endif

#if AERON_PBR_ALPHA_BLEND
    /* ===== Transmission ===========================================
     * KHR_materials_transmission, thin and without refraction: the surface
     * diffuses with c = 1 - transmission T (the base texture's alpha holds
     * the transmission texture, aeron_gltf_cook), and passes the background
     * times T and the base colour, as the spec tints it; with one blend
     * factor the tint is the base colour's Rec. 709 luminance Y. Its
     * specular reflection does not fade with c. Fresnel hides more of the
     * background at grazing angles: a = 1 - T·Y·(1 - F). The blend state is
     * SRC_ALPHA, so the colour is divided by a to composite as
     * surface·c + specular + background·(1 - a). */
    if ((g_materials[mat].flags & GLTF_MATERIAL_TRANSMISSION) != 0u && isolate == 0) {
        float  transmission = saturate(g_materials[mat].metal_rough.w * albedo_tex.a);
        float  coverage     = 1.0f - transmission;
        float  tint         = saturate(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)));
        float3 fresnel      = mp.F0 + (mp.F90 - mp.F0) * pow(1.0f - saturate(dot(N_spec, V)), 5.0f);
        float  covered = 1.0f - transmission * tint * (1.0f - saturate(dot(fresnel, 1.0f / 3.0f)));
        float3 specular = spec_rgb + spec_ibl_rgb;
        _out.color = float4(((lit - specular) * coverage + specular) / max(covered, 1.0e-3f), covered);
        return _out;
    }
#endif

    /* Alpha-BLEND materials (flag bit 3 — canopy glass, drawn by the
     * blend pipelines) carry texture x factor alpha; every opaque
     * material writes 1 so the scene RT keeps full coverage. */
    float alpha = ((g_materials[mat].flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u)
                      ? saturate(albedo_tex.a * g_materials[mat].base_color_factor.a)
                      : 1.0f;
    _out.color = float4(lit, alpha);
    return _out;
}
