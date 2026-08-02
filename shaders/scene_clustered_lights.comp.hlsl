#define AERON_CLUSTER_UNIFORM_BINDING register(b0, space2)
#define AERON_CLUSTER_NO_FRAGMENT_BUFFERS 1
#include "scene_clustered_lights.hlsli"

#define AERON_CLUSTER_MAX_SCENE_LIGHTS 256u
#define AERON_CLUSTER_THREADS 64u

struct ClusterLight {
	float4 view_position_range;
	float4 color_luminance;
};

StructuredBuffer<ClusterLight> g_lights : register(t0, space0);
RWStructuredBuffer<uint2>      g_headers : register(u0, space1);
RWStructuredBuffer<uint>       g_indices : register(u1, space1);
RWStructuredBuffer<uint>       g_stats : register(u2, space1);

groupshared uint  s_candidate_count;
groupshared uint  s_candidate_indices[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared float s_candidate_scores[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared uint  s_selected[AERON_CLUSTER_MAX_LIGHTS];

float cluster_slice_depth(uint boundary) {
	return fs_cluster_near_z * exp2((float)boundary / fs_cluster_slice_scale);
}

bool sphere_intersects_cluster(ClusterLight light, uint3 cluster, out float nearest_distance) {
	nearest_distance   = 0.0f;
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
	float slope_left   = (ndc_left - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half;
	float slope_right  = (ndc_right - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half;
	float slope_top    = (fs_cluster_proj_y_offset - ndc_top) * fs_cluster_tan_v_half;
	float slope_bottom = (fs_cluster_proj_y_offset - ndc_bottom) * fs_cluster_tan_v_half;

	float  z_near = cluster_slice_depth(cluster.z);
	float  z_far  = cluster_slice_depth(min(cluster.z + 1u, fs_cluster_grid_z - 1u));
	float3 p      = light.view_position_range.xyz;
	float  r      = light.view_position_range.w;
	if (p.z + r < z_near || p.z - r > z_far)
		return false;
	if (p.x - slope_left * p.z < -r * length(float2(1.0f, slope_left)))
		return false;
	if (slope_right * p.z - p.x < -r * length(float2(1.0f, slope_right)))
		return false;
	if (p.y - slope_top * p.z < -r * length(float2(1.0f, slope_top)))
		return false;
	if (slope_bottom * p.z - p.y < -r * length(float2(1.0f, slope_bottom)))
		return false;

	float  x0         = slope_left * z_near;
	float  x1         = slope_left * z_far;
	float  x2         = slope_right * z_near;
	float  x3         = slope_right * z_far;
	float  y0         = slope_top * z_near;
	float  y1         = slope_top * z_far;
	float  y2         = slope_bottom * z_near;
	float  y3         = slope_bottom * z_far;
	float3 bounds_min = float3(min(min(x0, x1), min(x2, x3)), min(min(y0, y1), min(y2, y3)), z_near);
	float3 bounds_max = float3(max(max(x0, x1), max(x2, x3)), max(max(y0, y1), max(y2, y3)), z_far);
	float3 outside    = max(max(bounds_min - p, p - bounds_max), 0.0f.xxx);
	nearest_distance  = length(outside);
	return true;
}

bool candidate_better(float score, uint index, float best_score, uint best_index) {
	return score > best_score || (score == best_score && index < best_index);
}

[numthreads(AERON_CLUSTER_THREADS, 1, 1)] void main(uint3 group_id : SV_GroupID,
													uint  group_index : SV_GroupIndex) {
	if (group_index == 0u)
		s_candidate_count = 0u;
	GroupMemoryBarrierWithGroupSync();

	for (uint light_index = group_index; light_index < fs_cluster_point_count;
		 light_index += AERON_CLUSTER_THREADS) {
		ClusterLight light = g_lights[light_index];
		float        nearest_distance;
		if (sphere_intersects_cluster(light, group_id, nearest_distance)) {
			uint candidate_slot;
			InterlockedAdd(s_candidate_count, 1u, candidate_slot);
			if (candidate_slot < AERON_CLUSTER_MAX_SCENE_LIGHTS) {
				float score = light.color_luminance.w * 0.5f /
							  max(nearest_distance, max(fs_cluster_point_min_distance, 1.0f));
				if (fs_cluster_point_contribution_cap > 0.0f)
					score = min(score, fs_cluster_point_contribution_cap);
				s_candidate_indices[candidate_slot] = light_index;
				s_candidate_scores[candidate_slot]  = score;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (group_index != 0u)
		return;
	uint candidate_count = min(s_candidate_count, AERON_CLUSTER_MAX_SCENE_LIGHTS);
	uint retained_count  = min(candidate_count, AERON_CLUSTER_MAX_LIGHTS);
	if (candidate_count <= AERON_CLUSTER_MAX_LIGHTS) {
		for (uint i = 0u; i < retained_count; ++i)
			s_selected[i] = s_candidate_indices[i];
	} else {
		for (uint output = 0u; output < retained_count; ++output) {
			uint  best_slot  = 0xffffffffu;
			uint  best_index = 0xffffffffu;
			float best_score = -1.0f;
			for (uint i = 0u; i < candidate_count; ++i) {
				uint index = s_candidate_indices[i];
				if (index != 0xffffffffu &&
					candidate_better(s_candidate_scores[i], index, best_score, best_index)) {
					best_slot  = i;
					best_index = index;
					best_score = s_candidate_scores[i];
				}
			}
			s_selected[output]             = best_index;
			s_candidate_indices[best_slot] = 0xffffffffu;
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

	InterlockedMax(g_stats[0], candidate_count);
	if (candidate_count > 0u)
		InterlockedAdd(g_stats[1], 1u);
	if (candidate_count > AERON_CLUSTER_MAX_LIGHTS) {
		InterlockedAdd(g_stats[2], 1u);
		InterlockedAdd(g_stats[3], candidate_count - AERON_CLUSTER_MAX_LIGHTS);
	}
}
