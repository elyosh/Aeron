/* The depth of a mask range at the start of the MSAA colour pass. */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrMsaaDepthMaskVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrMsaaDepthMaskVSOut output;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.uv = float2(0.0f, 0.0f);
		output.material_lookup = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
		return output;
	}
	output.position = current.raster_position;
	output.uv = input.uv;
	output.material_lookup =
		uint4(input.prim_id, variant_row_base, variant_group_count, material_count);
	return output;
}
