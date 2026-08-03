Texture2D<float4> g_texture : register(t0, space2);
SamplerState      s_texture : register(s0, space2);
cbuffer           OverlayFS : register(b0, space3) {
	float4 uv_xform;
	float4 uv_rect;
	float4 color;
};
struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};
float4 main(VSOut input) : SV_Target {
	float2 atlas_uv = input.uv * uv_xform.zw + uv_xform.xy;
	if (any(atlas_uv < uv_rect.xy) || any(atlas_uv > uv_rect.zw)) {
		discard;
	}
	atlas_uv             = clamp(atlas_uv, uv_rect.xy, uv_rect.zw);
	float4 sampled_color = g_texture.Sample(s_texture, atlas_uv) * color;
	if (sampled_color.a < 0.01f) {
		discard;
	}
	return sampled_color;
}
