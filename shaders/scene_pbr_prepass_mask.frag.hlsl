/* Alpha-mask depth/normal/velocity prepass fragment shader. */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_prepass_common.hlsli"
#include "scene_pbr_vsout.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t1
#define AERON_PBR_VARIANT_REGISTER t2
#include "scene_pbr_material_alpha.hlsli"

PbrPrepassFSOut main(PbrMaskPrepassVSOut i)
{
    uint4 lookup = i.material_lookup;
    GltfMaterial material = pbr_resolve_material(
        lookup.x, lookup.y, lookup.z, lookup.w);
    pbr_apply_alpha_mask(pbr_base_alpha(material, i.uv),
                         material.metal_rough.z);

    return pbr_build_prepass_output(i.position, i.world_pos, i.world_normal,
                                    i.clip_curr, i.clip_prev);
}
