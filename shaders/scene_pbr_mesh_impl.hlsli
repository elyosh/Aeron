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
 *   b2 space3 — ClusteredLightUniform
 *   t7 space2 — material storage (after seven sampled textures)
 *   t8 space2 — packed variant-map storage
 *   t9 space2 — scene point-light storage
 *   t10 space2 — cluster headers
 *   t11 space2 — cluster light indices
 *
 * Texture / sampler slots:
 *   t0/s0 — base_color atlas    (sRGB)
 *   t1/s1 — normal atlas        (UNORM)
 *   t2/s2 — metallic_rough atlas (UNORM)
 *   t3/s3 — emissive atlas      (sRGB)
 *   t4/s4 — SSAO
 *   t5/s5 — comparison shadow depth
 *   t6/s6 — raw shadow depth for PCSS blocker search
 */

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#include "scene_pbr_lighting.hlsli"
#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "srgb.hlsli"

/* Per-material entry. Layout mirrors host GltfMaterialEntry (128 B).
 * Sub-rect zw == 0 means "channel absent for this material" → factor
 * fallback. */
struct GltfMaterial
{
    float4 base_rect;
    float4 normal_rect;
    float4 mr_rect;
    float4 emissive_rect;
    float4 base_color_factor;
    float4 emissive_packed;    /* .rgb = factor, .a = emissive_strength */
    float4 metal_rough;        /* .x = metallic, .y = roughness */
    uint   flags;              /* mirrors sub-rect presence for fast gate */
    uint3  _pad;
};

StructuredBuffer<GltfMaterial> g_materials : register(t7, space2);
StructuredBuffer<uint4> g_prim_to_material : register(t8, space2);

static const uint GLTF_NO_MATERIAL = 0xFFFFFFFFu;

Texture2D    g_base_color   : register(t0, space2);
SamplerState g_base_sampler : register(s0, space2);
Texture2D    g_normal       : register(t1, space2);
SamplerState g_normal_sampler : register(s1, space2);
Texture2D    g_mr           : register(t2, space2);
SamplerState g_mr_sampler   : register(s2, space2);
Texture2D    g_emissive     : register(t3, space2);
SamplerState g_em_sampler   : register(s3, space2);
/* Half-res RG8 visibility: R = SSAO, G = denoised main directional
 * shadow. A 1×1 white placeholder keeps both effects unoccluded when the
 * joint visibility pass is unavailable. */
Texture2D<float2> g_ao      : register(t4, space2);
SamplerState g_ao_sampler   : register(s4, space2);


struct FSOut
{
    float4 color : SV_Target0;
};

FSOut main(PbrForwardVSOut i, bool is_front : SV_IsFrontFace)
{
    FSOut _out;

    /* ===== Material resolution =================================== */
    uint mat_idx;
    {
        uint slot = i.prim_id >> 2u;
        uint comp = i.prim_id & 3u;
        uint4 packed = slot < i.variant_group_count
            ? g_prim_to_material[i.variant_row_base + slot]
            : uint4(GLTF_NO_MATERIAL, GLTF_NO_MATERIAL, GLTF_NO_MATERIAL, GLTF_NO_MATERIAL);
        mat_idx =
              (comp == 0u) ? packed.x
            : (comp == 1u) ? packed.y
            : (comp == 2u) ? packed.z
            :                packed.w;
    }
    bool has_mat = (mat_idx != GLTF_NO_MATERIAL) && (mat_idx < i.material_count);

    GltfMaterial m;
    if (has_mat) {
        m = g_materials[mat_idx];
    } else {
        /* Fully-default material — base_color (1,1,1,1), no texture
         * channels. The whole fragment falls back to factor-only. */
        m.base_rect          = float4(0,0,0,0);
        m.normal_rect        = float4(0,0,0,0);
        m.mr_rect            = float4(0,0,0,0);
        m.emissive_rect      = float4(0,0,0,0);
        m.base_color_factor  = float4(1,1,1,1);
        m.emissive_packed    = float4(0,0,0,1);
        m.metal_rough        = float4(0,1,0,0);
        m.flags              = 0u;
    }

    /* ===== Base color ============================================ */
    float4 albedo_tex;
    if (m.base_rect.z > 0.0f && m.base_rect.w > 0.0f) {
        albedo_tex = atlas_sample(g_base_color, g_base_sampler,
                                  i.uv, m.base_rect);
    } else {
        albedo_tex = float4(1, 1, 1, 1);
    }
    float3 albedo = albedo_tex.rgb * m.base_color_factor.rgb;

    /* ===== Normal vector ========================================= */
    float  side_sign = is_front ? 1.0f : -1.0f;
    float3 N_geom = normalize(i.world_normal) * side_sign;
    float3 world_pos_dx = ddx(i.world_pos);
    float3 world_pos_dy = ddy(i.world_pos);
    float3 N_face = N_geom;
#if AERON_PBR_DEBUG_VIEWS
    if (shadow_camera_pos.w != 0.0f || spec_geom_adapt != 0.0f) {
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
    if ((m.flags & 0x1u) != 0u) {
        float3 ntex = atlas_sample(g_normal, g_normal_sampler,
                                   i.uv, m.normal_rect).rgb * 2.0f - 1.0f;
        float  nz   = sqrt(saturate(1.0f - dot(ntex.xy, ntex.xy)));
        float3 nt   = float3(ntex.xy, max(ntex.z, nz));
        float3 T    = normalize(i.world_tangent);
        float3 B    = normalize(cross(N_geom, T)) * i.tangent_sign;
        N = normalize(T * nt.x + B * nt.y + N_geom * nt.z);
    }

    /* ===== View + light ========================================== */
    float3 V = normalize(fs_camera_pos_world - i.world_pos);
    float3 L = fs_directional_dir;
    float  ndotl = saturate(dot(N, L));

    float wrap_n = saturate((ndotl + light_wrap) / (1.0f + light_wrap));
    wrap_n = lerp(wrap_n, wrap_n * wrap_n, light_wrap);
    float  lambert_term = saturate(wrap_n * light_intensity);
    float3 ambient_rgb  = world_ambient(N);
    uint shadow_cascade = 4u;
    float shadow_cascade_blend = 0.0f;
    float shadow_coverage = 0.0f;
    float3 shadow_bias_normal = shadow_camera_pos.w != 0.0f ? N_face : N_geom;
    float shadow_bias_ndotl = saturate(dot(N_face, L));
    /* SSAO — occludes indirect (ambient) light only. Sampled in screen
     * space at this fragment; pow() applies the contrast knob, then the
     * intensity lerp blends toward unoccluded. intensity 0 → ao 1 (and
     * the sample is skipped). */
    float ao = 1.0f;
    float screen_shadow_visibility = 1.0f;
    if (ssao_intensity > 0.0f) {
        float2 ao_uv = i.position.xy / float2(ssao_rt_w, ssao_rt_h);
        float2 visibility = g_ao.Sample(g_ao_sampler, ao_uv);
        float  ao_s  = visibility.x;
        screen_shadow_visibility = visibility.y;
        ao_s = pow(ao_s, ssao_power);
        ao   = lerp(1.0f, ao_s, ssao_intensity);
    }

    float shadow_visibility = 1.0f;
    if (i.receive_shadow != 0u) {
        if (i.screen_shadow != 0u) {
            shadow_visibility = screen_shadow_visibility;
        } else {
            shadow_visibility = directional_shadow_visibility(
                i.world_pos, shadow_bias_normal, ndotl, shadow_bias_ndotl,
                world_pos_dx, world_pos_dy, i.position.xy, shadow_cascade,
                shadow_cascade_blend, shadow_coverage);
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
    mp.albedo         = albedo;
    float metallic = m.metal_rough.x;
    if ((m.flags & 0x2u) != 0u) {
        float3 mr = atlas_sample(g_mr, g_mr_sampler, i.uv, m.mr_rect).rgb;
        mp.roughness = mr.g * m.metal_rough.y;
        metallic     = mr.b * m.metal_rough.x;
    }
    mp.roughness = max(mp.roughness, 0.045f);
    mp.F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
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

    float3 lit = base_rgb + spec_rgb;

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
        if ((m.flags & 0x4u) != 0u) {
            float4 emissive_tex = atlas_sample(g_emissive, g_em_sampler,
                                               i.uv, m.emissive_rect);
            emissive_coverage = emissive_tex.a;

            float3 emissive_rgb = emissive_tex.rgb;
            if ((m.flags & 0x10u) != 0u) {
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
        if ((m.flags & 0x10u) != 0u) {
            lit = emissive + lit * (1.0f - saturate(emissive_coverage * emissive_mul));
        } else {
            lit += emissive;
        }
    }

    /* Alpha-BLEND materials (flag bit 3 — canopy glass, drawn by the
     * blend pipelines) carry texture x factor alpha; every opaque
     * material writes 1 so the scene RT keeps full coverage. */
    float alpha = ((m.flags & 0x8u) != 0u)
                      ? saturate(albedo_tex.a * m.base_color_factor.a)
                      : 1.0f;
    _out.color = float4(lit, alpha);
    return _out;
}
