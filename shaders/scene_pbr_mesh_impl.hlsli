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
 * image-based lighting, for scenes without an environment. All keep this
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

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#if AERON_PBR_ANIM
#define AERON_PBR_MATERIAL_REGISTER t14
#define AERON_PBR_VARIANT_REGISTER t15
#define AERON_PBR_POINT_LIGHT_REGISTER t16
#define AERON_CLUSTER_HEADER_REGISTER t17
#define AERON_CLUSTER_INDEX_REGISTER t18
#define AERON_PBR_SCREEN_RECT_REGISTER t19
#define AERON_PBR_SCREEN_FRAME_REGISTER t20
#define AERON_PBR_ANIM_RECORD_REGISTER t21
#else
#define AERON_PBR_MATERIAL_REGISTER t12
#define AERON_PBR_VARIANT_REGISTER t13
#define AERON_PBR_POINT_LIGHT_REGISTER t14
#define AERON_CLUSTER_HEADER_REGISTER t15
#define AERON_CLUSTER_INDEX_REGISTER t16
#define AERON_PBR_SCREEN_RECT_REGISTER t17
#define AERON_PBR_SCREEN_FRAME_REGISTER t18
#endif
#include "scene_pbr_lighting.hlsli"
#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "srgb.hlsli"
#include "scene_pbr_material_alpha.hlsli"
#include "scene_pbr_screen.hlsli"

Texture2DArray g_normal     : register(t1, space2);
SamplerState g_normal_sampler : register(s1, space2);
Texture2DArray g_mr         : register(t2, space2);
SamplerState g_mr_sampler   : register(s2, space2);
Texture2DArray g_emissive   : register(t3, space2);
SamplerState g_em_sampler   : register(s3, space2);
/* Half-res RG8 visibility: R = SSAO, G = denoised main directional
 * shadow. A 1×1 white placeholder keeps both effects unoccluded when the
 * joint visibility pass is unavailable. */
Texture2D<float2> g_ao      : register(t4, space2);
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

/* A flipbook's frame UV: its uv record's scale and offset, clamped to the
 * image when set. */
float2 anim_frame_uv(uint flipbook, float2 uv)
{
    uint x = g_anim_records[flipbook].x;
    if ((x & AERON_ANIM_FLIPBOOK_HAS_UV) == 0u) return uv;
    uint4  t = g_anim_records[flipbook + 1u];
    float2 r = uv * float2(asfloat(t.x), asfloat(t.y)) + float2(asfloat(t.z), asfloat(t.w));
    return (x & AERON_ANIM_FLIPBOOK_UV_CLAMP) != 0u ? min(saturate(r), 0.99999f) : r;
}

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
     * the result; illumination: base = c·k + layer_rgb over that colour c
     * (while it plays, the material's aeronAnimBaseColor when it has one, else
     * its base colour), and its emission replaces the material's. Without
     * an illumination frame, a colour flipbook's unlit weight u splits the
     * result as the static bake does: base c·(1 − u), emission c·u in place
     * of the material's. A layer without a playing frame keeps the static
     * material. */
    bool   anim_emission = false;
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
        float3 c = albedo_tex.rgb;
        if (lit_frame != 0u && g_materials[mat].anim_base_rect.z > 0.0f) {
            c = atlas_sample(g_base_color, g_base_sampler, i.uv, g_materials[mat].anim_base_rect,
                             anim.x & 0xFFFu, (anim.x >> 12) & 0xFu).rgb;
        }
        if (colour_frame != 0u) {
            float4 frame = anim_frame_sample(g_effect_layer, g_effect_layer_sampler, g_anim_records[colour_frame],
                                             anim_frame_uv(colour_book, i.uv));
            c = lerp(c, frame.rgb, frame.a);
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
            float2 frame_uv = anim_frame_uv(lit_book, i.uv);
            float4 layer = anim_frame_sample(g_effect_layer, g_effect_layer_sampler, g_anim_records[lit_frame],
                                             frame_uv);
            c = c * layer.a + layer.rgb;
            uint4 lit_emissive = g_anim_records[lit_frame + 1u];
            anim_emissive = anim_frame_sample(g_effect_emissive, g_effect_emissive_sampler, lit_emissive,
                                              frame_uv).rgb * asfloat(lit_emissive.w);
            anim_emission = true;
        }
        albedo_tex.rgb = c;
    }
#endif
    float3 albedo = albedo_tex.rgb * g_materials[mat].base_color_factor.rgb;

#if AERON_PBR_SCREENS
    /* ===== Transparent screens ===================================
     * Unlit (no normal or shading): the cover, when present,
     * over the screen, with alpha from the display luminance. */
    if ((g_materials[mat].screen_flags & SCREEN_TRANSPARENT) != 0u) {
        ScreenFrame screen_frame = screen_load_frame();
        if ((screen_frame.flags & SCREEN_FRAME_HIDE_TRANSPARENT) != 0u &&
            (g_materials[mat].screen_flags & SCREEN_ALWAYS_VISIBLE) == 0u) {
            _out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return _out;
        }
        float3 screen = screen_evaluate(g_materials[mat], i.uv, screen_frame);
        float3 color = screen;
        if ((g_materials[mat].screen_flags & SCREEN_COVER) != 0u) {
            float3 cover_glow = float3(0.0f, 0.0f, 0.0f);
            if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
                cover_glow = pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat], GLTF_ATLAS_EMISSIVE,
                                                g_materials[mat].emissive_rect, i.uv).rgb *
                             g_materials[mat].emissive_packed.rgb * g_materials[mat].emissive_packed.a;
            }
            color = albedo + cover_glow + screen * (1.0f - albedo_tex.a);
        }
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
    float3 V = normalize(fs_camera_pos_world - i.world_pos);
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
    if (!AERON_PBR_ALPHA_BLEND && (ssao_intensity > 0.0f || i.screen_shadow != 0u)) {
        float2 visibility = g_ao.Sample(g_ao_sampler, i.position.xy * fs_render_size.zw);
        screen_shadow_visibility = visibility.y;
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

    /* AO occludes ambient fully; it occludes the direct diffuse only by
     * `ssao_direct` (0 = physically correct ambient-only, 1 = direct
     * fully occluded). Specular / emissive / local lights are untouched. */
    float ao_direct = lerp(1.0f, ao, ssao_direct);
    /* Additional directionals: plain Lambert, diffuse only (the XWA
     * classic sums max(0, N.L) * color per light with no wrap and no
     * specular; zero-color slots contribute nothing). */
    float3 extra_rgb = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int ed = 0; ed < 3; ed++) {
        extra_rgb += saturate(dot(N, fs_extra_dir[ed].xyz)) * inv_pi * fs_extra_col[ed].rgb;
    }
    float3 base_rgb = albedo *
        ((sun_color * lambert_term * shadow_visibility + extra_rgb) * ao_direct + ambient_rgb * ao);

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
     * x (mips - 1)) and the BRDF table, occluded by SSAO with Lagarde's
     * N.V-dependent approximation so creases do not reflect the open sky.
     * The diffuse environment keeps the energy specular does not take.
     * Not shadowed: the directional shadow covers the analytic light only.
     * Without a specular cube both terms are as before. */
    float3 spec_ibl_rgb = float3(0.0f, 0.0f, 0.0f);
    float3 environment_diffuse_weight = float3(1.0f, 1.0f, 1.0f);
    if (AERON_PBR_ENVIRONMENT && fs_environment_params.y > 0.0f && fs_environment_params.z >= 1.0f) {
        float  ndotv_ibl = saturate(dot(N_spec, V));
        float3 R = reflect(-V, N_spec);
        float3 prefiltered = g_specular_environment.SampleLevel(
            g_specular_environment_sampler, normalize(environment_local(R)),
            mp.roughness * (fs_environment_params.z - 1.0f)).rgb;
        float2 split = g_brdf_lut.SampleLevel(g_brdf_lut_sampler, float2(ndotv_ibl, mp.roughness), 0.0f);
        float3 F_ss = mp.F0 * split.x + mp.F90 * split.y;
        float  specular_occlusion =
            saturate(pow(ndotv_ibl + ao, exp2(-16.0f * mp.roughness - 1.0f)) - 1.0f + ao);
        spec_ibl_rgb = max(prefiltered, 0.0f) * F_ss * fs_environment_params.y * specular_occlusion;
        environment_diffuse_weight = 1.0f - F_ss;
    }
    float3 environment_diffuse_term =
        albedo * (1.0f - metallic) * environment_diffuse_rgb * environment_diffuse_weight * ao;
    base_rgb += environment_diffuse_term;

    float  G_term;
    float3 spec_brdf = cook_torrance_spec(N_spec, V, L, mp, G_term);
    float3 spec_rgb  = spec_brdf
                     * (sun_color * mp.spec_intensity)
                     * shadow_visibility;

    /* Clustered punctual lights (laser bolts, explosions, engine
     * glows): windowed inverse-square spheres and tubes, Lambert diffuse
     * and per-light specular. Untouched by AO. */
    if (fs_cluster_point_count > 0u) {
        float3 point_diff = accumulate_point_lights(N, N_spec, V, i.world_pos,
                                                    i.position.xy, mp, spec_rgb);
        base_rgb += albedo * (1.0f - metallic) * inv_pi * point_diff;
    }

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
     *
     * Otherwise, materials using the legacy sRGB/SRCALPHA mode carry
     * glow coverage in alpha. Untagged glTF materials retain standard
     * additive emissive composition. */
    if (isolate == 0 && i.base_color_emissive_strength > 0.0f) {
        lit = albedo * (i.base_color_emissive_strength * fs_emission.x);
#if AERON_PBR_ANIM
    } else if (anim_emission) {
        lit += anim_emissive * (i.emissive_mul * fs_emission.x);
#endif
    } else {
        float3 emissive = g_materials[mat].emissive_packed.rgb;
        float emissive_coverage = 0.0f;
        if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
            float4 emissive_tex = pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat],
                                                     GLTF_ATLAS_EMISSIVE,
                                                     g_materials[mat].emissive_rect, i.uv);
            emissive_coverage = emissive_tex.a;

            float3 emissive_rgb = emissive_tex.rgb;
            if ((g_materials[mat].flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
                /* Against transparent black, the sRGB hardware sample is
                 * approximately coverage * linear(glow_rgb). Recover the
                 * glow colour and reproduce both classic operations in
                 * encoded space: bilinear RGB coverage, followed by the
                 * SRCALPHA source-blend multiplication. Then return to
                 * linear HDR space. */
                if (emissive_coverage > 1.0e-5f) {
                    float3 glow_linear = saturate(emissive_rgb / emissive_coverage);
                    float3 glow_srgb = AeronLinearToSrgb(glow_linear);
                    float coverage = saturate(emissive_coverage);
                    emissive_rgb = AeronSrgbToLinear(glow_srgb * coverage * coverage);
                } else {
                    emissive_rgb = 0.0f;
                }
            }
            emissive *= emissive_rgb;
        }
        float emissive_mul = i.emissive_mul;
        emissive *= g_materials[mat].emissive_packed.a * emissive_mul * fs_emission.x;
        if ((g_materials[mat].flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
            lit = emissive + lit * (1.0f - saturate(emissive_coverage * emissive_mul));
        } else {
            lit += emissive;
        }
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
     * the surface without a cover). */
    if (g_materials[mat].screen_count != 0u && isolate == 0) {
        float3 screen = screen_evaluate(g_materials[mat], i.uv, screen_load_frame());
        lit += screen * fs_emission.x *
               ((g_materials[mat].screen_flags & SCREEN_COVER) != 0u ? 1.0f - albedo_tex.a : 1.0f);
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
