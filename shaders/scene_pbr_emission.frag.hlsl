/*
 * Fragment shader for batches of emission_only instances
 * (AeronSceneMeshInstance.emission_only), such as laser bolts. It outputs
 * the material's emission and alpha, computed as in
 * scene_pbr_mesh_impl.hlsli for a blended material, and skips all lighting.
 *
 * Bindings: the base colour atlas at sampler slot 0, the emissive atlas at
 * slot 1 (the scene binds it there for these batches), and the material and
 * variant storage at slots 0 and 1. The emissive scale is already in
 * emissive_mul, from the vertex shader.
 */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t2
#define AERON_PBR_VARIANT_REGISTER t3
#include "scene_pbr_material_alpha.hlsli"

Texture2DArray g_emissive : register(t1, space2);
SamplerState g_em_sampler : register(s1, space2);

struct FSOut
{
    float4 color : SV_Target0;
};

FSOut main(PbrEmissionVSOut i)
{
    FSOut o;
    const uint mat = pbr_material_slot(i.material_lookup.x, i.material_lookup.y,
                                       i.material_lookup.z, i.material_lookup.w);
    /* Sample outside the branch: sampling uses the UV derivatives, which
     * aren't reliable inside a branch. */
    const float base_alpha = pbr_sample_base_color(g_materials[mat], i.uv).a;
    float alpha = 1.0f;
    if ((g_materials[mat].flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u) {
        alpha = saturate(base_alpha * g_materials[mat].base_color_factor.a);
        /* With the SRC_ALPHA blend, alpha 0 leaves the target unchanged. */
        if (alpha <= 0.0f) {
            o.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return o;
        }
    }
    float3 emissive = g_materials[mat].emissive_packed.rgb;
    if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
        emissive *= pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat], GLTF_ATLAS_EMISSIVE,
                                       g_materials[mat].emissive_rect, i.uv).rgb;
    }
    o.color = float4(emissive * (g_materials[mat].emissive_packed.a * i.emissive_mul), alpha);
    return o;
}
