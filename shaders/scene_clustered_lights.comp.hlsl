#define AERON_CLUSTER_UNIFORM_BINDING register(b0, space2)
#define AERON_CLUSTER_NO_FRAGMENT_BUFFERS 1
#include "scene_clustered_lights.hlsli"

#define AERON_CLUSTER_MAX_SCENE_LIGHTS 256u
#define AERON_CLUSTER_THREADS 64u

struct ClusterLight {
	float4 view_position_range;
	uint   point_light_index;
	float  luminance;
	float  source_radius;
	float  _pad;
};

StructuredBuffer<ClusterLight> g_lights : register(t0, space0);
/* Per cluster two header words, then every cluster's indices
 * (scene_clustered_lights.hlsli). */
RWStructuredBuffer<uint>       g_data : register(u0, space1);

groupshared uint   s_candidate_count;
groupshared uint   s_candidate_lights[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared float  s_candidate_values[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared uint   s_selected[AERON_CLUSTER_MAX_LIGHTS];
groupshared uint   s_kept[AERON_CLUSTER_MAX_SCENE_LIGHTS];
groupshared uint   s_tube_count;
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

	/* The whole group selects in parallel: a cluster can collect up to
	 * AERON_CLUSTER_MAX_SCENE_LIGHTS candidates, and one thread scanning
	 * them all for each retained slot dominated dense scenes. */
	uint candidate_count = min(s_candidate_count, AERON_CLUSTER_MAX_SCENE_LIGHTS);
	uint retained_count  = min(candidate_count, AERON_CLUSTER_MAX_LIGHTS);
	bool overflow        = candidate_count > AERON_CLUSTER_MAX_LIGHTS;
	for (uint i = group_index; i < candidate_count; i += AERON_CLUSTER_THREADS) {
		ClusterLight light = g_lights[s_candidate_lights[i]];
		float        d     = max(sqrt(s_candidate_values[i]), light.source_radius);
		s_candidate_values[i] = light.luminance / (d * d);
		s_candidate_lights[i] = light.point_light_index;
	}
	GroupMemoryBarrierWithGroupSync();

	/* Keep the best AERON_CLUSTER_MAX_LIGHTS by score (ties to the lower
	 * index): a candidate's rank is the number that beat it. */
	for (uint i = group_index; i < candidate_count; i += AERON_CLUSTER_THREADS) {
		uint rank = 0u;
		if (overflow) {
			float score = s_candidate_values[i];
			uint  index = s_candidate_lights[i];
			for (uint j = 0u; j < candidate_count; ++j) {
				rank += candidate_better(s_candidate_values[j], s_candidate_lights[j], score, index) ? 1u : 0u;
			}
		}
		s_kept[i] = rank < AERON_CLUSTER_MAX_LIGHTS ? 1u : 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	/* Store the kept indices ascending: the tubes come last in the light
	 * buffer, so they are the tail. */
	if (group_index == 0u)
		s_tube_count = 0u;
	GroupMemoryBarrierWithGroupSync();
	for (uint i = group_index; i < candidate_count; i += AERON_CLUSTER_THREADS) {
		if (s_kept[i] == 0u)
			continue;
		uint index    = s_candidate_lights[i];
		uint position = 0u;
		for (uint j = 0u; j < candidate_count; ++j) {
			position += s_kept[j] != 0u && s_candidate_lights[j] < index ? 1u : 0u;
		}
		s_selected[position] = index;
		if (index >= fs_cluster_tube_start)
			InterlockedAdd(s_tube_count, 1u);
	}
	GroupMemoryBarrierWithGroupSync();

	uint cluster_index = (group_id.z * fs_cluster_grid_y + group_id.y) * fs_cluster_grid_x + group_id.x;
	if (group_index == 0u) {
		/* The header's high half counts the tubes. */
		g_data[AERON_CLUSTER_HEADER_WORDS * cluster_index]      = retained_count | (s_tube_count << 16u);
		g_data[AERON_CLUSTER_HEADER_WORDS * cluster_index + 1u] = candidate_count;
	}
	uint base = clustered_light_index_base(cluster_index);
	for (uint i = group_index; i < retained_count; i += AERON_CLUSTER_THREADS)
		g_data[base + i] = s_selected[i];
}
