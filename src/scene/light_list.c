#include "light_list.h"

#include <math.h>
#include <string.h>

/* Returns nonzero when a spot light's cone can't reach a sphere: the sphere
 * is behind the light, beyond its range, or outside its outer angle. This
 * is the same test as spot_misses_sphere in
 * scene_clustered_lights.comp.hlsl. The test is conservative: it never
 * says that a sphere the light reaches is missed. A cone of 90 degrees or
 * more is treated as reaching every sphere. */
static int light_list_spot_misses(const AeronSceneClusterLightGPU* light, const float center[3],
								  float radius) {
	const float cos_outer = light->view_spot_direction_cos[3];
	if (!(cos_outer > 0.0f)) {
		return 0;
	}
	float v[3];
	float along   = 0.0f;
	float length2 = 0.0f;
	for (int axis = 0; axis < 3; ++axis) {
		v[axis] = center[axis] - light->view_spot_position_range[axis];
		along += v[axis] * light->view_spot_direction_cos[axis];
		length2 += v[axis] * v[axis];
	}
	const float across  = sqrtf(fmaxf(length2 - along * along, 0.0f));
	const float outside = cos_outer * across - sqrtf(1.0f - cos_outer * cos_outer) * along;
	return along < -radius || along > radius + light->view_spot_position_range[3] || outside > radius;
}

enum { LIGHT_LIST_SPHERE, LIGHT_LIST_SPOT, LIGHT_LIST_TUBE };

/* Far lights from directions more than 60 degrees apart go to different
 * combined lights. */
#define LIGHT_LIST_FAR_SPLIT_COS 0.5f

static int light_list_kind(uint32_t index, uint32_t spot_start, uint32_t tube_start) {
	return index >= tube_start ? LIGHT_LIST_TUBE : index >= spot_start ? LIGHT_LIST_SPOT : LIGHT_LIST_SPHERE;
}

static float light_list_dot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float light_list_luminance(const float rgb[3]) {
	return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

/* Computes the light that a fragment at `center`, the centre of an instance
 * of radius `radius`, gets from `light`. It uses the formulas of
 * accumulate_point_light, accumulate_spot_light and accumulate_tube_light
 * (scene_pbr_lighting.hlsli), and treats a far tube as a sphere at its
 * centre, as the shader does.
 *
 * Returns -1 when the light is near the instance. Returns 0 when it is far
 * but gives no light at the centre. Returns 1 when it is far and lights
 * the centre: `direction` is then the unit direction toward the light,
 * `rgb` its illuminance on a surface that faces it, and `spread` how much
 * the light's own size widens a highlight's GGX alpha. A NaN makes the
 * light near. */
static int light_list_far_light(const AeronScenePointLightGPU* light, int kind, const float center[3],
								float radius, float direction[3], float rgb[3], float* spread) {
	float to_center[3];
	float nearest[3];
	float half_length = 0.0f;
	for (int axis = 0; axis < 3; ++axis) {
		to_center[axis] = light->position_range[axis] - center[axis];
		nearest[axis]   = to_center[axis];
	}
	if (kind == LIGHT_LIST_TUBE) {
		if (!(light->color[3] > 0.0f)) {
			return -1;
		}
		half_length              = light->half_axis[3];
		const float inv_length   = light->direction[3];
		const float along        = -light_list_dot(to_center, light->half_axis) * inv_length * inv_length;
		const float along_inside = fminf(fmaxf(along, -1.0f), 1.0f);
		for (int axis = 0; axis < 3; ++axis) {
			nearest[axis] += light->half_axis[axis] * along_inside;
		}
	}
	const float nearest_distance = sqrtf(light_list_dot(nearest, nearest));
	const float center_distance  = sqrtf(light_list_dot(to_center, to_center));
	if (!(nearest_distance >= AERON_SCENE_LIGHT_LIST_FAR * radius) ||
		!(nearest_distance >= AERON_SCENE_LIGHT_LIST_FAR_TUBE * half_length) || !(center_distance > 0.0f)) {
		return -1;
	}
	const float q2 = nearest_distance * nearest_distance * light->position_range[3];
	if (!(q2 < 1.0f)) {
		return 0;
	}
	for (int axis = 0; axis < 3; ++axis) {
		direction[axis] = to_center[axis] / center_distance;
	}
	const float source_radius = fabsf(light->color[3]);
	const float near_distance = fmaxf(center_distance, source_radius);
	float       scale         = (1.0f - q2 * q2) / (near_distance * near_distance);
	if (kind == LIGHT_LIST_SPOT) {
		const float cone = fminf(
			fmaxf(-light_list_dot(light->direction, direction) * light->direction[3] + light->half_axis[0],
				  0.0f),
			1.0f);
		scale *= cone * cone;
	} else if (kind == LIGHT_LIST_SPHERE && light->color[3] < 0.0f) {
		/* A one-sided sphere. */
		scale *= fmaxf(-light_list_dot(light->direction, direction), 0.0f);
	}
	if (!(scale > 0.0f)) {
		return 0;
	}
	for (int c = 0; c < 3; ++c) {
		rgb[c] = light->color[c] * scale;
	}
	float size = source_radius;
	if (kind == LIGHT_LIST_TUBE) {
		/* The part of the tube across the direction toward it. */
		const float along = light_list_dot(light->half_axis, direction);
		float       across[3];
		for (int axis = 0; axis < 3; ++axis) {
			across[axis] = light->half_axis[axis] - direction[axis] * along;
		}
		size += sqrtf(light_list_dot(across, across));
	}
	*spread = size / (2.0f * near_distance);
	return 1;
}

/* Combines the far lights listed in `far_indices` into at most
 * AERON_SCENE_LIGHT_LIST_FAR_MAX lights. The first combined light takes the
 * lights close to the strongest light's direction. The second takes the
 * lights closer to the direction most opposite to it, if that direction is
 * more than 60 degrees away. Each pass computes the lights again, so no
 * array of results is needed. */
static void light_list_combine(const AeronScenePointLightGPU* shaded, const uint32_t* far_indices,
							   uint32_t far_listed, uint32_t spot_start, uint32_t tube_start,
							   const float center[3], float radius,
							   AeronSceneFarLightGPU far[AERON_SCENE_LIGHT_LIST_FAR_MAX],
							   uint32_t*             far_count) {
	float    seeds[AERON_SCENE_LIGHT_LIST_FAR_MAX][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	uint32_t seed_count                               = 0;
	float    strongest                                = 0.0f;
	float    direction[3], rgb[3], spread;
	for (uint32_t k = 0; k < far_listed; ++k) {
		const uint32_t i = far_indices[k];
		if (light_list_far_light(&shaded[i], light_list_kind(i, spot_start, tube_start), center, radius,
								 direction, rgb, &spread) > 0 &&
			light_list_luminance(rgb) > strongest) {
			strongest = light_list_luminance(rgb);
			memcpy(seeds[0], direction, sizeof seeds[0]);
			seed_count = 1;
		}
	}
	if (seed_count == 0) {
		return;
	}
	float lowest = 1.0f;
	for (uint32_t k = 0; k < far_listed; ++k) {
		const uint32_t i = far_indices[k];
		if (light_list_far_light(&shaded[i], light_list_kind(i, spot_start, tube_start), center, radius,
								 direction, rgb, &spread) > 0 &&
			light_list_dot(direction, seeds[0]) < lowest) {
			lowest = light_list_dot(direction, seeds[0]);
			memcpy(seeds[1], direction, sizeof seeds[1]);
		}
	}
	if (lowest < LIGHT_LIST_FAR_SPLIT_COS) {
		seed_count = 2;
	}

	/* For each group: the sum of its directions weighted by luminance, the
	 * total weight, the weighted sum of the lights' spreads, and the sum of
	 * their illuminances. */
	float   sums[AERON_SCENE_LIGHT_LIST_FAR_MAX][3]   = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	float   weights[AERON_SCENE_LIGHT_LIST_FAR_MAX]   = { 0.0f, 0.0f };
	float   spreads[AERON_SCENE_LIGHT_LIST_FAR_MAX]   = { 0.0f, 0.0f };
	float   totals[AERON_SCENE_LIGHT_LIST_FAR_MAX][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	uint8_t groups[AERON_SCENE_LIGHT_LIST_FAR_MAX]    = { 0, 0 };
	for (uint32_t k = 0; k < far_listed; ++k) {
		const uint32_t i = far_indices[k];
		if (light_list_far_light(&shaded[i], light_list_kind(i, spot_start, tube_start), center, radius,
								 direction, rgb, &spread) <= 0) {
			continue;
		}
		const int group =
			seed_count == 2 && light_list_dot(direction, seeds[1]) > light_list_dot(direction, seeds[0]);
		const float weight = light_list_luminance(rgb);
		for (int axis = 0; axis < 3; ++axis) {
			sums[group][axis] += direction[axis] * weight;
			totals[group][axis] += rgb[axis];
		}
		weights[group] += weight;
		spreads[group] += spread * weight;
		groups[group] = 1;
	}

	/* A combined light points along the weighted mean of its lights'
	 * directions. Its spread is half the sine of the angle whose cosine is
	 * the length of that mean, plus the lights' own mean spread. */
	float directions[AERON_SCENE_LIGHT_LIST_FAR_MAX][3];
	for (uint32_t g = 0; g < seed_count; ++g) {
		const float length    = sqrtf(light_list_dot(sums[g], sums[g]));
		const float coherence = weights[g] > 0.0f ? fminf(length / weights[g], 1.0f) : 1.0f;
		for (int axis = 0; axis < 3; ++axis) {
			directions[g][axis] = length > 0.0f ? sums[g][axis] / length : seeds[g][axis];
		}
		spreads[g] = 0.5f * sqrtf(fmaxf(1.0f - coherence * coherence, 0.0f)) +
					 (weights[g] > 0.0f ? spreads[g] / weights[g] : 0.0f);
	}

	/* The diffuse on a surface that faces a combined light: the sum of each
	 * light's illuminance times its cosine to that direction. */
	float diffuse[AERON_SCENE_LIGHT_LIST_FAR_MAX][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	for (uint32_t k = 0; k < far_listed; ++k) {
		const uint32_t i = far_indices[k];
		if (light_list_far_light(&shaded[i], light_list_kind(i, spot_start, tube_start), center, radius,
								 direction, rgb, &spread) <= 0) {
			continue;
		}
		const int group =
			seed_count == 2 && light_list_dot(direction, seeds[1]) > light_list_dot(direction, seeds[0]);
		const float cosine = fmaxf(light_list_dot(direction, directions[group]), 0.0f);
		for (int c = 0; c < 3; ++c) {
			diffuse[group][c] += rgb[c] * cosine;
		}
	}

	for (uint32_t g = 0; g < seed_count; ++g) {
		if (!groups[g]) {
			continue;
		}
		AeronSceneFarLightGPU* out = &far[(*far_count)++];
		for (int axis = 0; axis < 3; ++axis) {
			out->direction_spread[axis] = directions[g][axis];
			out->diffuse[axis]          = diffuse[g][axis];
			out->specular[axis]         = totals[g][axis];
		}
		out->direction_spread[3] = spreads[g];
		out->diffuse[3]          = 0.0f;
		out->specular[3]         = 0.0f;
	}
}

int AeronSceneLightList_Build(const AeronSceneClusterLightGPU* lights, const AeronScenePointLightGPU* shaded,
							  uint32_t count, uint32_t spot_start, uint32_t tube_start,
							  const float view_center[3], const float world_center[3], float radius,
							  uint32_t* far_scratch, uint32_t indices[AERON_SCENE_LIGHT_LIST_MAX],
							  AeronSceneFarLightGPU far[AERON_SCENE_LIGHT_LIST_FAR_MAX],
							  uint32_t*             far_count) {
	int      listed     = 0;
	uint32_t far_listed = 0;

	if (!view_center || !indices || !far || !far_count || (count && !lights) || !(radius >= 0.0f) ||
		(shaded && (!world_center || !far_scratch))) {
		return -1;
	}
	*far_count = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const AeronSceneClusterLightGPU* light = &lights[i];
		/* Skip the light when its bounding sphere doesn't touch the
		 * instance's. The test is written so that a NaN keeps the light. */
		float distance2 = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			const float delta = view_center[axis] - light->view_position_range[axis];
			distance2 += delta * delta;
		}
		const float reach = light->view_position_range[3] + radius;
		if (reach < 0.0f || distance2 > reach * reach) {
			continue;
		}
		if (i >= spot_start && i < tube_start) {
			if (light_list_spot_misses(light, view_center, radius)) {
				continue;
			}
			const float merge = light->view_merge[3];
			if (merge != 0.0f) {
				float centre2 = 0.0f;
				for (int axis = 0; axis < 3; ++axis) {
					const float delta = view_center[axis] - light->view_merge[axis];
					centre2 += delta * delta;
				}
				const float distance = sqrtf(centre2);
				const float limit    = fabsf(merge);
				if (distance - radius > limit) {
					/* The whole instance is far from the group: use the
					 * merged light and skip the group's lights. */
					if (merge > 0.0f) {
						continue;
					}
				} else if (distance + radius <= limit) {
					/* The whole instance is near the group: use the group's
					 * lights and skip the merged light. */
					if (merge < 0.0f) {
						continue;
					}
				} else {
					/* Partly near and partly far: use the clusters. */
					return -1;
				}
			}
		}
		if (shaded) {
			float     direction[3], rgb[3], spread;
			const int far_light = light_list_far_light(&shaded[i], light_list_kind(i, spot_start, tube_start),
													   world_center, radius, direction, rgb, &spread);
			if (far_light > 0) {
				far_scratch[far_listed++] = i;
			}
			if (far_light >= 0) {
				continue;
			}
		}
		if (listed == AERON_SCENE_LIGHT_LIST_MAX) {
			/* Too many near lights for a list: use the clusters. */
			return -1;
		}
		indices[listed++] = i;
	}
	if (far_listed > 0) {
		light_list_combine(shaded, far_scratch, far_listed, spot_start, tube_start, world_center, radius, far,
						   far_count);
	}
	if (listed > AERON_SCENE_LIGHT_LIST_FEW &&
		2u * ((uint32_t)listed + *far_count) > (uint32_t)listed + far_listed) {
		/* Combining saves too little for so many near lights. */
		return -1;
	}
	return listed;
}
