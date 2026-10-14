/*
 * envmap — HDR environment cube resampling and filtering (host side).
 *
 * Cubes hold linear RGB floats per face, top row first, faces in KTX2
 * order (+X, -X, +Y, -Y, +Z, -Z). Texel directions follow the KTX2 /
 * Vulkan cube-map convention: for face texel (x, y) of an N-pixel face,
 * s = 2 (x + 0.5) / N - 1 and t = 2 (y + 0.5) / N - 1, and
 *
 *   +X: ( 1, -t, -s)    -X: (-1, -t,  s)
 *   +Y: ( s,  1,  t)    -Y: ( s, -1, -t)
 *   +Z: ( s, -t,  1)    -Z: (-s, -t, -1)
 *
 * Directions are in the authored environment frame: +X right, +Y up,
 * -Z forward. An equirectangular panorama maps longitude
 * atan2(x, -z) to u = 0.5 + lon / 2pi (forward at the image centre, right
 * at three quarters of the width) and latitude asin(y) to
 * v = 0.5 - lat / pi (up at the top row).
 *
 * Filtering is deterministic: every output texel is computed
 * independently from fixed sample sequences in a fixed order, so the
 * result does not depend on the worker count.
 */
#ifndef IMGBAKE_ENVMAP_H
#define IMGBAKE_ENVMAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#define ENVMAP_MAX_LEVELS 16
#define ENVMAP_MAX_JOBS 64

typedef struct EnvCube {
	int    size;        /* level-0 face size in pixels */
	int    level_count; /* level l has faces of max(size >> l, 1) pixels */
	float* faces[ENVMAP_MAX_LEVELS][6];
} EnvCube;

/* Runs fn(context, i) for i in [0, count) on up to `jobs` threads (the
 * caller's included). Tasks must be independent. */
typedef void (*EnvmapTaskFn)(void* context, int index);
void envmap_parallel_for(int count, int jobs, EnvmapTaskFn fn, void* context);

/* Levels of a complete chain down to 1 pixel. */
int  envmap_mip_count(int size);
int  envmap_level_size(const EnvCube* cube, int level);
bool envmap_cube_alloc(EnvCube* cube, int size, int level_count);
void envmap_cube_free(EnvCube* cube);

/* Unit direction of a face texel centre. */
void envmap_texel_direction(int face, int x, int y, int size, float out[3]);
/* Solid angle of a face texel, in steradians. */
double envmap_texel_solid_angle(int x, int y, int size);

/* Trilinear sample along `dir` (any length) at fractional `lod`, clamped
 * to the cube's levels. Bilinear filtering clamps at face edges. */
void envmap_sample(const EnvCube* cube, const float dir[3], float lod, float out[3]);

/* Levels 1.. from level 0 by 2x2 box averaging. */
void envmap_build_box_mips(EnvCube* cube);

/* Level 0 from an equirectangular panorama (`w` x `h` RGB floats), with
 * enough subsamples per texel to cover the panorama texels it spans. */
void envmap_from_equirect(EnvCube* cube, const float* rgb, int w, int h, int jobs);

/* Level 0 of `out` = cosine-weighted irradiance / pi of `src`, integrated
 * exactly over a box-reduced copy of `src` (the largest level of at most
 * 64 pixels) using texel solid angles. Requires `src` box mips. */
void envmap_irradiance(EnvCube* out, const EnvCube* src, int jobs);

/* L1 spherical harmonics of level 0's irradiance / pi, ready to evaluate:
 * at unit normal n, irradiance / pi = out[0] + out[1] n.y + out[2] n.z +
 * out[3] n.x, per RGB channel (the basis constants and the cosine lobe,
 * A0 = pi and A1 = 2 pi / 3, are folded in). A constant radiance c gives
 * (c, 0, 0, 0). */
void envmap_project_sh_l1(const EnvCube* src, float out[4][3]);

/* Every level of `out`: level m of M is GGX-prefiltered for perceptual
 * roughness m / (M - 1) under N = V = R, with `samples` Hammersley
 * importance samples per texel read from `src` mips sized to each sample's
 * solid angle. Level 0 is `src` resampled to `out`'s size. Requires `src`
 * box mips. */
void envmap_ggx_prefilter(EnvCube* out, const EnvCube* src, int samples, int jobs);

#ifdef __cplusplus
}
#endif

#endif
