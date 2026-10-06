/*
 * Back-face fill vertex shader: the shared precise raster position only,
 * so the fill costs no more than the transform (no normal or velocity).
 */

#include "scene_pbr_vertex_common.hlsli"

float4 main(VSIn input) : SV_Position
{
	PbrCurrentVertex current;
	pbr_build_current_vertex(input, current);
	return current.raster_position;
}
