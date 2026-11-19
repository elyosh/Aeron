#include "light_list.h"

#include <math.h>

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

int AeronSceneLightList_Build(const AeronSceneClusterLightGPU* lights, uint32_t count, uint32_t spot_start,
							  uint32_t tube_start, const float center[3], float radius,
							  uint32_t indices[AERON_SCENE_LIGHT_LIST_MAX]) {
	int listed = 0;

	if (!center || !indices || (count && !lights) || !(radius >= 0.0f)) {
		return -1;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const AeronSceneClusterLightGPU* light = &lights[i];
		/* Skip the light when its bounding sphere doesn't touch the
		 * instance's. The test is written so that a NaN keeps the light. */
		float distance2 = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			const float delta = center[axis] - light->view_position_range[axis];
			distance2 += delta * delta;
		}
		const float reach = light->view_position_range[3] + radius;
		if (reach < 0.0f || distance2 > reach * reach) {
			continue;
		}
		if (i >= spot_start && i < tube_start) {
			if (light_list_spot_misses(light, center, radius)) {
				continue;
			}
			const float merge = light->view_merge[3];
			if (merge != 0.0f) {
				float centre2 = 0.0f;
				for (int axis = 0; axis < 3; ++axis) {
					const float delta = center[axis] - light->view_merge[axis];
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
		if (listed == AERON_SCENE_LIGHT_LIST_MAX) {
			/* Too many lights for a list: use the clusters. */
			return -1;
		}
		indices[listed++] = i;
	}
	return listed;
}
