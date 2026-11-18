/* Samples the picture of a rectangle drawn by xr_quad.vert. Its colours are
 * premultiplied by alpha, and are written as they are. */
Texture2D<float4> g_texture : register(t0, space2);
SamplerState      g_sampler : register(s0, space2);

struct VSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target {
	return g_texture.Sample(g_sampler, input.uv);
}
