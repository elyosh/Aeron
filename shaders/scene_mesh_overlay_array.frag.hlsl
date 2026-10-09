/* scene_mesh_overlay.frag for an image in a 2D array texture (an effect
 * atlas): layer layer.x, sampling mips up to layer.y, as atlas_sample
 * limits them for images packed with neighbours. With layer.z, UVs outside
 * uv_rect sample its edge texels (D3D CLAMP) instead of being discarded. */
Texture2DArray<float4> g_texture : register(t0, space2);
SamplerState           s_texture : register(s0, space2);
cbuffer                OverlayFS : register(b0, space3) {
	float4 uv_xform;
	float4 uv_rect;
	float4 color;
	float4 layer; /* x: array layer, y: max mip level, z: clamp */
};
struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};
float4 main(VSOut input) : SV_Target {
	float2 atlas_uv = input.uv * uv_xform.zw + uv_xform.xy;
	/* Gradients before the discard, which makes the rest non-uniform. */
	float2 dx = ddx(atlas_uv);
	float2 dy = ddy(atlas_uv);
	if (layer.z == 0.0f && (any(atlas_uv < uv_rect.xy) || any(atlas_uv > uv_rect.zw))) {
		discard;
	}
	uint width, height, layers, levels;
	g_texture.GetDimensions(0, width, height, layers, levels);
	float2 size   = float2(width, height);
	float2 tdx    = dx * size;
	float2 tdy    = dy * size;
	float  lod    = 0.5f * log2(max(max(dot(tdx, tdx), dot(tdy, tdy)), 1e-20f));
	float  shrink = exp2(min(layer.y - lod, 0.0f));
	atlas_uv      = clamp(atlas_uv, uv_rect.xy, uv_rect.zw);
	float4 sampled_color =
		g_texture.SampleGrad(s_texture, float3(atlas_uv, layer.x), dx * shrink, dy * shrink) * color;
	if (sampled_color.a < 0.01f) {
		discard;
	}
	return sampled_color;
}
