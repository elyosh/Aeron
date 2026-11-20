/* Fragment shader for the projected overlays (see mesh_overlay.h). It draws
 * PROJECTION_COUNT images from one texture, where PROJECTION_COUNT is 1 to 4.
 *
 * Each image is drawn the same way as in scene_mesh_overlay.frag. The images
 * are blended in order with premultiplied alpha. The result is the same as
 * drawing one overlay per image, but the mesh is only drawn once.
 *
 * There is one compiled shader per count (scene_mesh_projection_1.frag to
 * scene_mesh_projection_4.frag), so a shader never does work for images it
 * doesn't have. */
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
/* Blends image `index` over the images before it. The texture is sampled
 * for every pixel, before the tests, because the GPU needs that to pick the
 * mip level. */
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
	/* `added` is the colour the images add to the pixel so far. `kept` is
	 * the share of the pixel's own colour that remains. */
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
