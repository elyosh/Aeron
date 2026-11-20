/* The fragment shader of the projected overlays (mesh_overlay.h), for
 * PROJECTION_COUNT images of one texture, 1 to 4. Each image is drawn as
 * scene_mesh_overlay.frag draws it, and the images blend one over the other
 * in order, with premultiplied alpha. The result is the same as one overlay
 * for each image, but the mesh is only drawn once.
 *
 * There is one shader for each count, so that each does no more work than
 * its images need: scene_mesh_projection_1.frag to _4.frag. */
#include "scene_mesh_projection.hlsli"
Texture2D<float4> g_texture : register(t0, space2);
SamplerState      s_texture : register(s0, space2);
cbuffer ProjectionFS : register(b0, space3) {
	uint4      projection_count; /* x */
	Projection projections[PROJECTION_MAX];
};
struct VSOut {
	float4 pos : SV_Position;
	float4 uv01 : TEXCOORD0;
#if PROJECTION_COUNT > 2
	float4 uv23 : TEXCOORD1;
#endif
};
/* Draws image `index` over what the images before it added. The image is
 * sampled for every pixel, before the tests, so that the GPU can work out
 * its mip level. */
void add_image(uint index, float2 uv, inout float3 added, inout float kept, inout bool drawn) {
	float4 sampled = g_texture.Sample(s_texture, uv) * projections[index].color;
	bool   shown   = all(uv >= projections[index].uv_rect.xy) && all(uv <= projections[index].uv_rect.zw) &&
					 sampled.a >= 0.01f;
	sampled = shown ? sampled : float4(0.0f, 0.0f, 0.0f, 0.0f);
	added   = sampled.rgb + added * (1.0f - sampled.a);
	kept *= 1.0f - sampled.a;
	drawn = drawn || shown;
}
float4 main(VSOut input) : SV_Target {
	/* What the images drawn so far add to the pixel, and how much of the
	 * pixel's own colour they leave. */
	float3 added = float3(0.0f, 0.0f, 0.0f);
	float  kept  = 1.0f;
	bool   drawn = false;
	add_image(0u, input.uv01.xy, added, kept, drawn);
#if PROJECTION_COUNT > 1
	add_image(1u, input.uv01.zw, added, kept, drawn);
#endif
#if PROJECTION_COUNT > 2
	add_image(2u, input.uv23.xy, added, kept, drawn);
#endif
#if PROJECTION_COUNT > 3
	add_image(3u, input.uv23.zw, added, kept, drawn);
#endif
	if (!drawn) {
		discard;
	}
	return float4(added, 1.0f - kept);
}
