/* Vertex shader for batches of emission_only instances
 * (AeronSceneMeshInstance.emission_only). */

#define AERON_PBR_INSTANCED 1
#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

/* Mirrors AeronScenePbrEmissionVsBlock. The emissive scale is passed here
 * instead of in the fragment shader's cluster uniform. That uniform is at
 * slot 2, and on Metal a fragment shader that declares only that uniform
 * gets its storage buffers at the wrong indices. */
cbuffer PbrEmissionVSUniforms : register(b1, space1) {
	/* AeronScene_EmissiveScale; the other shaders read it as fs_emission.x. */
	float emissive_scale;
	float3 _pad;
};

PbrEmissionVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrEmissionVSOut output;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.uv = float2(0.0f, 0.0f);
		output.emissive_mul = 0.0f;
		output.material_lookup = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
		return output;
	}
	output.position = current.raster_position;
	output.uv = input.uv;
	output.emissive_mul = mesh_table_lane(AERON_MESH_LANE_EMISSIVE, current.component) * emissive_scale;
	output.material_lookup = uint4(input.prim_id, variant_row_base, variant_group_count, material_count);
	return output;
}
