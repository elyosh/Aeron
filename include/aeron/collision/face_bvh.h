#ifndef AERON_COLLISION_FACE_BVH_H
#define AERON_COLLISION_FACE_BVH_H

/* Bounding volume hierarchy over the triangles of one mesh.
 *
 * The BVH only selects candidate triangles; it never evaluates them. Callers
 * run their own per-triangle code on the candidates, in ascending triangle
 * order, so any game whose original code keeps "the accepted triangle with
 * the smallest hit fraction, lowest index on ties" gets results identical to
 * a linear scan.
 *
 * Trees are built deterministically and can be serialized into the cooked
 * layout of the AERON_flight_model extension (`simulationData`/`renderData`),
 * then verified and loaded in linear time. Bounds are stored uninflated; each
 * node also stores the largest 1 / max(|nx|, |ny|, |nz|) of its triangles so a
 * single tree serves any plane tolerance. Box-only trees (render meshes) store
 * 0 there. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AERON_FACE_BVH_COOKED_VERSION 1u
/* Largest leaf; also the leaf size the builder targets. */
#define AERON_FACE_BVH_LEAF_SIZE 8u
/* Vertex-sample length stored in cooked simulation data. */
#define AERON_FACE_BVH_SAMPLE_MAX 256u

/* Strided view of a triangle mesh. Strides are in bytes. `normals` (unit face
 * normals, one per triangle) is required for simulation trees and must be
 * NULL for box-only trees. */
typedef struct AeronFaceBvhGeometry {
	const float*    positions;
	size_t          position_stride;
	uint32_t        position_count;
	const uint32_t* triangles; /* three vertex indices per triangle */
	size_t          triangle_stride;
	uint32_t        triangle_count;
	const float*    normals;
	size_t          normal_stride;
} AeronFaceBvhGeometry;

typedef struct AeronFaceBvhNode {
	float    min[3];
	float    max[3];
	float    max_inv_dominant;
	uint32_t first_or_child; /* leaf: first face_order entry; internal: left child */
	uint32_t count;          /* leaf: face count (> 0); internal: 0 */
} AeronFaceBvhNode;

typedef struct AeronFaceBvh {
	uint32_t          node_count;
	AeronFaceBvhNode* nodes; /* node 0 is the root; children follow their parent */
	uint32_t          face_count;
	uint32_t*         face_order;
	/* Set by AeronFaceBvh_ReorderToLeafOrder: face_order is the identity and
	 * a depth-first walk, left child first, meets the leaves in ascending
	 * face order, so queries need not sort. */
	bool              leaf_ordered;
} AeronFaceBvh;

/* Caller-owned query scratch; zero-initialize before first use. */
typedef struct AeronFaceBvhScratch {
	uint32_t* indices;
	uint32_t  count;
	uint32_t  capacity;
	uint32_t* stack;
	uint32_t  stack_capacity;
} AeronFaceBvhScratch;

/* A slab is the set of points p with min <= dot(p, normal) <= max. */
typedef struct AeronFaceBvhSlab {
	float normal[3];
	float min;
	float max;
} AeronFaceBvhSlab;

/* Builds a simulation tree (geometry->normals required). Returns NULL on
 * allocation failure, an empty mesh, an out-of-range index, a non-finite
 * position or a normal with max(|n_i|) < 1e-6. */
AeronFaceBvh* AeronFaceBvh_Build(const AeronFaceBvhGeometry* geometry);
/* Builds a box-only tree (geometry->normals must be NULL); every triangle is
 * included, degenerate ones too. */
AeronFaceBvh* AeronFaceBvh_BuildBoxes(const AeronFaceBvhGeometry* geometry);
void          AeronFaceBvh_Free(AeronFaceBvh* bvh);
/* Deep copy; NULL for NULL or on allocation failure. */
AeronFaceBvh* AeronFaceBvh_Clone(const AeronFaceBvh* bvh);

/* XXH64 (seed 0) of: position_count (u32), positions (3 x f32), then
 * triangle_count (u32) and triangle indices (3 x u32), all little-endian. */
uint64_t AeronFaceBvh_TopologyHash(const AeronFaceBvhGeometry* geometry);
/* As AeronFaceBvh_TopologyHash, but the count written after the positions is
 * the index count (3 x triangle_count). Used for render meshes. */
uint64_t AeronFaceBvh_MeshHash(const AeronFaceBvhGeometry* geometry);

/* Serializes the tree and an optional vertex sample into the cooked layout.
 * The caller frees *bytes with free(). */
bool AeronFaceBvh_WriteCooked(const AeronFaceBvh* bvh, uint32_t unique_vertex_count, const uint32_t* sample,
							  uint32_t sample_count, uint8_t** bytes, size_t* size);

/* Verifies cooked simulation data against the geometry it describes and
 * loads it. Checks the exact layout size, version and counts, the topology
 * hash, the tree structure, that every leaf lists its triangles in ascending
 * order (queries rely on it), and that every bound contains its triangles and
 * children (conservativeness). On success the caller owns *out_bvh and
 * *out_sample (free()). */
bool AeronFaceBvh_LoadCooked(const uint8_t* bytes, size_t size, uint64_t expected_topology_hash,
							 const AeronFaceBvhGeometry* geometry, uint32_t unique_vertex_count,
							 AeronFaceBvh** out_bvh, uint32_t** out_sample, uint32_t* out_sample_count,
							 char* error, size_t error_size);
/* Verifies and loads a cooked box-only tree (no sample, no normal factors). */
bool AeronFaceBvh_LoadCookedBoxes(const uint8_t* bytes, size_t size, uint64_t expected_mesh_hash,
								  const AeronFaceBvhGeometry* geometry, AeronFaceBvh** out_bvh, char* error,
								  size_t error_size);

/* Scales every bound, e.g. from metres to game units. */
void AeronFaceBvh_ScaleBounds(AeronFaceBvh* bvh, double scale);

/* Renumbers the triangles in leaf order: the leaves, met depth-first with the
 * left child first, get consecutive face ranges, `items` (face_count entries
 * of item_size bytes, indexed by triangle) is permuted to match, and
 * face_order becomes the identity. Queries then return each leaf as one
 * ascending range, with no sorting. The caller keeps its own per-triangle
 * data in the new order; `old_to_new` (face_count entries, or NULL) receives
 * each former triangle index's new index. Returns false on allocation
 * failure, leaving the tree and `items` as they were. */
bool AeronFaceBvh_ReorderToLeafOrder(AeronFaceBvh* bvh, void* items, size_t item_size, uint32_t* old_to_new);

/* Collects the candidate triangles for segment start->end into
 * scratch->indices, unique and ascending, and returns their count. A node
 * passes when the segment meets its box expanded by
 * plane_tolerance * max_inv_dominant + AERON_FACE_BVH_ABSOLUTE_SLACK + q,
 * q = 2^-10 * max(|start|, |end|). Returns UINT32_MAX on allocation failure. */
#define AERON_FACE_BVH_ABSOLUTE_SLACK 1.0 /* in the tree's units after scaling; sized for OPT units */
uint32_t AeronFaceBvh_Collect(const AeronFaceBvh* bvh, const float start[3], const float end[3],
							  float plane_tolerance, AeronFaceBvhScratch* scratch);
/* Collects every triangle in leaves whose box overlaps all slabs (each slab
 * widened by 2^-10 * (|min| + |max|) + 1e-3), unique and ascending. Returns
 * UINT32_MAX on allocation failure. */
uint32_t AeronFaceBvh_CollectSlabs(const AeronFaceBvh* bvh, const AeronFaceBvhSlab* slabs, uint32_t slab_count,
								   AeronFaceBvhScratch* scratch);

void AeronFaceBvhScratch_Free(AeronFaceBvhScratch* scratch);

#ifdef __cplusplus
}
#endif

#endif
