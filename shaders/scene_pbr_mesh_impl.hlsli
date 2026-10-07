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

/* Variants: AERON_PBR_SCREENS 0 drops the screen code, which the renderer
 * draws with its own pipeline (a mesh's screen primitives). AERON_PBR_ALPHA_BLEND
 * 1 is the variant for blended materials. Both keep this shader's register
 * count for ordinary materials down. */
#ifndef AERON_PBR_SCREENS
#define AERON_PBR_SCREENS 1
#endif
#ifndef AERON_PBR_ALPHA_BLEND
#define AERON_PBR_ALPHA_BLEND 0
#endif

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#define AERON_PBR_MATERIAL_REGISTER t12
#define AERON_PBR_VARIANT_REGISTER t13
#define AERON_PBR_POINT_LIGHT_REGISTER t14
#define AERON_CLUSTER_HEADER_REGISTER t15
#define AERON_CLUSTER_INDEX_REGISTER t16
#define AERON_PBR_SCREEN_RECT_REGISTER t17
#define AERON_PBR_SCREEN_FRAME_REGISTER t18
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
    GltfMaterial m = pbr_resolve_material(
        i.prim_id, i.variant_row_base, i.variant_group_count,
        i.material_count);

#if AERON_PBR_SCREENS
    /* ===== Hologram screens (unlit, straight alpha) ===============
     * Hidden screens return transparent black instead of discarding: they
     * are always blended (the loader keeps them in the blend range), where
     * it leaves the target unchanged, and a discard anywhere in this shader
     * would cost every opaque draw its early depth and hidden-surface
     * removal. */
    if ((m.screen_flags & SCREEN_HOLOGRAM) != 0u) {
        ScreenFrame holo_frame = screen_load_frame();
        if (holo_frame.holo_fade <= 0.01f) {
            _out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return _out;
        }
        _out.color = screen_hologram(m, i.uv, holo_frame);
        return _out;
    }
#endif

    /* ===== Base color ============================================ */
    float4 albedo_tex = pbr_sample_base_color(m, i.uv);
#if AERON_PBR_ALPHA_MASK
    pbr_apply_alpha_mask(albedo_tex.a * m.base_color_factor.a,
                         m.metal_rough.z);
#endif
    float3 albedo = albedo_tex.rgb * m.base_color_factor.rgb;

#if AERON_PBR_SCREENS
    /* ===== Transparent screens ===================================
     * Unlit like XWAU's (no normal or shading): the cover, when present,
     * over the screen, with alpha from the display luminance. */
    if ((m.screen_flags & SCREEN_TRANSPARENT) != 0u) {
        ScreenFrame screen_frame = screen_load_frame();
        if ((screen_frame.flags & SCREEN_FRAME_HIDE_TRANSPARENT) != 0u &&
            (m.screen_flags & SCREEN_ALWAYS_VISIBLE) == 0u) {
            _out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return _out;
        }
        float3 screen = screen_evaluate(m, i.uv, screen_frame);
        float3 color = screen;
        if ((m.screen_flags & SCREEN_COVER) != 0u) {
            float3 cover_glow = float3(0.0f, 0.0f, 0.0f);
            if ((m.flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
                cover_glow = pbr_sample_channel(g_emissive, g_em_sampler, m, GLTF_ATLAS_EMISSIVE,
                                                m.emissive_rect, i.uv).rgb *
                             m.emissive_packed.rgb * m.emissive_packed.a;
            }
            color = albedo + cover_glow + screen * (1.0f - albedo_tex.a);
        }
        float luma = dot(AeronLinearToSrgb(saturate(color)), float3(0.333f, 0.333f, 0.333f));
        _out.color = float4(color, saturate(4.0f * luma));
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
    if ((m.flags & GLTF_MATERIAL_HAS_NORMAL) != 0u) {
        float3 ntex = pbr_sample_channel(g_normal, g_normal_sampler, m,
                                         GLTF_ATLAS_NORMAL, m.normal_rect,
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

    float wrap_n = saturate((ndotl + light_wrap) / (1.0f + light_wrap));
    wrap_n = lerp(wrap_n, wrap_n * wrap_n, light_wrap);
    float  lambert_term = saturate(wrap_n * light_intensity);
    float3 ambient_rgb  = world_ambient(N);
    /* Kept apart from the ambient so specular image-based lighting can
     * take its Fresnel share (1 - F_ss) below. */
    float3 environment_diffuse_rgb = detailed_environment(N);
    uint shadow_cascade = 4u;
    float shadow_cascade_blend = 0.0f;
    float shadow_coverage = 0.0f;
    /* SSAO — occludes indirect (ambient) light only. Sampled in screen
     * space at this fragment; pow() applies the contrast knob, then the
     * intensity lerp blends toward unoccluded. intensity 0 → ao 1 (and
     * the sample is skipped). */
    float ao = 1.0f;
    float screen_shadow_visibility = 1.0f;
    /* Blended surfaces lie over the depth layer SSAO describes (the
     * renderer binds white AO for them): no lookup. */
    if (!AERON_PBR_ALPHA_BLEND && ssao_intensity > 0.0f) {
        float2 ao_uv = i.position.xy / float2(ssao_rt_w, ssao_rt_h);
        float2 visibility = g_ao.Sample(g_ao_sampler, ao_uv);
        float  ao_s  = visibility.x;
        screen_shadow_visibility = visibility.y;
        ao_s = pow(ao_s, ssao_power);
        ao   = lerp(1.0f, ao_s, ssao_intensity);
    }

    float shadow_visibility = 1.0f;
    if (i.receive_shadow != 0u) {
        if (!AERON_PBR_ALPHA_BLEND && i.screen_shadow != 0u) {
            shadow_visibility = screen_shadow_visibility;
        } else {
            /* The shadow scales only the sun's diffuse and specular terms.
             * Without wrap both are zero when N and N_face (and so N_spec,
             * their blend) face away from the light: skip the lookup. */
            float shadow_ndotl = light_wrap > 0.0f ? 1.0f : max(dot(N, L), dot(N_face, L));
            /* Blended materials (canopy glass) take one comparison: a
             * soft penumbra is not discernible through them. */
            bool shadow_single_tap =
                AERON_PBR_ALPHA_BLEND || (m.flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u;
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
        extra_rgb += saturate(dot(N, fs_extra_dir[ed].xyz)) * fs_extra_col[ed].rgb;
    }
    float3 base_rgb = albedo *
        ((sun_color * lambert_term * shadow_visibility + extra_rgb) * ao_direct + ambient_rgb * ao +
         i.local_rgb);

    /* ===== Metallic-roughness ==================================== */
    PbrMaterialParams mp;
    mp.spec_intensity = 1.0f;
    mp.roughness      = m.metal_rough.y;
    mp.F0             = float3(0.04f, 0.04f, 0.04f);
    mp.F90            = 1.0f;
    mp.albedo         = albedo;
    float metallic = m.metal_rough.x;
    if ((m.flags & GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS) != 0u) {
        float3 mr = pbr_sample_channel(g_mr, g_mr_sampler, m,
                                       GLTF_ATLAS_METALLIC_ROUGHNESS,
                                       m.mr_rect, i.uv).rgb;
        mp.roughness = mr.g * m.metal_rough.y;
        metallic     = mr.b * m.metal_rough.x;
    }
#if AERON_PBR_DEBUG_VIEWS
    if (fs_debug_material.x != 0.0f) {
        metallic     = fs_debug_material.y;
        mp.roughness = fs_debug_material.z;
    }
#endif
    mp.roughness = max(mp.roughness, 0.045f);
    /* KHR_materials_specular: the dielectric F0 is scaled by the colour
     * factor and both dielectric Fresnel ends by specularFactor; the
     * defaults (1, 1) give the plain metallic-roughness model. */
    float3 f0_dielectric = min(0.04f * m.specular.rgb, 1.0f) * m.specular.a;
    mp.F0  = lerp(f0_dielectric, albedo, metallic);
    mp.F90 = lerp(m.specular.a, 1.0f, metallic);
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
    if (fs_environment_params.y > 0.0f && fs_environment_params.z >= 1.0f) {
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
                     * (sun_color * mp.spec_intensity * global_spec_mul)
                     * shadow_visibility;

    /* Clustered punctual lights (laser bolts, explosions, engine
     * glows): windowed classic 1/d attenuation, Lambert diffuse + per-light
     * spec. Untouched by AO like the per-instance local lights. */
    if (fs_cluster_point_count > 0u) {
        float3 point_diff = accumulate_point_lights(N, N_spec, V, i.world_pos,
                                                    i.position.xy, mp,
                                                    global_spec_mul, spec_rgb);
        base_rgb += albedo * (1.0f - metallic) * point_diff;
    }

    float3 lit = base_rgb + spec_rgb + spec_ibl_rgb;

    if (fs_cluster_debug_view != 0u && fs_cluster_enabled != 0u) {
        _out.color = float4(clustered_light_debug_color(i.position.xy, i.world_pos), 1.0f);
        return _out;
    }

    /* ===== XvT flat-shading override ============================
     * Replace the full PBR composition with a diffuse-only flat
     * model: hard Lambert (no wrap) + hemisphere ambient, no
     * specular and no metallic darkening — the look of the
     * X-Wing vs TIE Fighter era. Emissive is skipped below too. */
    if (xvt_flat != 0.0f) {
        /* Period look (late-90s Gouraud-shaded space sim): a flat global
         * ambient floor lifts the shadowed side to a visible shade —
         * never black — with a straight clamped-Lambert directional term
         * on top, then quantised into a 16-level material ramp (the TIE/
         * XvT materialcolors[16*color - lightval] look). No HD directional
         * ambient cube, no wrap, no specular.
         *
         * The floor is a LINEAR multiplier: 0.08 linear ≈ 0.30 displayed
         * after gamma/tonemap, so the shadow side reads as ~30% rather
         * than a >50% wash. */
        const float xvt_ambient = 0.08f;   /* shadow floor (linear) */
        float ndotl = saturate(dot(N, L)) * light_intensity * shadow_visibility;
        float shade = xvt_ambient + (1.0f - xvt_ambient) * ndotl;
        shade = round(shade * 15.0f) / 15.0f;   /* 16 discrete shades */
        lit = albedo * (sun_color * shade + i.local_rgb);
    }

#if AERON_PBR_DEBUG_VIEWS
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
    else if (isolate == 9) lit = i.local_rgb;
    else if (isolate == 10) lit = environment_diffuse_term;
    else if (isolate == 11) lit = spec_ibl_rgb;
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
     * additive emissive composition. XvT mode skips both paths. */
    if (xvt_flat == 0.0f && isolate == 0 &&
        i.base_color_emissive_strength > 0.0f) {
        lit = albedo * i.base_color_emissive_strength;
    } else if (xvt_flat == 0.0f) {
        float3 emissive = m.emissive_packed.rgb;
        float emissive_coverage = 0.0f;
        if ((m.flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
            float4 emissive_tex = pbr_sample_channel(g_emissive, g_em_sampler, m,
                                                     GLTF_ATLAS_EMISSIVE,
                                                     m.emissive_rect, i.uv);
            emissive_coverage = emissive_tex.a;

            float3 emissive_rgb = emissive_tex.rgb;
            if ((m.flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
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
        emissive *= m.emissive_packed.a * emissive_mul;
        if ((m.flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
            lit = emissive + lit * (1.0f - saturate(emissive_coverage * emissive_mul));
        } else {
            lit += emissive;
        }
    }

#if AERON_PBR_SCREENS
    /* ===== Opaque screens =========================================
     * Unlit screen radiance, shown where the cover is transparent (all of
     * the surface without a cover). */
    if (m.screen_count != 0u && xvt_flat == 0.0f && isolate == 0) {
        float3 screen = screen_evaluate(m, i.uv, screen_load_frame());
        lit += screen * ((m.screen_flags & SCREEN_COVER) != 0u ? 1.0f - albedo_tex.a : 1.0f);
    }
#endif

    /* Alpha-BLEND materials (flag bit 3 — canopy glass, drawn by the
     * blend pipelines) carry texture x factor alpha; every opaque
     * material writes 1 so the scene RT keeps full coverage. */
    float alpha = ((m.flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u)
                      ? saturate(albedo_tex.a * m.base_color_factor.a)
                      : 1.0f;
    _out.color = float4(lit, alpha);
    return _out;
}
