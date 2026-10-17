#ifndef AERON_SCENE_PROBE_SET_H
#define AERON_SCENE_PROBE_SET_H

/*
 * GPU copy of a model's environment probes: the reflection probes (a cube
 * array) and the irradiance grid, placed in the model frame. Where they do
 * not apply, they are blended with the scene's environment. A probe set is
 * built from a flight model's cooked probes (AeronFlightProbes). A scene
 * mesh built from such a model owns one in AeronSceneMesh.probes. Use only
 * on the render thread.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/asset/flight_model.h"
#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- probe cells ------------------------------------------------------ */

enum {
	AERON_PROBE_CELL_PROBES   = 4,  /* probes kept per cell */
	AERON_PROBE_CELL_MAX_AXIS = 64, /* cells per axis */
};

#define AERON_PROBE_CELL_MIN_SIZE 2.0f /* metres */

/* Precomputed lists of the probes that a point can use, so a pixel tests at
 * most AERON_PROBE_CELL_PROBES probes regardless of the probe count. The
 * cells are cubes of size max(AERON_PROBE_CELL_MIN_SIZE, largest extent /
 * AERON_PROBE_CELL_MAX_AXIS) and cover the union of the probes' influence
 * boxes. An ellipsoid uses its bounding box. Each cell stores up to four
 * probes whose influence box overlaps it, smallest volume first, packed in
 * one word: byte k holds the index + 1 of the k-th probe, or 0 when there
 * is none. */
typedef struct AeronProbeCells {
	float     min[3];    /* model frame, metres */
	float     cell_size; /* metres */
	uint32_t  dims[3];
	uint32_t* words;          /* dims[0] * dims[1] * dims[2]; x fastest, then y, then z */
	uint32_t  most_probes;    /* most influence boxes overlapping one cell */
	uint32_t  overflow_cells; /* cells overlapped by more than four boxes; the four smallest are kept */
} AeronProbeCells;

/* Builds the cells of a layout. Returns false when the layout has no probes
 * or memory runs out. Free the result with AeronProbeCells_Free. */
bool AeronProbeCells_Build(const AeronFlightProbeLayout* layout, AeronProbeCells* out);
void AeronProbeCells_Free(AeronProbeCells* cells);

/* ---- probe sets ------------------------------------------------------- */

/* The set buffer, uint4 elements (floats as bits):
 *   [0]            cells min xyz (model frame, metres), 1 / cell size
 *   [1]            cell dims xyz, probe count (uints)
 *   [2 + 6k]       probe k: centre xyz, 1 / falloff
 *   [2 + 6k + 1]   probe k: 1 / half size xyz, cube array layer (uint)
 *   [2 + 6k + 2]   probe k: parallax half size xyz, shapes (uint: bit 0
 *                  influence ellipsoid, bit 1 parallax ellipsoid)
 *   [2 + 6k + 3]   probe k: parallax centre xyz, 0
 *   [2 + 6k + 4]   probe k: grid luminance at the probe centre toward +x,
 *                  -x, +y, -y
 *   [2 + 6k + 5]   probe k: grid luminance toward +z, -z, mean luminance, 0
 *                  (all 0 when the centre is outside the grid)
 *   [2 + 6N ...]   the cell words (AeronProbeCells), four per element, x
 *                  fastest, then y, then z.
 * The grid weight is 1 inside the grid box and falls to 0 at one point
 * spacing outside it. The set's extent includes that margin. */
enum { AERON_PROBE_BUFFER_HEADER = 2, AERON_PROBE_BUFFER_RECORD = 6 };

typedef struct AeronProbeSet {
	AeronFlightProbeLayout layout;
	/* `cubes` is the probe cube array (probe k in layer k). `grid` is the
	 * irradiance grid as a 3D RGBA16F texture of rx x ry x 6 rz texels.
	 * Slab f, for f = 0 to 5, starts at z = f rz. It holds irradiance / pi
	 * toward +x, -x, +y, -y, +z, -z (in that order of f) in RGB and the
	 * luminance of the mean radiance in A. `buffer` is the set buffer
	 * described above. */
	AeronTexture* cubes;
	AeronTexture* grid;
	AeronBuffer*  buffer;
	AeronSampler* sampler; /* clamp-to-edge trilinear */
	uint32_t      mip_count;
	/* Extent of the set in the model frame, in metres. It covers the probes'
	 * influence boxes and the grid box plus the grid fade margin. */
	float    bounds_min[3];
	float    bounds_max[3];
	uint64_t bytes; /* total size of the textures and the buffer */
	uint32_t overflow_cells;
} AeronProbeSet;

/* Uploads `probes` with `cmd`. Returns NULL on failure, for example when the
 * device cannot sample BC6H or RGBA16F, and logs the failure with `name`. */
AeronProbeSet* AeronProbeSet_Create(AeronCommandBuffer* cmd, const AeronFlightProbes* probes,
									const char* name);
void           AeronProbeSet_Destroy(AeronProbeSet* set);

#ifdef __cplusplus
}
#endif

#endif
