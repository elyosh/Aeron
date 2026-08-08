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
#include "scene_pbr_prepass_common.hlsli"
#include "scene_pbr_vsout.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t1
#define AERON_PBR_VARIANT_REGISTER t2
#include "scene_pbr_material_alpha.hlsli"

/* Texels below this alpha are not visible enough to deserve motion
 * vectors; stamping them would smear the background behind the
 * ribbon's transparent margin. */
static const float STAMP_ALPHA_CUTOFF = 0.5f;

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
    GltfMaterial material = pbr_resolve_material(
        prim_id, variant_row_base, variant_group_count, material_count);
    pbr_apply_alpha_mask(pbr_base_alpha(material, i.uv),
                         STAMP_ALPHA_CUTOFF);

    FSOut o;
    o.normal = float2(0.0f, 0.0f);

    o.velocity = pbr_prepass_velocity(i.clip_curr, i.clip_prev);
    return o;
}
