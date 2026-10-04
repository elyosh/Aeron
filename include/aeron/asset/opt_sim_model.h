#ifndef AERON_ASSET_OPT_SIM_MODEL_H
#define AERON_ASSET_OPT_SIM_MODEL_H

/* Converts an AeronFlightModel into per-mesh simulation data in OPT units and
 * OPT payload encodings, for games whose recovered code reads OPT models. It
 * applies no game-specific limits; callers validate those. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/asset/flight_model.h"
#include "aeron/asset/opt_payload.h"
#include "aeron/collision/face_bvh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 65536 OPT units are 1600 metres; conversions are computed in double. */
#define AERON_OPT_SIM_UNITS_PER_METER (65536.0 / 1600.0)

typedef enum AeronOptTrailingBox {
	AERON_OPT_TRAILING_BOX_DESCRIPTOR = 0, /* MESHVERTS tail = descriptor box (XWA) */
	AERON_OPT_TRAILING_BOX_GEOMETRY   = 1, /* MESHVERTS tail = vertex AABB (XvT, XW, TIE) */
} AeronOptTrailingBox;

typedef struct AeronOptSimOptions {
	uint32_t            vertex_sample_count; /* K <= AERON_FACE_BVH_SAMPLE_MAX; 0 = none */
	AeronOptTrailingBox trailing_box;
} AeronOptSimOptions;

typedef struct AeronOptSimMesh {
	uint32_t               vertex_count; /* V, excluding the two trailing box entries */
	float (*vertices)[3];                /* V + 2: vertices, then trailing box min, max */
	float (*normals)[3];                 /* V, parallel to vertices */
	uint32_t               face_count;
	AeronOptFace*          faces;
	AeronOptMeshDescriptor descriptor;
	AeronOptRotScale       rot_scale;
	uint32_t               hardpoint_count;
	AeronOptHardpoint*     hardpoints;
	uint32_t               glow_count;
	AeronOptEngineGlow*    glows;
	AeronFaceBvh*          bvh; /* OPT units; cooked or built at load */
	bool                   bvh_cooked;
} AeronOptSimMesh;

typedef struct AeronOptSimModel {
	uint32_t         mesh_count;
	AeronOptSimMesh* meshes;
} AeronOptSimModel;

/* Deduplicates topology positions by bitwise equality of their float
 * triples, keeping first occurrences. `remap` (position_count entries)
 * receives each position's unique index and `unique` (position_count entries)
 * the unique position indices in first-occurrence order. Returns the unique
 * count, or 0 on failure. */
uint32_t Aeron_FlightTopologyDeduplicate(const AeronFlightTopology* topology, uint32_t* remap, uint32_t* unique);

/* Deterministic farthest-point sample over the unique positions (indices
 * into topology->positions, metres). The seed is the lexicographically
 * smallest (x, y, z); each step takes the largest minimum squared distance,
 * lowest index on ties. Writes min(count, unique_count) unique-vertex
 * ordinals to `sample`; the sequence for a smaller count is a prefix of the
 * sequence for a larger one. Returns the number written, or UINT32_MAX on
 * failure. */
uint32_t Aeron_FlightTopologySample(const AeronFlightTopology* topology, const uint32_t* unique,
									uint32_t unique_count, uint32_t count, uint32_t* sample);

bool Aeron_OptSimModelBuild(const AeronFlightModel* model, const AeronOptSimOptions* options,
							AeronOptSimModel* out, char* error, size_t error_size);

typedef enum AeronOptImageProfile {
	AERON_OPT_IMAGE_PROFILE_V5, /* XWA: version 5, engine glows */
	AERON_OPT_IMAGE_PROFILE_V2, /* XvT, X-Wing 98, TIE98: version 2, no engine glows */
} AeronOptImageProfile;

typedef struct AeronOptImageOptions {
	AeronOptImageProfile profile;
	/* One 8x8 neutral texture in mesh 0, NodeRefs to it in the others. */
	bool placeholder_texture;
} AeronOptImageOptions;

#define AERON_OPT_PLACEHOLDER_TEXTURE_NAME "AeronPlaceholder"
#define AERON_OPT_PLACEHOLDER_TEXTURE_ID   0x0A3E5E01u

/* Writes the standard replacement OPT payload (the bytes after the file's
 * -version/size header): one root per mesh, in mesh order, each holding
 * MESHVERTS, TEXCOORDS, VERTNORMALS, MESHDESC, HARDPOINTs, ENGINEGLOWs (V5
 * only), ROTSCALE and Group[NULL, NULL, NULL, FaceGrouping[LOD 0 Group]]. The
 * LOD group holds the optional placeholder texture and a stub FaceData with
 * no faces: faces stay in the simulation model. `stub_face_data_offsets`
 * (mesh_count entries) receives each stub's payload offset. Call before
 * Aeron_OptSimModelTrimToFaces. The caller frees *payload with free(). */
bool Aeron_OptSimModelWriteImage(const AeronOptSimModel* model, const AeronOptImageOptions* options,
								 uint8_t** payload, int* payload_size, uint32_t* stub_face_data_offsets,
								 char* error, size_t error_size);

/* Frees everything except each mesh's faces and BVH. */
void Aeron_OptSimModelTrimToFaces(AeronOptSimModel* model);
void Aeron_OptSimModelFree(AeronOptSimModel* model);

#ifdef __cplusplus
}
#endif

#endif
