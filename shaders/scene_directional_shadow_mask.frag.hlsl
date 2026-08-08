/* Alpha-tested directional-shadow depth. */

#include "scene_pbr_atlas_sample.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t1
#define AERON_PBR_VARIANT_REGISTER t2
#include "scene_pbr_material_alpha.hlsli"

struct FSIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint4 material_lookup : TEXCOORD1;
};

void main(FSIn input)
{
    uint4 lookup = input.material_lookup;
    GltfMaterial material = pbr_resolve_material(
        lookup.x, lookup.y, lookup.z, lookup.w);
    pbr_apply_alpha_mask(pbr_base_alpha(material, input.uv),
                         material.metal_rough.z);
}
