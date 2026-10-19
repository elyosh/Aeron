#define AERON_CLUSTER_UNIFORM_BINDING register(b0, space2)
#define AERON_CLUSTER_NO_FRAGMENT_BUFFERS 1
#include "scene_clustered_lights.hlsli"

#define AERON_CLUSTER_THREADS 64u

/* The point lights in view space, in their index order. w bounds the
 * whole light. */
StructuredBuffer<float4> g_lights : register(t0, space0);
/* The depth bins' ranges, then the tiles' masks
 * (scene_clustered_lights.hlsli). */
RWStructuredBuffer<uint> g_data : register(u0, space1);

float cluster_slice_depth(uint boundary) {
	return fs_cluster_near_z * exp2((float)boundary / fs_cluster_slice_scale);
}

/* The first | last << 16 range of the lights from `begin` to `end` whose
 * depth extent overlaps near_z to far_z. */
uint bin_range(uint begin, uint end, float near_z, float far_z) {
	uint first = AERON_CLUSTER_EMPTY_RANGE;
	uint last  = 0u;
	for (uint i = begin; i < end; ++i) {
		float4 light = g_lights[i];
		if (light.z + light.w >= near_z && light.z - light.w <= far_z) {
			first = min(first, i);
			last  = i;
		}
	}
	return first | (last << 16u);
}

/* Fragments nearer than the near plane fall in the first bin, and those
 * past the last boundary in the last one. */
void build_bin(uint bin) {
	float near_z     = bin == 0u ? -3.402823466e+38f : cluster_slice_depth(bin);
	float far_z      = bin + 1u >= fs_cluster_grid_z ? 3.402823466e+38f : cluster_slice_depth(bin + 1u);
	uint  tube_start = min(fs_cluster_tube_start, fs_cluster_point_count);
	g_data[AERON_CLUSTER_BIN_WORDS * bin]      = bin_range(0u, tube_start, near_z, far_z);
	g_data[AERON_CLUSTER_BIN_WORDS * bin + 1u] = bin_range(tube_start, fs_cluster_point_count, near_z, far_z);
}

/* A light reaches a tile if it is in front of the near plane and inside
 * the tile's four side planes through the camera, expanded by a pixel. */
void build_tile(uint tile_index) {
	uint  tile_x       = tile_index % fs_cluster_grid_x;
	uint  tile_y       = tile_index / fs_cluster_grid_x;
	float width        = (float)fs_cluster_viewport_width;
	float height       = (float)fs_cluster_viewport_height;
	float tile_size    = (float)fs_cluster_tile_size;
	float pixel_left   = max((float)tile_x * tile_size - 1.0f, 0.0f);
	float pixel_right  = min((float)(tile_x + 1u) * tile_size + 1.0f, width);
	float pixel_top    = max((float)tile_y * tile_size - 1.0f, 0.0f);
	float pixel_bottom = min((float)(tile_y + 1u) * tile_size + 1.0f, height);

	float  ndc_left      = pixel_left * (2.0f / width) - 1.0f;
	float  ndc_right     = pixel_right * (2.0f / width) - 1.0f;
	float  ndc_top       = 1.0f - pixel_top * (2.0f / height);
	float  ndc_bottom    = 1.0f - pixel_bottom * (2.0f / height);
	float4 slopes        = float4((ndc_left - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
								  (ndc_right - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
								  (fs_cluster_proj_y_offset - ndc_top) * fs_cluster_tan_v_half,
								  (fs_cluster_proj_y_offset - ndc_bottom) * fs_cluster_tan_v_half);
	float4 plane_lengths = sqrt(1.0f + slopes * slopes);

	uint base = clustered_light_mask_base(tile_index);
	for (uint word = 0u; word < fs_cluster_mask_words; ++word) {
		uint bits = 0u;
		uint end  = min(32u, fs_cluster_point_count - (word << 5u));
		for (uint bit = 0u; bit < end; ++bit) {
			float4 light = g_lights[(word << 5u) + bit];
			float3 p     = light.xyz;
			float  r     = light.w;
			float4 side  = float4(p.x - slopes.x * p.z, slopes.y * p.z - p.x, p.y - slopes.z * p.z,
								  slopes.w * p.z - p.y);
			if (p.z + r >= fs_cluster_near_z && all(side >= -r * plane_lengths))
				bits |= 1u << bit;
		}
		g_data[base + word] = bits;
	}
}

[numthreads(AERON_CLUSTER_THREADS, 1, 1)] void main(uint3 thread : SV_DispatchThreadID) {
	uint tile_count = fs_cluster_grid_x * fs_cluster_grid_y;
	if (thread.x < tile_count)
		build_tile(thread.x);
	else if (thread.x < tile_count + fs_cluster_grid_z)
		build_bin(thread.x - tile_count);
}
