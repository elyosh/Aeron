/*
 * Tests light lists (src/scene/light_list.h): which lights go in an
 * instance's list, and when the instance must use the light clusters.
 *
 * Built with the tools as aeron_light_list_test. It needs no window and no
 * GPU. It prints each check and exits with a non-zero status when a check
 * fails.
 */

#include <stdio.h>
#include <string.h>

#include "light_list.h"

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

/* Returns a sphere light. (x, y, z) and `radius` are the sphere that the
 * light can reach. */
static AeronSceneClusterLightGPU sphere(float x, float y, float z, float radius) {
	AeronSceneClusterLightGPU light;
	memset(&light, 0, sizeof light);
	light.view_position_range[0]     = x;
	light.view_position_range[1]     = y;
	light.view_position_range[2]     = z;
	light.view_position_range[3]     = radius;
	light.view_spot_direction_cos[3] = -1.0f;
	return light;
}

/* Returns a spot light at (x, y, z) that points along the unit vector
 * (dx, dy, dz). It reaches up to `range`, inside the outer angle whose
 * cosine is `cos_outer`. Its bounding sphere is centred on the light, which
 * is larger than needed. That is allowed: bounds only have to contain the
 * cone. */
static AeronSceneClusterLightGPU spot(float x, float y, float z, float dx, float dy, float dz, float range,
									  float cos_outer) {
	AeronSceneClusterLightGPU light   = sphere(x, y, z, range);
	light.view_spot_position_range[0] = x;
	light.view_spot_position_range[1] = y;
	light.view_spot_position_range[2] = z;
	light.view_spot_position_range[3] = range;
	light.view_spot_direction_cos[0]  = dx;
	light.view_spot_direction_cos[1]  = dy;
	light.view_spot_direction_cos[2]  = dz;
	light.view_spot_direction_cos[3]  = cos_outer;
	return light;
}

/* Puts a spot light in a merged group whose centre is (x, y, z). Pass the
 * merge distance for a light of the group, and minus the merge distance
 * for the group's merged light. */
static void merge(AeronSceneClusterLightGPU* light, float x, float y, float z, float distance) {
	light->view_merge[0] = x;
	light->view_merge[1] = y;
	light->view_merge[2] = z;
	light->view_merge[3] = distance;
}

/* Builds the light list of an instance and checks it. `expected` is the
 * number of lights the list must hold, or -1 when the instance must use
 * the clusters. a, b, c and d are the expected indices. */
static int list_is(const AeronSceneClusterLightGPU* lights, uint32_t count, uint32_t spot_start,
				   uint32_t tube_start, const float center[3], float radius, int expected, uint32_t a,
				   uint32_t b, uint32_t c, uint32_t d) {
	const uint32_t want[AERON_SCENE_LIGHT_LIST_MAX] = { a, b, c, d };
	uint32_t       indices[AERON_SCENE_LIGHT_LIST_MAX];
	const int      listed =
		AeronSceneLightList_Build(lights, count, spot_start, tube_start, center, radius, indices);
	if (listed != expected) {
		return 0;
	}
	for (int k = 0; k < listed; ++k) {
		if (indices[k] != want[k]) {
			return 0;
		}
	}
	return 1;
}

int main(void) {
	const float               origin[3] = { 0.0f, 0.0f, 0.0f };
	AeronSceneClusterLightGPU lights[8];

	/* Sphere lights. The instance is at the origin with a radius of 2. */
	lights[0] = sphere(10.0f, 0.0f, 0.0f, 5.0f);    /* 10 away with a reach of 5: too far */
	lights[1] = sphere(0.0f, 6.0f, 0.0f, 5.0f);     /* 6 away with a reach of 5: touches the instance */
	lights[2] = sphere(0.0f, 0.0f, 100.0f, 200.0f); /* contains the instance */
	check(list_is(lights, 3, 3, 3, origin, 2.0f, 2, 1, 2, 0, 0),
		  "a light is listed when its sphere touches the instance");
	check(list_is(lights, 1, 1, 1, origin, 2.0f, 0, 0, 0, 0, 0),
		  "an instance that no light reaches gets an empty list");
	check(list_is(NULL, 0, 0, 0, origin, 2.0f, 0, 0, 0, 0, 0), "a scene without lights gives an empty list");

	/* Lights that all reach the instance: four fit in a list, five don't. */
	for (int i = 0; i < 5; ++i) {
		lights[i] = sphere(0.0f, 0.0f, 0.0f, 1.0f);
	}
	check(list_is(lights, 4, 4, 4, origin, 1.0f, 4, 0, 1, 2, 3), "four lights fill the list");
	check(list_is(lights, 5, 5, 5, origin, 1.0f, -1, 0, 0, 0, 0),
		  "an instance that five lights reach uses the clusters");
	check(list_is(lights, 1, 1, 1, origin, -1.0f, -1, 0, 0, 0, 0),
		  "an instance without a bound uses the clusters");

	/* Spot lights. The light is 10 units to the left of the instance, with
	 * an outer angle of 30 degrees. Only its direction changes. */
	const float cos30 = 0.8660254f;
	lights[0]         = spot(-10.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 50.0f, cos30);
	check(list_is(lights, 1, 0, 1, origin, 1.0f, 1, 0, 0, 0, 0),
		  "a spot light that points at the instance is listed");
	lights[0] = spot(-10.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 50.0f, cos30);
	check(list_is(lights, 1, 0, 1, origin, 1.0f, 0, 0, 0, 0, 0),
		  "a spot light that points away from the instance is skipped");
	lights[0] = spot(-10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 50.0f, cos30);
	check(list_is(lights, 1, 0, 1, origin, 1.0f, 0, 0, 0, 0, 0),
		  "a spot light whose cone passes beside the instance is skipped");
	check(list_is(lights, 1, 0, 1, origin, 20.0f, 1, 0, 0, 0, 0),
		  "the same light is listed when the instance is large enough to enter its cone");
	lights[0] = spot(-10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 50.0f, -0.5f);
	check(list_is(lights, 1, 0, 1, origin, 1.0f, 1, 0, 0, 0, 0),
		  "a spot light wider than 90 degrees is never skipped because of its cone");
	/* The cone test only applies to indices in the spot light range. Here
	 * the same light is passed as a sphere, so it is listed. */
	lights[0] = spot(-10.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 50.0f, cos30);
	check(list_is(lights, 1, 1, 1, origin, 1.0f, 1, 0, 0, 0, 0), "the cone test only applies to spot lights");

	/* A merged group. Lights 0 and 1 are the group's lights and light 2 is
	 * their merged light. The group is at x = -100, points along +X, and
	 * has a merge distance of 30. */
	lights[0] = spot(-100.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 500.0f, cos30);
	lights[1] = spot(-100.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 500.0f, cos30);
	lights[2] = spot(-100.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 500.0f, cos30);
	merge(&lights[0], -100.0f, 0.0f, 0.0f, 30.0f);
	merge(&lights[1], -100.0f, 0.0f, 0.0f, 30.0f);
	merge(&lights[2], -100.0f, 0.0f, 0.0f, -30.0f);
	check(list_is(lights, 3, 0, 3, origin, 2.0f, 1, 2, 0, 0, 0),
		  "an instance far from a merged group gets only the merged light");
	const float near_group[3] = { -90.0f, 0.0f, 0.0f };
	check(list_is(lights, 3, 0, 3, near_group, 2.0f, 2, 0, 1, 0, 0),
		  "an instance near a merged group gets only the group's lights");
	const float at_limit[3] = { -70.0f, 0.0f, 0.0f };
	check(list_is(lights, 3, 0, 3, at_limit, 2.0f, -1, 0, 0, 0, 0),
		  "an instance that is partly near and partly far uses the clusters");
	const float beside[3] = { -100.0f, 0.0f, 300.0f };
	check(list_is(lights, 3, 0, 3, beside, 2.0f, 0, 0, 0, 0, 0),
		  "a merged group that can't reach the instance doesn't prevent a list");

	if (g_failures) {
		printf("%d check%s failed\n", g_failures, g_failures == 1 ? "" : "s");
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
