/*
 * Forward vertex shader for the glTF mesh path.
 *
 * Vertex stream is AeronGltfVertex (56 B). Per-vertex component selects an
 * articulated component's mesh-table entry; prim_id selects material/variant data in the
 * fragment shader.
 */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrForwardVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrForwardVSOut output;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.world_pos = float3(0.0f, 0.0f, 0.0f);
		output.world_normal = float3(0.0f, 0.0f, 1.0f);
		output.world_tangent = float3(1.0f, 0.0f, 0.0f);
		output.tangent_sign = 1.0f;
		output.uv = float2(0.0f, 0.0f);
		output.emissive_mul = 1.0f;
		output.prim_id = 0xFFFFFFFFu;
		output.base_color_emissive_strength = 0.0f;
		output.receive_shadow = 0u;
		output.screen_shadow = 0u;
		output.variant_row_base = 0u;
		output.variant_group_count = 0u;
		output.material_count = 0u;
#if AERON_PBR_ANIM
		output.model_pos = float3(0.0f, 0.0f, 0.0f);
		output.model_normal = float3(0.0f, 0.0f, 1.0f);
#endif
		return output;
	}

	float3 rotated_normal;
	rotated_normal.x = dot(current.row0.xyz, input.normal);
	rotated_normal.y = dot(current.row1.xyz, input.normal);
	rotated_normal.z = dot(current.row2.xyz, input.normal);

	float3 rotated_tangent;
	rotated_tangent.x = dot(current.row0.xyz, input.tangent.xyz);
	rotated_tangent.y = dot(current.row1.xyz, input.tangent.xyz);
	rotated_tangent.z = dot(current.row2.xyz, input.tangent.xyz);

	float3 world_normal =
		pbr_safe_normalize(mul((float3x3)craft_to_world, rotated_normal),
						   float3(0.0f, 0.0f, 1.0f));
	float3 world_tangent =
		pbr_safe_normalize(mul((float3x3)craft_to_world, rotated_tangent),
						   float3(1.0f, 0.0f, 0.0f));

	uint component = current.component;
	output.position = current.raster_position;
	output.world_pos = current.world_position.xyz;
	output.world_normal = world_normal;
	output.world_tangent = world_tangent;
	output.tangent_sign = input.tangent.w;
	output.uv = input.uv;
	output.emissive_mul = mesh_table_lane(AERON_MESH_LANE_EMISSIVE, component);
	output.prim_id = input.prim_id;
	output.base_color_emissive_strength = base_color_emissive_strength;
	output.receive_shadow = receive_shadow;
	output.screen_shadow = screen_shadow;
	output.variant_row_base = variant_row_base;
	output.variant_group_count = variant_group_count;
	output.material_count = material_count;
#if AERON_PBR_ANIM
	output.model_pos = input.position;
	output.model_normal = input.normal;
#endif
	return output;
}
