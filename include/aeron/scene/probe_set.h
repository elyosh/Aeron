#ifndef AERON_SCENE_PROBE_SET_H
#define AERON_SCENE_PROBE_SET_H

/*
 * A model's environment probes on the GPU: its reflection probes (a cube
 * array) and its irradiance grid, placed in the model's frame, blended with
 * the scene's environment where they do not apply. A probe set is built
 * from a flight model's cooked probes (AeronFlightProbes); a scene mesh
 * built from such a model owns one (AeronSceneMesh.probes). Render-thread
 * only.
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

/* Which probes a point may use, precomputed so a pixel tests at most
 * AERON_PROBE_CELL_PROBES whatever the probe count. The cells cover the
 * union of the probes' influence boxes (an ellipsoid counts as its box)
 * with cubes of max(AERON_PROBE_CELL_MIN_SIZE, largest extent /
 * AERON_PROBE_CELL_MAX_AXIS). A cell lists the probes whose influence box
 * overlaps it, smallest volume first, at most four, in one word: byte k
 * holds the k-th probe's index + 1, 0 when none. */
typedef struct AeronProbeCells {
	float     min[3];    /* model frame, metres */
	float     cell_size; /* metres */
	uint32_t  dims[3];
	uint32_t* words;          /* dims[0] * dims[1] * dims[2]; x fastest, then y, then z */
	uint32_t  most_probes;    /* most influence boxes overlapping one cell */
	uint32_t  overflow_cells; /* cells overlapped by more than four (the four smallest kept) */
} AeronProbeCells;

/* Builds the cells of a layout; false when it has no probes or memory runs
 * out. Free with AeronProbeCells_Free. */
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
 *   [2 + 6k + 4]   probe k: the grid's light at its centre, as luminance:
 *                  toward +x, -x, +y, -y
 *   [2 + 6k + 5]   probe k: toward +z, -z, the mean, 0 (all 0 when the
 *                  centre is outside the grid)
 *   [2 + 6N ...]   the cell words (AeronProbeCells), four per element, x
 *                  fastest, then y, then z.
 * The grid weighs 1 inside its box and falls to 0 one point spacing
 * outside it, so the set's extent includes that margin. */
enum { AERON_PROBE_BUFFER_HEADER = 2, AERON_PROBE_BUFFER_RECORD = 6 };

typedef struct AeronProbeSet {
	AeronFlightProbeLayout layout;
	/* The probes' cube array (probe k in layer k), the irradiance grid as a
	 * 3D RGBA16F texture of rx x ry x 6 rz texels (slab f, z from f rz,
	 * holds the irradiance / pi toward +x, -x, +y, -y, +z, -z for f = 0 to
	 * 5 in RGB, and the luminance of the mean radiance in A), and the set
	 * buffer above. */
	AeronTexture* cubes;
	AeronTexture* grid;
	AeronBuffer*  buffer;
	AeronSampler* sampler; /* clamp-to-edge trilinear */
	uint32_t      mip_count;
	/* The set's extent in the model frame, metres: its probes' influence
	 * boxes and its grid box with the grid's fade. */
	float    bounds_min[3];
	float    bounds_max[3];
	uint64_t bytes; /* the textures' and the buffer's sizes */
	uint32_t overflow_cells;
} AeronProbeSet;

/* Uploads `probes` with `cmd`; NULL on failure (logged with `name`), such as
 * a device without BC6H or RGBA16F sampling. */
AeronProbeSet* AeronProbeSet_Create(AeronCommandBuffer* cmd, const AeronFlightProbes* probes,
									const char* name);
void           AeronProbeSet_Destroy(AeronProbeSet* set);

#ifdef __cplusplus
}
#endif

#endif
