/* Alpha-tested velocity-stamp vertex shader for glTF blend ranges. */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrStampVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrStampVSOut output;
	if (!pbr_build_current_vertex(input, current)) {
		output.position = current.raster_position;
		output.uv = float2(0.0f, 0.0f);
		output.material_lookup = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
		output.clip_curr = current.raster_position;
		output.clip_prev = current.raster_position;
		return output;
	}

	output.position = current.raster_position;
	output.uv = input.uv;
	output.material_lookup =
		uint4(input.prim_id, variant_row_base, variant_group_count, material_count);
	pbr_build_velocity(input, current, output.clip_curr, output.clip_prev);
	return output;
}
