/*
 * Forward vertex shader for the glTF mesh path.
 *
 * Vertex stream is AeronGltfVertex (56 B). Per-vertex mesh_index selects an
 * articulated mesh-table slot; prim_id selects material/variant data in the
 * fragment shader.
 */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_local_lights.hlsli"
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
		output.local_rgb = float3(0.0f, 0.0f, 0.0f);
		output.prim_id = 0xFFFFFFFFu;
		output.base_color_emissive_strength = 0.0f;
		output.receive_shadow = 0u;
		output.screen_shadow = 0u;
		output.variant_row_base = 0u;
		output.variant_group_count = 0u;
		output.material_count = 0u;
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

	/* craft_to_world is rotation × uniform scale, so transpose(R)/scale²
	 * maps world deltas back to raw instance-local engine units. */
	float3 local_rgb = float3(0.0f, 0.0f, 0.0f);
	if (local_light_count > 0u) {
		float3x3 rotation_scale = (float3x3)craft_to_world;
		float scale_squared = dot(rotation_scale[0], rotation_scale[0]);
		float3x3 world_to_local = transpose(rotation_scale) / scale_squared;
		float3 craft_position =
			float3(craft_to_world[0][3], craft_to_world[1][3], craft_to_world[2][3]);
		local_rgb = accumulate_local_lights(current.rotated_local, rotated_normal,
											world_to_local, craft_position);
	}

	uint mesh_index = current.mesh_index;
	output.position = current.raster_position;
	output.world_pos = current.world_position.xyz;
	output.world_normal = world_normal;
	output.world_tangent = world_tangent;
	output.tangent_sign = input.tangent.w;
	output.uv = input.uv;
	output.emissive_mul =
		mesh_table_load(current_table_index,
						MESH_EMISSIVE_OFFSET + (mesh_index >> 2u))[mesh_index & 3u];
	output.local_rgb = local_rgb;
	output.prim_id = input.prim_id;
	output.base_color_emissive_strength = base_color_emissive_strength;
	output.receive_shadow = receive_shadow;
	output.screen_shadow = screen_shadow;
	output.variant_row_base = variant_row_base;
	output.variant_group_count = variant_group_count;
	output.material_count = material_count;
	return output;
}
