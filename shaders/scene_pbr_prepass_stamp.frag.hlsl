/*
 * Velocity STAMP fragment shader for alpha-BLEND mesh ranges.
 *
 * Blend-classified geometry (soft-alpha projectile ribbons) is excluded
 * from the depth/normal prepass — it writes no depth — so the regular
 * velocity path never covers it and the motion-blur resolve leaves it
 * sharp. This pass stamps the velocity buffer for the geometry's
 * VISIBLE texels only (alpha-tested against the base-color atlas),
 * depth-tested GE against the laid opaque depth with NO depth write —
 * the mesh-instance analog of the batched billboards' velocity
 * stamping (scene_billboard3d_vel).
 *
 * The paired stamp VS supplies clip_curr/clip_prev from the instance's
 * previous transforms. The normal RT rides along write-masked (2-RT
 * velocity-prepass layout); only SV_Target1 lands.
 *
 * t0/s0 is the base-color atlas. Material and variant-map storage follow
 * it at t1 and t2.
 */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"

struct GltfMaterial
{
    float4 base_rect;
    float4 normal_rect;
    float4 mr_rect;
    float4 emissive_rect;
    float4 base_color_factor;
    float4 emissive_packed;
    float4 metal_rough;
    uint   flags;
    uint3  _pad;
};

StructuredBuffer<GltfMaterial> g_materials : register(t1, space2);
StructuredBuffer<uint4> g_prim_to_material : register(t2, space2);

static const uint  GLTF_NO_MATERIAL = 0xFFFFFFFFu;
/* Texels below this alpha are not visible enough to deserve motion
 * vectors; stamping them would smear the background behind the
 * ribbon's transparent margin. */
static const float STAMP_ALPHA_CUTOFF = 0.5f;

Texture2D    g_base_color   : register(t0, space2);
SamplerState g_base_sampler : register(s0, space2);


struct FSOut
{
    float2 normal   : SV_Target0; /* write-masked by the pipeline */
    float2 velocity : SV_Target1;
};

FSOut main(PbrStampVSOut i)
{
    uint prim_id = i.material_lookup.x;
    uint variant_row_base = i.material_lookup.y;
    uint variant_group_count = i.material_lookup.z;
    uint material_count = i.material_lookup.w;
    uint mat_idx;
    {
        uint slot = prim_id >> 2u;
        uint comp = prim_id & 3u;
        uint4 packed = slot < variant_group_count
            ? g_prim_to_material[variant_row_base + slot]
            : uint4(GLTF_NO_MATERIAL, GLTF_NO_MATERIAL, GLTF_NO_MATERIAL, GLTF_NO_MATERIAL);
        mat_idx =
              (comp == 0u) ? packed.x
            : (comp == 1u) ? packed.y
            : (comp == 2u) ? packed.z
            :                packed.w;
    }

    float alpha = 1.0f;
    if (mat_idx != GLTF_NO_MATERIAL && mat_idx < material_count) {
        GltfMaterial m = g_materials[mat_idx];
        alpha          = m.base_color_factor.a;
        if (m.base_rect.z > 0.0f && m.base_rect.w > 0.0f) {
            alpha *= atlas_sample(g_base_color, g_base_sampler,
                                  i.uv, m.base_rect).a;
        }
    }
    if (alpha < STAMP_ALPHA_CUTOFF) {
        discard;
    }

    FSOut o;
    o.normal = float2(0.0f, 0.0f);

    float2 ndc_now  = i.clip_curr.xy / i.clip_curr.w;
    float2 ndc_prev = i.clip_prev.xy / i.clip_prev.w;
    o.velocity = (ndc_now - ndc_prev) * float2(0.5f, -0.5f);
    return o;
}
