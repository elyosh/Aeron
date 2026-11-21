/* The depth of an opaque range at the start of the MSAA colour pass
 * (AeronScene3DDesc.msaa_depth_prepass). */

#include "scene_pbr_vertex_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrMsaaDepthVSOut main(VSIn input)
{
	PbrCurrentVertex current;
	pbr_build_current_vertex(input, current);
	PbrMsaaDepthVSOut output;
	output.position = current.raster_position;
	return output;
}
