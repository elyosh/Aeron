/* Opaque articulated mesh depth for a directional shadow cascade. */

#include "scene_directional_shadow_common.hlsli"

struct VSIn
{
    float3 position   : POSITION;
    float  mesh_index : COLOR0;
};

struct VSOut
{
    float4 position : SV_Position;
};

VSOut main(VSIn input)
{
    VSOut output;
    directional_shadow_project_vertex(input.position, input.mesh_index,
                                      output.position);
    return output;
}
