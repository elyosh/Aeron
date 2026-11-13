/* Vertex shader for the bloom draw of emission_only instances
 * (AeronScene_DrawEmissionBloom). It places them as
 * scene_pbr_emission_instanced.vert does; emissive_scale already holds
 * their extra bloom light. It also passes where each vertex lies across
 * the instance's width, which the fragment shader needs for its weight
 * across the width. */

#define AERON_PBR_INSTANCED 1
#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

#include "scene_pbr_emission_widen.hlsli"

PbrEmissionBloomVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrEmissionBloomVSOut output;
	output.depth_map = emission_depth_map;
	const float3 position = input.position;
	float3       width_axis;
	float        half_extent;
	const float  width_factor = pbr_emission_widen_ex(input, width_axis, half_extent);
	output.across = half_extent > 0.0f ? dot(position - emission_center, width_axis) / half_extent : 0.0f;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.uv = float2(0.0f, 0.0f);
		output.emissive_mul = 0.0f;
		output.material_lookup = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
		return output;
	}
	output.position = current.raster_position;
	output.uv = input.uv;
	output.emissive_mul =
		mesh_table_lane(AERON_MESH_LANE_EMISSIVE, current.component) * emissive_scale * width_factor;
	output.material_lookup = uint4(input.prim_id, variant_row_base, variant_group_count, material_count);
	return output;
}
