/* Opaque articulated mesh depth for a directional shadow cascade. */

cbuffer DirectionalShadowVS : register(b0, space1)
{
    row_major float4x4 shadow_view_proj;
    row_major float4x4 model_to_world;
    uint mesh_table_index;
    uint3 _pad;
};

StructuredBuffer<float4> mesh_tables : register(t0, space0);
static const uint MESH_TABLE_STRIDE = 160u;
static const uint MESH_VISIBILITY_OFFSET = 120u;

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
    int mesh_index = clamp((int)round(input.mesh_index), 0, 39);
    uint table_base = mesh_table_index * MESH_TABLE_STRIDE;
    if (mesh_tables[table_base + MESH_VISIBILITY_OFFSET +
                    ((uint)mesh_index >> 2u)][(uint)mesh_index & 3u] < 0.5f) {
        output.position = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return output;
    }

    float4 r0 = mesh_tables[table_base + (uint)mesh_index * 3u + 0u];
    float4 r1 = mesh_tables[table_base + (uint)mesh_index * 3u + 1u];
    float4 r2 = mesh_tables[table_base + (uint)mesh_index * 3u + 2u];
    float3 articulated = float3(
        dot(r0.xyz, input.position) + r0.w,
        dot(r1.xyz, input.position) + r1.w,
        dot(r2.xyz, input.position) + r2.w);
    float4 world_position = mul(model_to_world, float4(articulated, 1.0f));
    output.position = mul(shadow_view_proj, world_position);
    return output;
}
