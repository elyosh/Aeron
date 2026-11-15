#define AERON_CLUSTER_UNIFORM_BINDING register(b0, space2)
#define AERON_CLUSTER_NO_FRAGMENT_BUFFERS 1
#include "scene_clustered_lights.hlsli"

#define AERON_CLUSTER_THREADS 64u

/* The point lights in view space, in their index order
 * (AeronSceneClusterLightGPU). */
struct ClusterLight {
	/* The sphere that holds what the light lights. */
	float4 bound;
	/* A spot light's position and range. */
	float4 spot_position_range;
	/* A spot light's unit direction, w: the cosine of its outer angle, -1
	 * for other lights. */
	float4 spot_direction_cos;
};
StructuredBuffer<ClusterLight> g_lights : register(t0, space0);
/* The depth bins' ranges, then the tiles' masks
 * (scene_clustered_lights.hlsli). */
RWStructuredBuffer<uint> g_data : register(u0, space1);

float cluster_slice_depth(uint boundary) {
	return fs_cluster_near_z * exp2((float)boundary / fs_cluster_slice_scale);
}

/* Whether a spot light's cone misses a sphere: the sphere is behind the
 * light, past its range, or outside its outer angle (Bart Wronski, "Cull
 * that cone!"). The angle test measures to the line of the cone's side,
 * which is never farther than the cone itself, so it never culls a lit
 * sphere. A cone of 90 degrees or more is never culled, and neither is an
 * infinite sphere: every test is then false. */
bool spot_misses_sphere(ClusterLight light, float4 sphere) {
	float cos_outer = light.spot_direction_cos.w;
	if (!(cos_outer > 0.0f))
		return false;
	float3 v       = sphere.xyz - light.spot_position_range.xyz;
	float  along   = dot(v, light.spot_direction_cos.xyz);
	float  across  = sqrt(max(dot(v, v) - along * along, 0.0f));
	float  outside = cos_outer * across - sqrt(1.0f - cos_outer * cos_outer) * along;
	return along < -sphere.w || along > sphere.w + light.spot_position_range.w || outside > sphere.w;
}

/* The first | last << 16 range of the lights from `begin` to `end` whose
 * depth extent overlaps near_z to far_z, leaving out the spot lights whose
 * cone misses `sphere`. */
uint bin_range(uint begin, uint end, float near_z, float far_z, float4 sphere) {
	uint first = AERON_CLUSTER_EMPTY_RANGE;
	uint last  = 0u;
	for (uint i = begin; i < end; ++i) {
		ClusterLight light = g_lights[i];
		float4       bound = light.bound;
		if (bound.z + bound.w >= near_z && bound.z - bound.w <= far_z && !spot_misses_sphere(light, sphere)) {
			first = min(first, i);
			last  = i;
		}
	}
	return first | (last << 16u);
}

/* The sphere around the part of the view between depths near_z and far_z,
 * both 0 or more: around the box that holds its corners. */
float4 view_slab_sphere(float near_z, float far_z) {
	float2 low_slope  = float2((-1.0f - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
							   (fs_cluster_proj_y_offset - 1.0f) * fs_cluster_tan_v_half);
	float2 high_slope = float2((1.0f - fs_cluster_proj_x_offset) * fs_cluster_tan_h_half,
							   (fs_cluster_proj_y_offset + 1.0f) * fs_cluster_tan_v_half);
	float2 low        = min(low_slope * near_z, low_slope * far_z);
	float2 high       = max(high_slope * near_z, high_slope * far_z);
	float3 box_low    = float3(low, near_z);
	float3 box_high   = float3(high, far_z);
	return float4(0.5f * (box_low + box_high), 0.5f * length(box_high - box_low));
}

/* Fragments nearer than the near plane fall in the first bin, and those
 * past the last boundary in the last one. */
void build_bin(uint bin) {
	float near_z     = bin == 0u ? -3.402823466e+38f : cluster_slice_depth(bin);
	float far_z      = bin + 1u >= fs_cluster_grid_z ? 3.402823466e+38f : cluster_slice_depth(bin + 1u);
	uint  tube_start = min(fs_cluster_kind_starts >> 16u, fs_cluster_point_count);
	uint  spot_start = min(fs_cluster_kind_starts & 0xffffu, tube_start);
	/* The spot lights are tested against the part of the view in the bin.
	 * The last bin reaches past every boundary, and its infinite sphere
	 * culls nothing; spheres and tubes don't take the test. */
	float4 slab = view_slab_sphere(bin == 0u ? 0.0f : near_z, far_z);
	float4 none = float4(0.0f, 0.0f, 0.0f, 3.402823466e+38f);
	g_data[AERON_CLUSTER_BIN_WORDS * bin]      = bin_range(0u, spot_start, near_z, far_z, none);
	g_data[AERON_CLUSTER_BIN_WORDS * bin + 1u] = bin_range(spot_start, tube_start, near_z, far_z, slab);
	g_data[AERON_CLUSTER_BIN_WORDS * bin + 2u] = bin_range(tube_start, fs_cluster_point_count, near_z, far_z, none);
}

/* Whether a spot light's cone lies wholly outside a plane: `offset` is the
 * signed distance of the light from the plane, positive inside, and `facing`
 * the cosine between the light's direction and the plane's inward normal.
 * The cone holds the points within the light's range and outer angle. The
 * one farthest inside is the apex, or the point at full range on the side of
 * the cone nearest the normal, at the angle between the two less the outer
 * angle. */
bool spot_outside_plane(float offset, float facing, float range, float cos_outer, float sin_outer) {
	float reach = facing >= cos_outer
					  ? 1.0f
					  : facing * cos_outer + sqrt(max(1.0f - facing * facing, 0.0f)) * sin_outer;
	return offset + range * max(reach, 0.0f) < 0.0f;
}

/* A light reaches a tile if it is in front of the near plane and inside
 * the tile's four side planes through the camera, expanded by a pixel. A
 * spot light must also have part of its cone inside each of them: its
 * bounding sphere holds the whole cone, which is much larger than the part
 * the light shines into. */
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
	uint   tube_start    = min(fs_cluster_kind_starts >> 16u, fs_cluster_point_count);
	uint   spot_start    = min(fs_cluster_kind_starts & 0xffffu, tube_start);

	uint base = clustered_light_mask_base(tile_index);
	for (uint word = 0u; word < fs_cluster_mask_words; ++word) {
		uint bits = 0u;
		uint end  = min(32u, fs_cluster_point_count - (word << 5u));
		for (uint bit = 0u; bit < end; ++bit) {
			uint   index = (word << 5u) + bit;
			float4 light = g_lights[index].bound;
			float3 p     = light.xyz;
			float  r     = light.w;
			float4 side  = float4(p.x - slopes.x * p.z, slopes.y * p.z - p.x, p.y - slopes.z * p.z,
								  slopes.w * p.z - p.y);
			if (!(p.z + r >= fs_cluster_near_z && all(side >= -r * plane_lengths)))
				continue;
			if (index >= spot_start && index < tube_start) {
				float4 spot      = g_lights[index].spot_position_range;
				float4 direction = g_lights[index].spot_direction_cos;
				float  cos_outer = direction.w;
				float  sin_outer = sqrt(max(1.0f - cos_outer * cos_outer, 0.0f));
				/* The side planes go through the camera, so the same
				 * expressions give the direction's components along their
				 * normals. */
				float4 offset = float4(spot.x - slopes.x * spot.z, slopes.y * spot.z - spot.x,
									   spot.y - slopes.z * spot.z, slopes.w * spot.z - spot.y) /
								plane_lengths;
				float4 facing = float4(direction.x - slopes.x * direction.z, slopes.y * direction.z - direction.x,
									   direction.y - slopes.z * direction.z, slopes.w * direction.z - direction.y) /
								plane_lengths;
				if (spot_outside_plane(spot.z - fs_cluster_near_z, direction.z, spot.w, cos_outer, sin_outer) ||
					spot_outside_plane(offset.x, facing.x, spot.w, cos_outer, sin_outer) ||
					spot_outside_plane(offset.y, facing.y, spot.w, cos_outer, sin_outer) ||
					spot_outside_plane(offset.z, facing.z, spot.w, cos_outer, sin_outer) ||
					spot_outside_plane(offset.w, facing.w, spot.w, cos_outer, sin_outer))
					continue;
			}
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
