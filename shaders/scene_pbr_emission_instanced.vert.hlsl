/* Vertex shader for batches of emission_only instances
 * (AeronSceneMeshInstance.emission_only). */

#define AERON_PBR_INSTANCED 1
#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

#include "scene_pbr_emission_widen.hlsli"

PbrEmissionVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrEmissionVSOut output;
	const float width_factor = pbr_emission_widen(input);
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
