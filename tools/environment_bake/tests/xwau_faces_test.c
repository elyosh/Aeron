/*
 * Checks the face remap of xwau_faces.h: for random directions d, the cube
 * turned to OpenXWA's frame, read at d, must give the XWAU cube read at
 * M d, with M = diag(1, 1, -1).
 *
 *     environment_bake_faces_test [FOLDER...]
 *
 * Without folders, it checks generated faces. Each folder is an XWAU cube
 * folder (for example Effects/CubeMaps/Target), loaded at 64 px.
 */
#include "envmap.h"
#include "xwau_faces.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIRECTIONS 4096
#define FOLDER_SIZE 64

static uint32_t s_seed = 1;

static float next_unit(void) {
	s_seed = s_seed * 1664525u + 1013904223u;
	return (float)(s_seed >> 8) / (float)(1u << 24);
}

/* Returns the number of failures. */
static int check(const char* what, const XwauFaces* faces) {
	EnvCube raw, remapped;
	if (!envmap_cube_alloc(&raw, faces->size, 1) || !envmap_cube_alloc(&remapped, faces->size, 1)) {
		fprintf(stderr, "%s: out of memory\n", what);
		return 1;
	}
	xwau_faces_to_cube(faces, &raw, false);
	xwau_faces_to_cube(faces, &remapped, true);
	double worst     = 0.0;
	int    different = 0;
	for (int i = 0; i < DIRECTIONS; ++i) {
		float d[3];
		for (int k = 0; k < 3; ++k)
			d[k] = 2.0f * next_unit() - 1.0f;
		const float m[3] = { d[0], d[1], -d[2] };
		float       a[3], b[3], c[3];
		envmap_sample(&remapped, d, 0.0f, a);
		envmap_sample(&raw, m, 0.0f, b);
		envmap_sample(&raw, d, 0.0f, c);
		for (int k = 0; k < 3; ++k) {
			worst = fmax(worst, fabs((double)a[k] - b[k]));
			different += fabsf(a[k] - c[k]) > 1e-3f;
		}
	}
	envmap_cube_free(&raw);
	envmap_cube_free(&remapped);
	/* The faces must not be the same seen from d and from M d, or the check
	   would prove nothing. */
	const bool ok = worst <= 1e-5 && different > 0;
	printf("%s %s: %d px, largest difference %.3g over %d directions\n", ok ? "ok" : "FAILED", what,
		   faces->size, worst, DIRECTIONS);
	if (different == 0)
		printf("FAILED %s: the cube looks the same from d and from M d\n", what);
	return ok ? 0 : 1;
}

static int check_generated(void) {
	XwauFaces faces = { .size = 16, .source_size = 16, .channels = 3 };
	for (int face = 0; face < 6; ++face) {
		faces.faces[face] = (float*)malloc((size_t)faces.size * faces.size * 3u * sizeof(float));
		if (!faces.faces[face]) {
			fprintf(stderr, "out of memory\n");
			xwau_faces_free(&faces);
			return 1;
		}
		for (int i = 0; i < faces.size * faces.size * 3; ++i)
			faces.faces[face][i] = next_unit();
	}
	const int failures = check("generated faces", &faces);
	xwau_faces_free(&faces);
	return failures;
}

int main(int argc, char** argv) {
	int failures = check_generated();
	for (int i = 1; i < argc; ++i) {
		XwauFaces faces;
		char      err[512];
		if (!xwau_faces_load(argv[i], FOLDER_SIZE, false, &faces, err, sizeof err)) {
			printf("FAILED %s\n", err);
			++failures;
			continue;
		}
		failures += check(argv[i], &faces);
		xwau_faces_free(&faces);
	}
	return failures ? 1 : 0;
}
