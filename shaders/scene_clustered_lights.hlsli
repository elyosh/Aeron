#ifndef AERON_SCENE_CLUSTERED_LIGHTS_INCLUDED
#define AERON_SCENE_CLUSTERED_LIGHTS_INCLUDED

#define AERON_CLUSTER_MAX_LIGHTS 32u
#define AERON_CLUSTER_MAX_GLOBAL_LIGHTS 4u

#ifndef AERON_CLUSTER_UNIFORM_BINDING
#define AERON_CLUSTER_UNIFORM_BINDING register(b2, space3)
#endif

cbuffer ClusteredLightUniform : AERON_CLUSTER_UNIFORM_BINDING {
	float3 fs_cluster_camera_position;
	float  fs_cluster_near_z;
	float3 fs_cluster_camera_forward;
	float  fs_cluster_slice_scale;
	float  fs_cluster_tan_h_half;
	float  fs_cluster_tan_v_half;
	float  fs_cluster_proj_x_offset;
	float  fs_cluster_proj_y_offset;
	uint   fs_cluster_viewport_x;
	uint   fs_cluster_viewport_y;
	uint   fs_cluster_viewport_width;
	uint   fs_cluster_viewport_height;
	uint   fs_cluster_grid_x;
	uint   fs_cluster_grid_y;
	uint   fs_cluster_grid_z;
	uint   fs_cluster_point_count;
	float  fs_cluster_point_min_distance;
	float  fs_cluster_point_contribution_cap;
	uint   fs_cluster_tile_size;
	uint   fs_cluster_flags;
	uint4  fs_cluster_global_indices;
};

#define fs_cluster_enabled (fs_cluster_flags & 1u)
#define fs_cluster_debug_view ((fs_cluster_flags >> 1u) & 1u)
#define fs_cluster_global_count ((fs_cluster_flags >> 8u) & 0xffu)
#define fs_cluster_local_count ((fs_cluster_flags >> 16u) & 0xffffu)

#ifndef AERON_CLUSTER_NO_FRAGMENT_BUFFERS
#ifndef AERON_CLUSTER_HEADER_REGISTER
#define AERON_CLUSTER_HEADER_REGISTER t10
#endif
#ifndef AERON_CLUSTER_INDEX_REGISTER
#define AERON_CLUSTER_INDEX_REGISTER t11
#endif
StructuredBuffer<uint2> fs_cluster_headers : register(AERON_CLUSTER_HEADER_REGISTER, space2);
StructuredBuffer<uint>  fs_cluster_indices : register(AERON_CLUSTER_INDEX_REGISTER, space2);

uint clustered_light_depth_slice(float view_depth) {
	float depth       = max(view_depth, fs_cluster_near_z);
	float slice_value = log2(depth / fs_cluster_near_z) * fs_cluster_slice_scale;
	return min((uint)max(floor(slice_value), 0.0f), fs_cluster_grid_z - 1u);
}

uint clustered_light_fragment_index(float2 screen_position, float3 world_position) {
	float2 viewport_position = screen_position - float2(fs_cluster_viewport_x, fs_cluster_viewport_y);
	uint   tile_x            = min((uint)viewport_position.x / fs_cluster_tile_size, fs_cluster_grid_x - 1u);
	uint   tile_y            = min((uint)viewport_position.y / fs_cluster_tile_size, fs_cluster_grid_y - 1u);
	float  view_depth        = dot(world_position - fs_cluster_camera_position, fs_cluster_camera_forward);
	uint   tile_z            = clustered_light_depth_slice(view_depth);
	return (tile_z * fs_cluster_grid_y + tile_y) * fs_cluster_grid_x + tile_x;
}

uint2 clustered_light_fragment_header(float2 screen_position, float3 world_position, out uint cluster_index) {
	float2 viewport_position = screen_position - float2(fs_cluster_viewport_x, fs_cluster_viewport_y);
	if (any(viewport_position < 0.0f.xx) ||
		any(viewport_position >= float2(fs_cluster_viewport_width, fs_cluster_viewport_height))) {
		cluster_index = 0u;
		return uint2(0u, 0u);
	}
	cluster_index = clustered_light_fragment_index(screen_position, world_position);
	return fs_cluster_headers[cluster_index];
}

float3 clustered_light_debug_color(float2 screen_position, float3 world_position) {
	if (fs_cluster_point_count == 0u)
		return 0.0f.xxx;
	uint  cluster_index;
	uint2 header     = clustered_light_fragment_header(screen_position, world_position, cluster_index);
	uint  candidates = header.y;
	if (candidates > AERON_CLUSTER_MAX_LIGHTS)
		return float3(1.0f, 0.0f, 0.0f);
	if (candidates > 16u)
		return float3(1.0f, 0.35f, 0.0f);
	if (candidates > 8u)
		return float3(1.0f, 0.9f, 0.0f);
	if (candidates > 4u)
		return float3(0.1f, 1.0f, 0.1f);
	if (candidates > 0u)
		return float3(0.1f, 0.35f, 1.0f);
	return 0.0f.xxx;
}
#endif

#endif
