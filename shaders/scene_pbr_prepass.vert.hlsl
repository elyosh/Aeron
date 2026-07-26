/* Depth/normal/velocity prepass vertex shader for glTF meshes. */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrPrepassVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrPrepassVSOut output;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.world_pos = float3(0.0f, 0.0f, 0.0f);
		output.world_normal = float3(0.0f, 0.0f, 1.0f);
		output.clip_curr = current.raster_position;
		output.clip_prev = current.raster_position;
		return output;
	}

	float3 rotated_normal;
	rotated_normal.x = dot(current.row0.xyz, input.normal);
	rotated_normal.y = dot(current.row1.xyz, input.normal);
	rotated_normal.z = dot(current.row2.xyz, input.normal);

	output.position = current.raster_position;
	output.world_pos = current.world_position.xyz;
	output.world_normal =
		pbr_safe_normalize(mul((float3x3)craft_to_world, rotated_normal),
						   float3(0.0f, 0.0f, 1.0f));
	pbr_build_velocity(input, current, output.clip_curr, output.clip_prev);
	return output;
}
