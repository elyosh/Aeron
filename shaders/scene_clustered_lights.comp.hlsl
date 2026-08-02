#define AERON_CLUSTER_UNIFORM_BINDING register(b0, space2)
#define AERON_CLUSTER_NO_FRAGMENT_BUFFERS 1
#include "scene_clustered_lights.hlsli"

#define AERON_CLUSTER_MAX_SCENE_LIGHTS 256u
#define AERON_CLUSTER_THREADS 64u

struct ClusterLight {
	float4 view_position_range;
	uint   point_light_index;
	float  luminance;
	float2 _pad;
};

StructuredBuffer<ClusterLight> g_lights : register(t0, space0);
RWStructuredBuffer<uint2>      g_headers : register(u0, space1);
RWStructuredBuffer<uint>       g_indices : register(u1, space1);

groupshared uint   s_candidate_count;
groupshared uint   s_candidate_lights[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared float  s_candidate_values[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared uint   s_selected[AERON_CLUSTER_MAX_LIGHTS];
groupshared float4 s_cluster_slopes;
groupshared float4 s_cluster_plane_lengths;
groupshared float4 s_cluster_bounds_min;
groupshared float4 s_cluster_bounds_max;

float cluster_slice_depth(uint boundary) {
	return fs_cluster_near_z * exp2((float)boundary / fs_cluster_slice_scale);
}

void prepare_cluster_geometry(uint3 cluster) {
	float width        = (float)fs_cluster_viewport_width;
	float height       = (float)fs_cluster_viewport_height;
	float tile_size    = (float)fs_cluster_tile_size;
	float pixel_left   = max((float)cluster.x * tile_size - 1.0f, 0.0f);
	float pixel_right  = min((float)(cluster.x + 1u) * tile_size + 1.0f, width);
	float pixel_top    = max((float)cluster.y * tile_size - 1.0f, 0.0f);
	float pixel_bottom = min((float)(cluster.y + 1u) * tile_size + 1.0f, height);

	float ndc_left     = pixel_left * (2.0f / width) - 1.0f;
	float ndc_right    = pixel_right * (2.0f / width) - 1.0f;
	float ndc_top      = 1.0f - pixel_top * (2.0f / height);
	float ndc_bottom   = 1.0f - pixel_bottom * (2.0f / height);
	float4 slopes = float4((ndc_left - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
						  (ndc_right - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
						  (fs_cluster_proj_y_offset - ndc_top) * fs_cluster_tan_v_half,
						  (fs_cluster_proj_y_offset - ndc_bottom) * fs_cluster_tan_v_half);

	float  z_near = cluster_slice_depth(cluster.z);
	float  z_far  = cluster_slice_depth(min(cluster.z + 1u, fs_cluster_grid_z - 1u));
	float  x0     = slopes.x * z_near;
	float  x1     = slopes.x * z_far;
	float  x2     = slopes.y * z_near;
	float  x3     = slopes.y * z_far;
	float  y0     = slopes.z * z_near;
	float  y1     = slopes.z * z_far;
	float  y2     = slopes.w * z_near;
	float  y3     = slopes.w * z_far;

	s_cluster_slopes        = slopes;
	s_cluster_plane_lengths = sqrt(1.0f + slopes * slopes);
	s_cluster_bounds_min    = float4(min(min(x0, x1), min(x2, x3)),
									 min(min(y0, y1), min(y2, y3)), z_near, 0.0f);
	s_cluster_bounds_max    = float4(max(max(x0, x1), max(x2, x3)),
									 max(max(y0, y1), max(y2, y3)), z_far, 0.0f);
}

bool sphere_intersects_cluster(ClusterLight light, out float nearest_distance_sq) {
	nearest_distance_sq = 0.0f;
	float4 slopes        = s_cluster_slopes;
	float4 plane_lengths = s_cluster_plane_lengths;
	float3 p             = light.view_position_range.xyz;
	float  r             = light.view_position_range.w;
	if (p.z + r < s_cluster_bounds_min.z || p.z - r > s_cluster_bounds_max.z)
		return false;
	if (p.x - slopes.x * p.z < -r * plane_lengths.x)
		return false;
	if (slopes.y * p.z - p.x < -r * plane_lengths.y)
		return false;
	if (p.y - slopes.z * p.z < -r * plane_lengths.z)
		return false;
	if (slopes.w * p.z - p.y < -r * plane_lengths.w)
		return false;

	float3 outside = max(max(s_cluster_bounds_min.xyz - p, p - s_cluster_bounds_max.xyz), 0.0f.xxx);
	nearest_distance_sq = dot(outside, outside);
	return true;
}

bool candidate_better(float score, uint index, float best_score, uint best_index) {
	return score > best_score || (score == best_score && index < best_index);
}

[numthreads(AERON_CLUSTER_THREADS, 1, 1)] void main(uint3 group_id : SV_GroupID,
											uint  group_index : SV_GroupIndex) {
	if (group_index == 0u) {
		s_candidate_count = 0u;
		prepare_cluster_geometry(group_id);
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint light_index = group_index; light_index < fs_cluster_local_count;
		 light_index += AERON_CLUSTER_THREADS) {
		ClusterLight light = g_lights[light_index];
		float        nearest_distance_sq;
		if (sphere_intersects_cluster(light, nearest_distance_sq)) {
			uint candidate_slot;
			InterlockedAdd(s_candidate_count, 1u, candidate_slot);
			if (candidate_slot < AERON_CLUSTER_MAX_SCENE_LIGHTS) {
				s_candidate_lights[candidate_slot] = light_index;
				s_candidate_values[candidate_slot] = nearest_distance_sq;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (group_index != 0u)
		return;
	uint candidate_count = min(s_candidate_count, AERON_CLUSTER_MAX_SCENE_LIGHTS);
	uint retained_count  = min(candidate_count, AERON_CLUSTER_MAX_LIGHTS);
	if (candidate_count <= AERON_CLUSTER_MAX_LIGHTS) {
		for (uint i = 0u; i < retained_count; ++i) {
			s_selected[i] = g_lights[s_candidate_lights[i]].point_light_index;
		}
	} else {
		for (uint i = 0u; i < candidate_count; ++i) {
			ClusterLight light            = g_lights[s_candidate_lights[i]];
			float nearest_distance = sqrt(s_candidate_values[i]);
			float score            = light.luminance * 0.5f /
								 max(nearest_distance, max(fs_cluster_point_min_distance, 1.0f));
			if (fs_cluster_point_contribution_cap > 0.0f)
				score = min(score, fs_cluster_point_contribution_cap);
			s_candidate_values[i] = score;
			s_candidate_lights[i] = light.point_light_index;
		}
		for (uint output = 0u; output < retained_count; ++output) {
			uint  best_slot  = 0xffffffffu;
			uint  best_index = 0xffffffffu;
			float best_score = -1.0f;
			for (uint i = 0u; i < candidate_count; ++i) {
				uint index = s_candidate_lights[i];
				if (index != 0xffffffffu && candidate_better(s_candidate_values[i], index, best_score, best_index)) {
					best_slot  = i;
					best_index = index;
					best_score = s_candidate_values[i];
				}
			}
			s_selected[output]            = best_index;
			s_candidate_lights[best_slot] = 0xffffffffu;
		}
	}

	for (uint i = 1u; i < retained_count; ++i) {
		uint value    = s_selected[i];
		uint position = i;
		while (position > 0u && value < s_selected[position - 1u]) {
			s_selected[position] = s_selected[position - 1u];
			--position;
		}
		s_selected[position] = value;
	}

	uint cluster_index       = (group_id.z * fs_cluster_grid_y + group_id.y) * fs_cluster_grid_x + group_id.x;
	g_headers[cluster_index] = uint2(retained_count, candidate_count);
	uint base                = cluster_index * AERON_CLUSTER_MAX_LIGHTS;
	for (uint i = 0u; i < retained_count; ++i)
		g_indices[base + i] = s_selected[i];
}
