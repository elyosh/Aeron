/* Fullscreen diagnostic for the reversed-Z directional-shadow atlas. */

cbuffer ShadowDebugUniforms : register(b0, space3) {
	float2 output_size;
	float  cascade_count;
	float  selected_cascade; /* -1 = complete atlas, 0..3 = one tile */
};

Texture2D<float> g_shadow_atlas : register(t0, space2);
SamplerState     g_point_sampler : register(s0, space2);

struct VSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target0 {
	float  side       = min(output_size.x, output_size.y);
	float2 origin     = 0.5f * (output_size - side.xx);
	float2 preview_uv = (input.uv * output_size - origin) / max(side, 1.0f);
	if (any(preview_uv < 0.0f) || any(preview_uv > 1.0f))
		return float4(0.01f, 0.01f, 0.01f, 1.0f);

	int    count    = clamp((int)round(cascade_count), 1, 4);
	int    selected = clamp((int)round(selected_cascade), -1, count - 1);
	float2 atlas_uv = preview_uv;
	if (selected >= 0 && count > 1) {
		float2 tile = float2(selected & 1, selected >> 1);
		atlas_uv    = (tile + preview_uv) * 0.5f;
	}

	float  depth = g_shadow_atlas.SampleLevel(g_point_sampler, atlas_uv, 0.0f);
	float  value = depth > 0.0f ? 0.15f + 0.85f * pow(saturate(depth), 0.25f) : 0.0f;
	float3 color = value.xxx;

	/* Mark atlas topology without covering more than two output pixels. */
	float line_width = 2.0f / max(side, 1.0f);
	bool  outer      = any(preview_uv < line_width) || any(preview_uv > 1.0f - line_width);
	bool  divider    = selected < 0 && count > 1 &&
					   (abs(preview_uv.x - 0.5f) < line_width || abs(preview_uv.y - 0.5f) < line_width);
	if (outer || divider)
		color = float3(0.95f, 0.55f, 0.10f);

	return float4(color, 1.0f);
}
