#ifndef AERON_SCENE_CLUSTERED_LIGHTS_INCLUDED
#define AERON_SCENE_CLUSTERED_LIGHTS_INCLUDED

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
	/* The point lights from this index on are tubes, after the spheres. */
	uint   fs_cluster_tube_start;
	uint   fs_cluster_mask_words;
	uint   fs_cluster_tile_size;
	uint   fs_cluster_flags;
#ifdef AERON_SCENE_LIGHTING_ENVIRONMENT
	/* Image-based lighting environment (AeronScenePbrEnvironment):
	 * x = diffuse strength (0 = no diffuse cube), y = specular strength,
	 * z = specular mip count, w = reserved. The basis rows map world
	 * directions into the cubes' local frame. */
	float4 fs_environment_params;
	float4 fs_environment_basis[3];
	/* Debug material override: x = enabled, y = metallic, z = perceptual
	 * roughness. Read only by the diagnostic shader variant. */
	float4 fs_debug_material;
	/* x = emissive scale (AeronScene_SetEmissiveScale). */
	float4 fs_emission;
	/* xy = render-target size in pixels, zw = its inverse. */
	float4 fs_render_size;
	/* The drawn object's probe set (AeronScene_AddProbeSet), read by the
	 * probe variants only: x = probe count, y = cube array mip count,
	 * zw = grid points along x and y; the diffuse and specular strengths;
	 * the rows of the world-to-model matrix (scene units to model metres);
	 * and the grid mapping g = p * scale + offset in grid texel units,
	 * scale.w = grid points along z. */
	float4 fs_probe_params;
	float4 fs_probe_strengths;
	float4 fs_world_to_model[3];
	float4 fs_grid_scale;
	float4 fs_grid_offset;
	/* The probe occlusion grid: its low corner in model metres, w = 1
	 * while it applies; its cells along x, y, z, w = their size in metres. */
	float4 fs_occlusion_origin;
	float4 fs_occlusion_dims;
	/* The reflecting floor (planar_reflection.c), in model metres: its
	 * centre and half size along x and y; its height, how far from it a
	 * surface takes its reflection, 1 while the drawn instance takes it, and
	 * the reflection's mip count. While the mirrored view draws, its camera
	 * position (w = 1). Where the floor reads the reflection: x0, y0, x1,
	 * y1 in half-resolution texels. */
	float4 fs_planar_rect;
	float4 fs_planar_params;
	float4 fs_planar_eye;
	float4 fs_planar_screen;
#endif
};

#define fs_cluster_debug_view (fs_cluster_flags & 1u)

/* The cluster build's output buffer. Lights are sorted by view depth
 * within the spheres and within the tubes. First, per depth bin, two
 * words: the index range of the spheres that reach the bin, then that of
 * the tubes, each first | last << 16 (first > last when empty). Then, per
 * screen tile, a bitmask of the lights that reach the tile,
 * fs_cluster_mask_words words long. A light reaches a fragment if it is in
 * both its tile's mask and its bin's range. */
#define AERON_CLUSTER_BIN_WORDS 2u
#define AERON_CLUSTER_EMPTY_RANGE 0xffffu

uint clustered_light_mask_base(uint tile_index) {
	return AERON_CLUSTER_BIN_WORDS * fs_cluster_grid_z + tile_index * fs_cluster_mask_words;
}

/* The bits of a mask word that hold the lights first to last. */
uint clustered_light_range_bits(uint word, uint first, uint last) {
	uint word_first = word << 5u;
	uint lo         = first > word_first ? first - word_first : 0u;
	uint hi         = last + 1u > word_first ? last + 1u - word_first : 0u;
	uint below_hi   = hi >= 32u ? 0xffffffffu : (1u << hi) - 1u;
	uint below_lo   = lo >= 32u ? 0xffffffffu : (1u << lo) - 1u;
	return below_hi & ~below_lo;
}

#ifndef AERON_CLUSTER_NO_FRAGMENT_BUFFERS
#ifndef AERON_CLUSTER_DATA_REGISTER
#define AERON_CLUSTER_DATA_REGISTER t10
#endif
StructuredBuffer<uint> fs_cluster_data : register(AERON_CLUSTER_DATA_REGISTER, space2);

uint clustered_light_depth_slice(float view_depth) {
	float depth       = max(view_depth, fs_cluster_near_z);
	float slice_value = log2(depth / fs_cluster_near_z) * fs_cluster_slice_scale;
	return min((uint)max(floor(slice_value), 0.0f), fs_cluster_grid_z - 1u);
}

/* The fragment's lights: returns its tile's mask base, and its bin's
 * sphere and tube ranges in `ranges`. */
uint clustered_light_fragment(float2 screen_position, float3 world_position, out uint2 ranges) {
	float2 viewport_position = screen_position - float2(fs_cluster_viewport_x, fs_cluster_viewport_y);
	if (any(viewport_position < 0.0f.xx) ||
		any(viewport_position >= float2(fs_cluster_viewport_width, fs_cluster_viewport_height))) {
		ranges = uint2(AERON_CLUSTER_EMPTY_RANGE, AERON_CLUSTER_EMPTY_RANGE);
		return 0u;
	}
	uint  tile_x     = min((uint)viewport_position.x / fs_cluster_tile_size, fs_cluster_grid_x - 1u);
	uint  tile_y     = min((uint)viewport_position.y / fs_cluster_tile_size, fs_cluster_grid_y - 1u);
	float view_depth = dot(world_position - fs_cluster_camera_position, fs_cluster_camera_forward);
	uint  bin        = clustered_light_depth_slice(view_depth);
	ranges = uint2(fs_cluster_data[AERON_CLUSTER_BIN_WORDS * bin], fs_cluster_data[AERON_CLUSTER_BIN_WORDS * bin + 1u]);
	return clustered_light_mask_base(tile_y * fs_cluster_grid_x + tile_x);
}

uint clustered_light_range_count(uint range, uint mask_base) {
	uint first = range & 0xffffu;
	uint last  = range >> 16u;
	uint count = 0u;
	for (uint word = first >> 5u; first <= last && word <= (last >> 5u); ++word) {
		count += countbits(fs_cluster_data[mask_base + word] & clustered_light_range_bits(word, first, last));
	}
	return count;
}

float3 clustered_light_debug_color(float2 screen_position, float3 world_position) {
	if (fs_cluster_point_count == 0u)
		return 0.0f.xxx;
	uint2 ranges;
	uint  mask_base = clustered_light_fragment(screen_position, world_position, ranges);
	uint  count     = clustered_light_range_count(ranges.x, mask_base) + clustered_light_range_count(ranges.y, mask_base);
	if (count > 32u)
		return float3(1.0f, 0.0f, 0.0f);
	if (count > 16u)
		return float3(1.0f, 0.35f, 0.0f);
	if (count > 8u)
		return float3(1.0f, 0.9f, 0.0f);
	if (count > 4u)
		return float3(0.1f, 1.0f, 0.1f);
	if (count > 0u)
		return float3(0.1f, 0.35f, 1.0f);
	return 0.0f.xxx;
}
#endif

#endif
