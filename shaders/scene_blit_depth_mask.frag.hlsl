/*
 * scene_blit.frag with a depth mask (AeronDrawList_SetDepthMask).
 *
 * Hides the pixel where a reversed-Z depth texture is nearer than a value.
 * The depth's UV comes from the pixel's position in the list's target, so
 * the vertex shaders don't change. One texel is read, with a nearest
 * sampler.
 *
 * This shader must read every field of VSOut. The DXIL compiler drops
 * inputs that a pixel shader doesn't read and packs the others into the
 * free registers. The input registers then no longer match the vertex
 * shader's outputs, and Direct3D 12 refuses to create the pipeline
 * (E_INVALIDARG). Metal and Vulkan match inputs by name or location, so
 * they don't show the problem.
 */

#include "srgb.hlsli"

Texture2D<float4> g_tex : register(t0, space2);
SamplerState      g_smp : register(s0, space2);
Texture2D<float>  g_depth : register(t1, space2);
SamplerState      s_depth : register(s1, space2);

cbuffer DepthMaskFS : register(b0, space3) {
	/* xy: the depth's offset in the target, in pixels. zw: 1 / its size. */
	float4 mask_area;
	/* x: hide where the depth is larger than this. */
	float4 mask_params;
};

struct VSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 tint : TEXCOORD1;
	float4 bias : TEXCOORD2;
	float  monochrome : TEXCOORD3;
	float  source : TEXCOORD4;
	float  mip_bias : TEXCOORD5;
};

float4 main(VSOut input) : SV_Target {
	float2 depth_uv = (input.position.xy - mask_area.xy) * mask_area.zw;
	if (g_depth.Sample(s_depth, depth_uv) > mask_params.x)
		discard;

	/* The rest is scene_blit.frag. */
	float4 s = g_tex.SampleBias(g_smp, input.uv, input.mip_bias);
	if (input.source > 1.5f)
		s.rgb = s.a > 0.0f ? AeronLinearToSrgb(saturate(s.rgb / s.a)) * s.a : float3(0.0f, 0.0f, 0.0f);
	else if (input.source > 0.5f)
		s.rgb = AeronLinearToSrgb(saturate(s.rgb));
	if (s.a > 0.97f)
		s.a = 1.0f;
	float3 src = lerp(s.rgb, max(s.r, max(s.g, s.b)).xxx, input.monochrome);
	float3 rgb = src * input.tint.rgb + input.bias.rgb * s.a;
	float  a   = saturate(s.a * input.tint.a + input.bias.a);
	return float4(rgb, a);
}
