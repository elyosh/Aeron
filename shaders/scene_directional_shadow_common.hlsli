/* Shared articulated vertex projection for directional-shadow passes. */

#ifndef SCENE_DIRECTIONAL_SHADOW_COMMON_HLSLI
#define SCENE_DIRECTIONAL_SHADOW_COMMON_HLSLI

cbuffer DirectionalShadowVS : register(b0, space1)
{
    row_major float4x4 shadow_view_proj;
    row_major float4x4 model_to_world;
    uint mesh_table_base;
    uint variant_row_base;
    uint variant_group_count;
    uint material_count;
    uint table_slot_count;
    uint3 _pad;
};

#include "mesh_table_layout.hlsli"

StructuredBuffer<float4> mesh_tables : register(t0, space0);

bool directional_shadow_project_vertex(float3 position, float mesh_index_value,
                                       out float4 projected_position)
{
    int mesh_index = clamp((int)round(mesh_index_value), 0,
                           (int)table_slot_count - 1);
    uint table_base = mesh_table_base;
    if (mesh_tables[table_base + 3u * table_slot_count +
                    AERON_MESH_LANE_VISIBILITY * AERON_MESH_LANE_VEC4(table_slot_count) +
                    ((uint)mesh_index >> 2u)][(uint)mesh_index & 3u] < 0.5f) {
        projected_position = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return false;
    }

    float4 r0 = mesh_tables[table_base + (uint)mesh_index * 3u + 0u];
    float4 r1 = mesh_tables[table_base + (uint)mesh_index * 3u + 1u];
    float4 r2 = mesh_tables[table_base + (uint)mesh_index * 3u + 2u];
    float3 articulated = float3(
        dot(r0.xyz, position) + r0.w,
        dot(r1.xyz, position) + r1.w,
        dot(r2.xyz, position) + r2.w);
    float4 world_position = mul(model_to_world, float4(articulated, 1.0f));
    projected_position = mul(shadow_view_proj, world_position);
    return true;
}

#endif /* SCENE_DIRECTIONAL_SHADOW_COMMON_HLSLI */
