#ifndef AERON_SCENE_LIGHT_LIST_H
#define AERON_SCENE_LIGHT_LIST_H

#include <stdint.h>

/* Light lists.
 *
 * Normally a fragment finds its lights in the clusters: it looks up its
 * screen tile and its depth bin, then loops over the lights found there.
 * That lookup is slow, and it is wasted when the same few lights reach every
 * fragment of an instance, for example one far light that covers a whole
 * cockpit.
 *
 * So when only a few lights can reach an instance, the scene gives the
 * instance a short list of those lights in a uniform. Its fragments loop
 * over the list and skip the cluster lookup. They shade the same lights, in
 * the same order, with the same code.
 *
 * This file only does math, so tests/light_list_test.c can build it without
 * the rest of the scene. */

#ifdef __cplusplus
extern "C" {
#endif

/* What the cluster build knows about one point light, in view space. The
 * lights are stored in the same order as the shaders' point lights.
 *
 * - view_position_range: the sphere that contains everything the light can
 *   reach (AeronScene_LightBounds). xyz is its centre and w its radius.
 * - view_spot_position_range: for a spot light, its position and its range.
 * - view_spot_direction_cos: for a spot light, its unit direction, and the
 *   cosine of its outer angle in w. w is -1 for other lights.
 * - view_merge: for a spot light in a merged group, the group's centre and
 *   its merge distance. Near the centre the group's lights are used, and
 *   beyond the merge distance their merged light is used. w is the merge
 *   distance for a light of the group, minus the merge distance for the
 *   merged light, and 0 for a light that is in no group. */
typedef struct AeronSceneClusterLightGPU {
	float view_position_range[4];
	float view_spot_position_range[4];
	float view_spot_direction_cos[4];
	float view_merge[4];
} AeronSceneClusterLightGPU;

/* The largest number of lights in a list. When more lights can reach an
 * instance, it uses the clusters, which give each fragment only the lights
 * near it. */
#define AERON_SCENE_LIGHT_LIST_MAX 4

/* Builds the light list of an instance.
 *
 * `center` and `radius` are the instance's bounding sphere in view space.
 * `lights` holds the frame's `count` point lights: spheres first, spot
 * lights from index `spot_start`, tubes from index `tube_start`.
 *
 * The function writes the indices of the lights that can reach the instance
 * to `indices`, sorted, and returns how many there are. It returns 0 when
 * no light can reach the instance. It returns -1 when the instance must use
 * the clusters, because more than AERON_SCENE_LIGHT_LIST_MAX lights can
 * reach it.
 *
 * A light is skipped when its bounding sphere doesn't touch the instance's.
 * A spot light is also skipped when its cone doesn't. Listing a light that
 * doesn't reach the instance is harmless: each fragment still tests the
 * light's range and cone.
 *
 * Merged groups need a decision. A fragment that reads a list can't choose
 * between a group's lights and their merged light, so the list must hold
 * one or the other:
 * - If the whole instance is farther from the group's centre than the merge
 *   distance, the list holds the merged light.
 * - If the whole instance is nearer, the list holds the group's lights.
 * - Otherwise the instance uses the clusters.
 *
 * This is the one case where a list and the clusters can differ. The
 * clusters decide for each screen tile, and they underestimate the distance
 * to the group's centre. So in a tile where the group's centre is on
 * screen, the clusters light a far instance with the group's lights, and
 * the list lights it with the merged light. */
int AeronSceneLightList_Build(const AeronSceneClusterLightGPU* lights, uint32_t count, uint32_t spot_start,
							  uint32_t tube_start, const float center[3], float radius,
							  uint32_t indices[AERON_SCENE_LIGHT_LIST_MAX]);

#ifdef __cplusplus
}
#endif

#endif
