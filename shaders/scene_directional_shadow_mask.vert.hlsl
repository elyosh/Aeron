/* Alpha-mask articulated mesh depth for a directional shadow cascade. */

#include "scene_directional_shadow_common.hlsli"

struct VSIn
{
    float3 position   : POSITION;
    float2 uv         : TEXCOORD0;
    float  mesh_index : COLOR0;
    uint   prim_id    : COLOR1;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint4 material_lookup : TEXCOORD1;
};

VSOut main(VSIn input)
{
    VSOut output;
    if (!directional_shadow_project_vertex(input.position, input.mesh_index,
                                           output.position)) {
        output.uv = float2(0.0f, 0.0f);
        output.material_lookup = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
        return output;
    }
    output.uv = input.uv;
    output.material_lookup = uint4(input.prim_id, variant_row_base,
                                   variant_group_count, material_count);
    return output;
}
