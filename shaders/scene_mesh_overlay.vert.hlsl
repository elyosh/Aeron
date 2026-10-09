cbuffer OverlayVS : register(b0, space1) {
	row_major float4x4 view_proj;
	row_major float4x4 model_to_world;
	float4             params; /* x = view-space depth bias */
	uint               mesh_table_base;
	uint               table_component_count;
	uint2              _pad;
};
#include "mesh_table_layout.hlsli"
StructuredBuffer<float4> mesh_tables : register(t0, space0);
struct VSIn {
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
	float  component : COLOR0;
};
struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};
VSOut main(VSIn input) {
	VSOut output;
	int   component = clamp((int)round(input.component), 0,
							 (int)table_component_count - 1);
	uint table_base = mesh_table_base;
	if (mesh_tables[table_base + 3u * table_component_count +
					AERON_MESH_LANE_VISIBILITY * AERON_MESH_LANE_VEC4(table_component_count) +
					((uint)component >> 2u)][(uint)component & 3u] < 0.5f) {
		output.pos = float4(0, 0, 0, -1);
		output.uv  = input.uv;
		return output;
	}
	float4 rotation_row_0 = mesh_tables[table_base + (uint)component * 3u + 0u];
	float4 rotation_row_1 = mesh_tables[table_base + (uint)component * 3u + 1u];
	float4 rotation_row_2 = mesh_tables[table_base + (uint)component * 3u + 2u];
	float3 articulated_position =
		float3(dot(rotation_row_0.xyz, input.pos) + rotation_row_0.w,
			   dot(rotation_row_1.xyz, input.pos) + rotation_row_1.w,
			   dot(rotation_row_2.xyz, input.pos) + rotation_row_2.w);
	float4 clip_position = mul(view_proj, mul(model_to_world, float4(articulated_position, 1)));
	if (params.x > 0.0f && clip_position.w - params.x > 0.001f) {
		clip_position.z = clip_position.z * clip_position.w / (clip_position.w - params.x);
	}
	output.pos = clip_position;
	output.uv  = input.uv;
	return output;
}
