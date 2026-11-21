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
 * Lights that are far away compared with the instance's size are combined.
 * Their direction and strength barely change over the instance, so each is
 * computed once at the instance's centre, and they are summed into at most
 * AERON_SCENE_LIGHT_LIST_FAR_MAX combined lights. The list only holds the
 * near lights, so an instance lit by many far lights, such as a cockpit in
 * a battle, can still have a list.
 *
 * This file only does math, so tests/light_list_test.c can build it without
 * the rest of the scene. */

#ifdef __cplusplus
extern "C" {
#endif

/* One point light as the shaders read it (fs_point_lights), in world
 * space. The lights come as spheres, then spot lights, then tubes. */
typedef struct AeronScenePointLightGPU {
	/* xyz: the position, w: the inverse square of the range. */
	float position_range[4];
	float color[4];
	/* xyz: a tube's half axis, w: its half length (zero for a sphere). A
	 * spot light's x holds the offset of its falloff instead. */
	float half_axis[4];
	/* xyz: a one-sided or spot light's unit direction (zero for a light
	 * emitting all around), w: the inverse of a tube's half length, or the
	 * scale of a spot light's falloff (zero for other spheres). Per-light
	 * values the shader would otherwise derive at every fragment. A
	 * one-sided light's color.w (its sphere radius) is negated, so lights
	 * emitting all around never load the direction. A spot light's stays
	 * positive: its loop knows that it has a direction. */
	float direction[4];
} AeronScenePointLightGPU;

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

/* The largest number of near lights in a list. When more near lights reach
 * an instance, it uses the clusters, which give each fragment only the
 * lights near it.
 *
 * Up to AERON_SCENE_LIGHT_LIST_FEW near lights, an instance always gets a
 * list. With more, it only gets one when the list shades at most half the
 * lights that the clusters could give its fragments: its near lights and
 * its combined far lights, against its near lights and all its far lights.
 * Otherwise the clusters can be faster, because they give each fragment
 * only the lights of its own tile. */
#define AERON_SCENE_LIGHT_LIST_MAX 8
#define AERON_SCENE_LIGHT_LIST_FEW 4

/* The largest number of combined far lights in a list. The far lights are
 * split by direction: the second combined light takes those that come from
 * the other side, which a single light would cancel out. */
#define AERON_SCENE_LIGHT_LIST_FAR_MAX 2

/* A light is far from an instance when the nearest point of the light is
 * at least AERON_SCENE_LIGHT_LIST_FAR times the instance's radius away.
 * Over the instance, a far light's direction then changes by at most about
 * 1 / AERON_SCENE_LIGHT_LIST_FAR radians, and its strength by at most about
 * 2 / AERON_SCENE_LIGHT_LIST_FAR (12%), from the side facing it to the
 * other side.
 *
 * A far tube is measured as a sphere at its centre, so it must also be
 * AERON_SCENE_LIGHT_LIST_FAR_TUBE times its half length away: from there
 * the shaders also shade a tube as a sphere (AERON_TUBE_SPHERE_LENGTHS in
 * scene_pbr_lighting.hlsli). Its diffuse is then off by at most
 * 1 / AERON_SCENE_LIGHT_LIST_FAR_TUBE^2 (1.6%), along its axis. */
#define AERON_SCENE_LIGHT_LIST_FAR 16.0f
#define AERON_SCENE_LIGHT_LIST_FAR_TUBE 8.0f

/* A combined far light, as the shaders read it (fs_far_lights). It is a
 * light from one direction, the same on every fragment of the instance.
 * - direction_spread: xyz, the unit direction toward the lights, in world
 *   space. w: how much to widen the highlight's GGX alpha, for how spread
 *   out the lights are and for their sizes.
 * - diffuse: rgb, the illuminance on a surface that faces the direction.
 * - specular: rgb, the sum of the lights' illuminances, used for the
 *   highlight. */
typedef struct AeronSceneFarLightGPU {
	float direction_spread[4];
	float diffuse[4];
	float specular[4];
} AeronSceneFarLightGPU;

/* Builds the light list of an instance.
 *
 * `view_center` and `radius` are the instance's bounding sphere in view
 * space, and `world_center` its centre in world space. `lights` holds the
 * frame's `count` point lights as the cluster build sees them, and
 * `shaded` the same lights as the shaders see them: spheres first, spot
 * lights from index `spot_start`, tubes from index `tube_start`.
 *
 * The function writes the indices of the near lights that can reach the
 * instance to `indices`, sorted, and returns how many there are. It writes
 * the combined far lights to `far` and their number to `far_count`. It
 * returns -1 when the instance must use the clusters: more than
 * AERON_SCENE_LIGHT_LIST_MAX near lights can reach it, or more than
 * AERON_SCENE_LIGHT_LIST_FEW and combining saves too little. `far_scratch` is
 * working space for `count` indices.
 *
 * Pass NULL for `shaded` to combine no lights: every light that can reach
 * the instance is then a near light, and `far_count` is 0.
 *
 * Combining changes the picture a little. At the instance's centre, the
 * diffuse is exact on a surface that faces a combined light. Elsewhere it
 * is close when the far lights come from about the same direction, and when
 * they don't, the second combined light takes those from the other side.
 * The far lights' highlights become one highlight per combined light,
 * widened by how spread the lights are, which keeps their energy. A
 * one-sided tube is never combined, because its light has no simple form
 * far away.
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
 * Apart from combined far lights, this is the only case where a list and
 * the clusters can give different results. The clusters decide for each screen tile, and they
 * underestimate the distance to the group's centre. So in a tile where the
 * group's centre is on screen, the clusters light a far instance with the
 * group's lights, and the list lights it with the merged light. */
int AeronSceneLightList_Build(const AeronSceneClusterLightGPU* lights, const AeronScenePointLightGPU* shaded,
							  uint32_t count, uint32_t spot_start, uint32_t tube_start,
							  const float view_center[3], const float world_center[3], float radius,
							  uint32_t* far_scratch, uint32_t indices[AERON_SCENE_LIGHT_LIST_MAX],
							  AeronSceneFarLightGPU far[AERON_SCENE_LIGHT_LIST_FAR_MAX], uint32_t* far_count);

#ifdef __cplusplus
}
#endif

#endif
