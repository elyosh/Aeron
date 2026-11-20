/* Vertex shader for the projected overlays (see mesh_overlay.h). It draws
 * the mesh's own vertices and computes each image's UV from the vertex
 * position, so no copy of the geometry is needed. */
#include "scene_mesh_projection.hlsli"
cbuffer OverlayVS : register(b0, space1) {
	row_major float4x4 view_proj;
	row_major float4x4 model_to_world;
	float4             params; /* x = view-space depth bias */
	uint               mesh_table_base;
	uint               table_component_count;
	uint2              _pad;
};
cbuffer ProjectionVS : register(b1, space1) {
	uint4      projection_count; /* x */
	Projection projections[PROJECTION_MAX];
};
#include "mesh_table_layout.hlsli"
StructuredBuffer<float4> mesh_tables : register(t0, space0);
/* Only the position and the component of AeronGltfVertex are read. */
struct VSIn {
	float3 pos : POSITION;
	float  component : COLOR0;
};
/* uv01 holds the UVs of images 0 and 1, and uv23 those of images 2 and 3.
 * The fragment shaders for one or two images only read uv01. */
struct VSOut {
	float4 pos : SV_Position;
	float4 uv01 : TEXCOORD0;
	float4 uv23 : TEXCOORD1;
};
VSOut main(VSIn input) {
	VSOut output;
	uint  row       = (uint)max((int)round(input.component), 0);
	uint  component = min(row, table_component_count - 1u);
	uint table_base = mesh_table_base;
	/* The UVs are a linear function of the position. So when the GPU
	 * interpolates them between vertices, each pixel gets its exact UV. */
	uint mask_row  = min(row, 255u);
	uint mask_word = mask_row >> 5u;
	uint mask_bit  = 1u << (mask_row & 31u);
	float2 image_uv[PROJECTION_MAX];
	[unroll] for (uint i = 0u; i < PROJECTION_MAX; ++i) {
		image_uv[i] = float2(PROJECTION_NO_UV, PROJECTION_NO_UV);
		if (i < projection_count.x && (projections[i].rows[mask_word >> 2u][mask_word & 3u] & mask_bit) != 0u) {
			image_uv[i] = float2(dot(projections[i].u_row.xyz, input.pos) + projections[i].u_row.w,
								 dot(projections[i].v_row.xyz, input.pos) + projections[i].v_row.w);
		}
	}
	output.uv01 = float4(image_uv[0], image_uv[1]);
	output.uv23 = float4(image_uv[2], image_uv[3]);
	if (mesh_tables[table_base + 3u * table_component_count +
					AERON_MESH_LANE_VISIBILITY * AERON_MESH_LANE_VEC4(table_component_count) +
					(component >> 2u)][component & 3u] < 0.5f) {
		output.pos = float4(0, 0, 0, -1);
		return output;
	}
	float4 rotation_row_0 = mesh_tables[table_base + component * 3u + 0u];
	float4 rotation_row_1 = mesh_tables[table_base + component * 3u + 1u];
	float4 rotation_row_2 = mesh_tables[table_base + component * 3u + 2u];
	float3 articulated_position =
		float3(dot(rotation_row_0.xyz, input.pos) + rotation_row_0.w,
			   dot(rotation_row_1.xyz, input.pos) + rotation_row_1.w,
			   dot(rotation_row_2.xyz, input.pos) + rotation_row_2.w);
	float4 world_position = mul(model_to_world, float4(articulated_position, 1));
	float4 clip_position  = mul(view_proj, world_position);
	if (params.x > 0.0f && clip_position.w - params.x > 0.001f) {
		clip_position.z = clip_position.z * clip_position.w / (clip_position.w - params.x);
	}
	output.pos = clip_position;
	return output;
}
