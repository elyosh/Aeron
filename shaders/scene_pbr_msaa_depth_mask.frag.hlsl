/* The depth of a mask range at the start of the MSAA colour pass, with the
 * same alpha test as scene_pbr_prepass_mask.frag.hlsl. */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t1
#define AERON_PBR_VARIANT_REGISTER t2
#include "scene_pbr_material_alpha.hlsli"

float4 main(PbrMsaaDepthMaskVSOut i) : SV_Target0
{
    uint4 lookup = i.material_lookup;
    GltfMaterial material = pbr_resolve_material(
        lookup.x, lookup.y, lookup.z, lookup.w);
    pbr_apply_alpha_mask(pbr_base_alpha(material, i.uv),
                         material.metal_rough.z);
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
