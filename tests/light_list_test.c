/*
 * Tests light lists (src/scene/light_list.h): which lights go in an
 * instance's list, when the instance must use the light clusters, and how
 * far lights are combined.
 *
 * Built with the tools as aeron_light_list_test. It needs no window and no
 * GPU. It prints each check and exits with a non-zero status when a check
 * fails.
 */

#include <math.h>
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
	const uint32_t        want[AERON_SCENE_LIGHT_LIST_MAX] = { a, b, c, d };
	uint32_t              indices[AERON_SCENE_LIGHT_LIST_MAX];
	AeronSceneFarLightGPU far[AERON_SCENE_LIGHT_LIST_FAR_MAX];
	uint32_t              far_count = 99;
	const int listed = AeronSceneLightList_Build(lights, NULL, count, spot_start, tube_start, center, NULL,
												 radius, NULL, indices, far, &far_count);
	if (listed != expected || (listed >= 0 && far_count != 0)) {
		return 0;
	}
	for (int k = 0; k < listed; ++k) {
		if (indices[k] != want[k]) {
			return 0;
		}
	}
	return 1;
}

/* The lights of the far-light checks: the cluster build's view of each
 * light and the shaders' view, in a space where view and world are the
 * same. */
static AeronSceneClusterLightGPU g_cluster[16];
static AeronScenePointLightGPU   g_shaded[16];
static uint32_t                  g_scratch[16];

/* Makes light `i` a sphere at (x, y, z) with colour (r, g, b) and radius
 * 0.1, reaching up to `range`. */
static void shaded_sphere(int i, float x, float y, float z, float range, float r, float g, float b) {
	g_cluster[i] = sphere(x, y, z, range);
	memset(&g_shaded[i], 0, sizeof g_shaded[i]);
	g_shaded[i].position_range[0] = x;
	g_shaded[i].position_range[1] = y;
	g_shaded[i].position_range[2] = z;
	g_shaded[i].position_range[3] = 1.0f / (range * range);
	g_shaded[i].color[0]          = r;
	g_shaded[i].color[1]          = g;
	g_shaded[i].color[2]          = b;
	g_shaded[i].color[3]          = 0.1f;
}

/* Makes light `i` a white tube centred on (x, y, z), along X, with a half
 * length of `half_length`, reaching up to `range` from its segment. A
 * negative `thickness` makes it one-sided, emitting along -Y. */
static void shaded_tube(int i, float x, float y, float z, float half_length, float range, float thickness) {
	shaded_sphere(i, x, y, z, range, 1.0f, 1.0f, 1.0f);
	g_cluster[i].view_position_range[3] = range + half_length;
	g_shaded[i].half_axis[0]            = half_length;
	g_shaded[i].half_axis[3]            = half_length;
	g_shaded[i].direction[3]            = 1.0f / half_length;
	g_shaded[i].color[3]                = thickness;
	if (thickness < 0.0f) {
		g_shaded[i].direction[1] = -1.0f;
	}
}

static int close_to(float a, float b) { return fabsf(a - b) <= 1e-4f * fmaxf(fabsf(a), fabsf(b)) + 1e-12f; }

/* Builds the list of an instance of radius `radius` at the origin, from
 * lights 0 to count - 1. The tubes start at index `tube_start`. Returns the
 * number of near lights, and writes the combined lights to `far` and
 * `far_count`. */
static int far_build(uint32_t count, uint32_t tube_start, float radius, AeronSceneFarLightGPU far[2],
					 uint32_t* far_count) {
	const float origin[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t    indices[AERON_SCENE_LIGHT_LIST_MAX];
	return AeronSceneLightList_Build(g_cluster, g_shaded, count, tube_start, tube_start, origin, origin,
									 radius, g_scratch, indices, far, far_count);
}

static void far_light_checks(void) {
	AeronSceneFarLightGPU far[AERON_SCENE_LIGHT_LIST_FAR_MAX];
	uint32_t              far_count = 0;

	/* One far sphere, 100 above an instance of radius 2. Its window at the
	 * centre is 1 - (100 / 1000)^4. */
	shaded_sphere(0, 0.0f, 100.0f, 0.0f, 1000.0f, 1.0e4f, 2.0e4f, 3.0e4f);
	const float scale = (1.0f - 1.0e-4f) / (100.0f * 100.0f);
	check(far_build(1, 1, 2.0f, far, &far_count) == 0 && far_count == 1,
		  "a far light is combined, not listed");
	check(close_to(far[0].direction_spread[1], 1.0f) && close_to(far[0].diffuse[0], 1.0e4f * scale) &&
			  close_to(far[0].diffuse[2], 3.0e4f * scale) && close_to(far[0].specular[1], 2.0e4f * scale) &&
			  close_to(far[0].direction_spread[3], 0.1f / 200.0f),
		  "one far light is combined as itself");

	/* The same light 20 away is nearer than 16 times the radius. */
	shaded_sphere(0, 0.0f, 20.0f, 0.0f, 1000.0f, 1.0f, 1.0f, 1.0f);
	check(far_build(1, 1, 2.0f, far, &far_count) == 1 && far_count == 0,
		  "a light near the instance is listed");

	/* Twelve far lights and one near light: the list doesn't overflow. */
	for (int i = 0; i < 12; ++i) {
		shaded_sphere(i, 10.0f * (float)i, 200.0f, 0.0f, 1000.0f, 1.0f, 1.0f, 1.0f);
	}
	shaded_sphere(12, 0.0f, 0.0f, 5.0f, 100.0f, 1.0f, 1.0f, 1.0f);
	check(far_build(13, 13, 2.0f, far, &far_count) == 1 && far_count == 1,
		  "many far lights leave room for the near ones");
	float sum = 0.0f;
	for (int i = 0; i < 12; ++i) {
		const float x = 10.0f * (float)i;
		const float d = sqrtf(x * x + 200.0f * 200.0f);
		const float w = 1.0f - (d / 1000.0f) * (d / 1000.0f) * (d / 1000.0f) * (d / 1000.0f);
		sum += w / (d * d);
	}
	check(close_to(far[0].specular[0], sum), "the highlight takes the sum of the far lights");
	check(far[0].diffuse[0] <= far[0].specular[0] && far[0].diffuse[0] > 0.95f * far[0].specular[0],
		  "lights from close directions keep almost all their diffuse");

	/* Far lights above and below go to two combined lights. */
	shaded_sphere(0, 0.0f, 200.0f, 0.0f, 1000.0f, 2.0f, 2.0f, 2.0f);
	shaded_sphere(1, 0.0f, -200.0f, 0.0f, 1000.0f, 1.0f, 1.0f, 1.0f);
	check(far_build(2, 2, 2.0f, far, &far_count) == 0 && far_count == 2 &&
			  close_to(far[0].direction_spread[1], 1.0f) && close_to(far[1].direction_spread[1], -1.0f),
		  "far lights from opposite sides don't cancel out");

	/* A far light whose range ends before the centre gives nothing, and
	 * isn't listed. */
	shaded_sphere(0, 0.0f, 100.0f, 0.0f, 99.0f, 1.0f, 1.0f, 1.0f);
	g_cluster[0].view_position_range[3] = 101.0f;
	check(far_build(1, 1, 2.0f, far, &far_count) == 0 && far_count == 0,
		  "a far light out of range at the centre adds nothing");

	/* Tubes: far from 16 times the radius and 8 times the half length. */
	shaded_tube(0, 0.0f, 300.0f, 0.0f, 50.0f, 2000.0f, 0.1f);
	check(far_build(1, 0, 2.0f, far, &far_count) == 1 && far_count == 0,
		  "a long tube stays near until 8 half lengths away");
	shaded_tube(0, 0.0f, 1000.0f, 0.0f, 50.0f, 2000.0f, 0.1f);
	check(far_build(1, 0, 2.0f, far, &far_count) == 0 && far_count == 1 &&
			  close_to(far[0].direction_spread[3], (0.1f + 50.0f) / 2000.0f),
		  "a far tube is combined, widened by its length across the view");
	shaded_tube(0, 0.0f, 1000.0f, 0.0f, 50.0f, 2000.0f, -0.1f);
	check(far_build(1, 0, 2.0f, far, &far_count) == 1 && far_count == 0,
		  "a one-sided tube is never combined");
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

	far_light_checks();

	if (g_failures) {
		printf("%d check%s failed\n", g_failures, g_failures == 1 ? "" : "s");
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
