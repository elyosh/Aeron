/* Velocity-stamp vertex shader for batches of emission_only instances. It
 * widens them as their colour shader does (scene_pbr_emission_widen.hlsli),
 * so their motion vectors cover the strip that is drawn. It pairs with
 * scene_pbr_prepass_stamp.frag. */

#define AERON_PBR_INSTANCED 1
#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "scene_pbr_emission_widen.hlsli"

PbrStampVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	PbrStampVSOut output;
	pbr_emission_widen(input);
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
