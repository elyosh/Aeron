#include "aeron/collision/face_bvh.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== Geometry access =============================================== */

static const float* geometry_position(const AeronFaceBvhGeometry* geometry, uint32_t index) {
	return (const float*)((const uint8_t*)geometry->positions + (size_t)index * geometry->position_stride);
}

static const uint32_t* geometry_triangle(const AeronFaceBvhGeometry* geometry, uint32_t index) {
	return (const uint32_t*)((const uint8_t*)geometry->triangles + (size_t)index * geometry->triangle_stride);
}

static const float* geometry_normal(const AeronFaceBvhGeometry* geometry, uint32_t index) {
	return (const float*)((const uint8_t*)geometry->normals + (size_t)index * geometry->normal_stride);
}

static bool face_bounds(const AeronFaceBvhGeometry* geometry, uint32_t face, float min[3], float max[3]) {
	const uint32_t* triangle = geometry_triangle(geometry, face);
	for (int corner = 0; corner < 3; ++corner) {
		if (triangle[corner] >= geometry->position_count)
			return false;
		const float* position = geometry_position(geometry, triangle[corner]);
		for (int axis = 0; axis < 3; ++axis) {
			if (!isfinite(position[axis]))
				return false;
			if (corner == 0 || position[axis] < min[axis])
				min[axis] = position[axis];
			if (corner == 0 || position[axis] > max[axis])
				max[axis] = position[axis];
		}
	}
	return true;
}

/* 1 / max(|nx|, |ny|, |nz|), computed identically by the builder and the
 * cooked-data verifier. */
static bool face_inv_dominant(const AeronFaceBvhGeometry* geometry, uint32_t face, float* out) {
	const float* normal   = geometry_normal(geometry, face);
	float        dominant = fabsf(normal[0]);
	if (fabsf(normal[1]) > dominant)
		dominant = fabsf(normal[1]);
	if (fabsf(normal[2]) > dominant)
		dominant = fabsf(normal[2]);
	if (!isfinite(dominant) || !(dominant >= 1.0e-6f))
		return false;
	*out = (float)(1.0 / (double)dominant);
	return true;
}

static bool geometry_valid(const AeronFaceBvhGeometry* geometry, bool box_only) {
	return geometry && geometry->positions && geometry->triangles && geometry->position_count &&
		   geometry->triangle_count && (box_only ? geometry->normals == NULL : geometry->normals != NULL);
}

/* ===== Build ========================================================= */

typedef struct BuildFace {
	float  min[3];
	float  max[3];
	double centroid[3];
	float  inv_dominant;
} BuildFace;

typedef struct BuildRange {
	uint32_t start;
	uint32_t count;
} BuildRange;

/* Total order on faces along one axis: (centroid, face index). */
static bool face_less(const BuildFace* faces, int axis, uint32_t a, uint32_t b) {
	const double ka = faces[a].centroid[axis];
	const double kb = faces[b].centroid[axis];
	return ka < kb || (ka == kb && a < b);
}

static void sift_down(const BuildFace* faces, int axis, uint32_t* items, uint32_t root, uint32_t count) {
	for (;;) {
		uint32_t child = 2 * root + 1;
		if (child >= count)
			return;
		if (child + 1 < count && face_less(faces, axis, items[child], items[child + 1]))
			++child;
		if (!face_less(faces, axis, items[root], items[child]))
			return;
		const uint32_t swap = items[root];
		items[root]         = items[child];
		items[child]        = swap;
		root                = child;
	}
}

static void heap_sort(const BuildFace* faces, int axis, uint32_t* items, uint32_t count) {
	for (uint32_t index = count / 2; index-- > 0;)
		sift_down(faces, axis, items, index, count);
	for (uint32_t end = count; end-- > 1;) {
		const uint32_t swap = items[0];
		items[0]            = items[end];
		items[end]          = swap;
		sift_down(faces, axis, items, 0, end);
	}
}

/* Reorders items so the first `k` are the k smallest under face_less. Keys
 * are unique, so the resulting split is the same for any correct selection;
 * a heap sort bounds the worst case. */
static void select_smallest(const BuildFace* faces, int axis, uint32_t* items, uint32_t count, uint32_t k) {
	uint32_t lo     = 0;
	uint32_t hi     = count;
	uint32_t budget = 64;
	while (hi - lo > 1) {
		if (budget-- == 0) {
			heap_sort(faces, axis, items + lo, hi - lo);
			return;
		}
		/* Median of three as the pivot. */
		const uint32_t mid = lo + (hi - lo) / 2;
		uint32_t       a = items[lo], b = items[mid], c = items[hi - 1];
		uint32_t       pivot;
		if (face_less(faces, axis, a, b))
			pivot = face_less(faces, axis, b, c) ? b : (face_less(faces, axis, a, c) ? c : a);
		else
			pivot = face_less(faces, axis, a, c) ? a : (face_less(faces, axis, b, c) ? c : b);
		/* Three-way split: [lo, less) < pivot, [less, greater) == pivot, rest > pivot. */
		uint32_t less = lo, index = lo, greater = hi;
		while (index < greater) {
			if (face_less(faces, axis, items[index], pivot)) {
				const uint32_t swap = items[less];
				items[less++]       = items[index];
				items[index++]      = swap;
			} else if (face_less(faces, axis, pivot, items[index])) {
				const uint32_t swap = items[--greater];
				items[greater]      = items[index];
				items[index]        = swap;
			} else {
				++index;
			}
		}
		if (k < less)
			hi = less;
		else if (k >= greater)
			lo = greater;
		else
			return;
	}
}

static int compare_u32(const void* a, const void* b) {
	const uint32_t x = *(const uint32_t*)a;
	const uint32_t y = *(const uint32_t*)b;
	return (x > y) - (x < y);
}

static AeronFaceBvh* build_tree(const AeronFaceBvhGeometry* geometry, bool box_only) {
	if (!geometry_valid(geometry, box_only))
		return NULL;
	const uint32_t face_count = geometry->triangle_count;
	if (face_count > UINT32_MAX / 2)
		return NULL;
	const uint32_t node_capacity = 2 * face_count;
	BuildFace*     faces         = malloc((size_t)face_count * sizeof *faces);
	BuildRange*    ranges        = malloc((size_t)node_capacity * sizeof *ranges);
	AeronFaceBvh*  bvh           = calloc(1, sizeof *bvh);
	if (!faces || !ranges || !bvh)
		goto fail;
	bvh->nodes      = calloc(node_capacity, sizeof *bvh->nodes);
	bvh->face_order = malloc((size_t)face_count * sizeof *bvh->face_order);
	if (!bvh->nodes || !bvh->face_order)
		goto fail;
	bvh->face_count = face_count;

	for (uint32_t face = 0; face < face_count; ++face) {
		BuildFace* build = &faces[face];
		if (!face_bounds(geometry, face, build->min, build->max))
			goto fail;
		const uint32_t* triangle = geometry_triangle(geometry, face);
		for (int axis = 0; axis < 3; ++axis) {
			double sum = 0.0;
			for (int corner = 0; corner < 3; ++corner)
				sum += (double)geometry_position(geometry, triangle[corner])[axis];
			build->centroid[axis] = sum / 3.0;
		}
		build->inv_dominant = 0.0f;
		if (!box_only && !face_inv_dominant(geometry, face, &build->inv_dominant))
			goto fail;
		bvh->face_order[face] = face;
	}

	/* Breadth-first: nodes are processed in index order and children are
	 * appended, so every child index exceeds its parent's. */
	ranges[0]       = (BuildRange) { 0, face_count };
	bvh->node_count = 1;
	for (uint32_t node_index = 0; node_index < bvh->node_count; ++node_index) {
		AeronFaceBvhNode* node  = &bvh->nodes[node_index];
		const BuildRange  range = ranges[node_index];
		uint32_t*         items = bvh->face_order + range.start;
		double            centroid_min[3], centroid_max[3];
		node->max_inv_dominant = 0.0f;
		for (uint32_t item = 0; item < range.count; ++item) {
			const BuildFace* face = &faces[items[item]];
			for (int axis = 0; axis < 3; ++axis) {
				if (item == 0 || face->min[axis] < node->min[axis])
					node->min[axis] = face->min[axis];
				if (item == 0 || face->max[axis] > node->max[axis])
					node->max[axis] = face->max[axis];
				if (item == 0 || face->centroid[axis] < centroid_min[axis])
					centroid_min[axis] = face->centroid[axis];
				if (item == 0 || face->centroid[axis] > centroid_max[axis])
					centroid_max[axis] = face->centroid[axis];
			}
			if (face->inv_dominant > node->max_inv_dominant)
				node->max_inv_dominant = face->inv_dominant;
		}
		if (range.count <= AERON_FACE_BVH_LEAF_SIZE) {
			/* Leaves list their faces ascending, so cooked trees are canonical. */
			qsort(items, range.count, sizeof *items, compare_u32);
			node->first_or_child = range.start;
			node->count          = range.count;
			continue;
		}
		int axis = 0;
		for (int candidate = 1; candidate < 3; ++candidate)
			if (centroid_max[candidate] - centroid_min[candidate] > centroid_max[axis] - centroid_min[axis])
				axis = candidate;
		const uint32_t left_count = range.count / 2;
		select_smallest(faces, axis, items, range.count, left_count);
		const uint32_t left = bvh->node_count;
		if (left + 2 > node_capacity)
			goto fail;
		ranges[left]         = (BuildRange) { range.start, left_count };
		ranges[left + 1]     = (BuildRange) { range.start + left_count, range.count - left_count };
		bvh->node_count      = left + 2;
		node->first_or_child = left;
		node->count          = 0;
	}

	AeronFaceBvhNode* shrunk = realloc(bvh->nodes, (size_t)bvh->node_count * sizeof *bvh->nodes);
	if (shrunk)
		bvh->nodes = shrunk;
	free(faces);
	free(ranges);
	return bvh;

fail:
	free(faces);
	free(ranges);
	AeronFaceBvh_Free(bvh);
	return NULL;
}

AeronFaceBvh* AeronFaceBvh_Build(const AeronFaceBvhGeometry* geometry) { return build_tree(geometry, false); }

AeronFaceBvh* AeronFaceBvh_BuildBoxes(const AeronFaceBvhGeometry* geometry) { return build_tree(geometry, true); }

void AeronFaceBvh_Free(AeronFaceBvh* bvh) {
	if (!bvh)
		return;
	free(bvh->nodes);
	free(bvh->face_order);
	free(bvh);
}

AeronFaceBvh* AeronFaceBvh_Clone(const AeronFaceBvh* bvh) {
	if (!bvh)
		return NULL;
	AeronFaceBvh* copy = calloc(1, sizeof *copy);
	if (!copy)
		return NULL;
	copy->node_count   = bvh->node_count;
	copy->face_count   = bvh->face_count;
	copy->leaf_ordered = bvh->leaf_ordered;
	copy->nodes      = malloc((size_t)bvh->node_count * sizeof *copy->nodes);
	copy->face_order = malloc((size_t)bvh->face_count * sizeof *copy->face_order);
	if (!copy->nodes || !copy->face_order) {
		AeronFaceBvh_Free(copy);
		return NULL;
	}
	memcpy(copy->nodes, bvh->nodes, (size_t)bvh->node_count * sizeof *copy->nodes);
	memcpy(copy->face_order, bvh->face_order, (size_t)bvh->face_count * sizeof *copy->face_order);
	return copy;
}

/* ===== Hashing ======================================================= */

static void put_u32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static void put_f32(uint8_t* out, float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	put_u32(out, bits);
}

static uint32_t get_u32(const uint8_t* in) {
	return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static float get_f32(const uint8_t* in) {
	const uint32_t bits = get_u32(in);
	float          value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

/* Hashes the counts, positions and triangle indices in little-endian order,
 * staging them through a fixed buffer. */
static uint64_t geometry_hash(const AeronFaceBvhGeometry* geometry, uint32_t second_count) {
	enum { CHUNK = 1024 };
	uint8_t       buffer[CHUNK * 12];
	XXH64_state_t state;
	XXH64_reset(&state, 0);
	put_u32(buffer, geometry->position_count);
	XXH64_update(&state, buffer, 4);
	for (uint32_t first = 0; first < geometry->position_count; first += CHUNK) {
		const uint32_t count = geometry->position_count - first < CHUNK ? geometry->position_count - first : CHUNK;
		for (uint32_t index = 0; index < count; ++index) {
			const float* position = geometry_position(geometry, first + index);
			for (int axis = 0; axis < 3; ++axis)
				put_f32(buffer + index * 12 + axis * 4, position[axis]);
		}
		XXH64_update(&state, buffer, (size_t)count * 12);
	}
	put_u32(buffer, second_count);
	XXH64_update(&state, buffer, 4);
	for (uint32_t first = 0; first < geometry->triangle_count; first += CHUNK) {
		const uint32_t count = geometry->triangle_count - first < CHUNK ? geometry->triangle_count - first : CHUNK;
		for (uint32_t index = 0; index < count; ++index) {
			const uint32_t* triangle = geometry_triangle(geometry, first + index);
			for (int corner = 0; corner < 3; ++corner)
				put_u32(buffer + index * 12 + corner * 4, triangle[corner]);
		}
		XXH64_update(&state, buffer, (size_t)count * 12);
	}
	return XXH64_digest(&state);
}

uint64_t AeronFaceBvh_TopologyHash(const AeronFaceBvhGeometry* geometry) {
	return geometry_hash(geometry, geometry->triangle_count);
}

uint64_t AeronFaceBvh_MeshHash(const AeronFaceBvhGeometry* geometry) {
	return geometry_hash(geometry, geometry->triangle_count * 3u);
}

/* ===== Cooked layout ================================================= */

enum { COOKED_HEADER_BYTES = 20, COOKED_NODE_BYTES = 36 };

bool AeronFaceBvh_WriteCooked(const AeronFaceBvh* bvh, uint32_t unique_vertex_count, const uint32_t* sample,
							  uint32_t sample_count, uint8_t** bytes, size_t* size) {
	if (!bvh || !bytes || !size || (sample_count && !sample))
		return false;
	const uint64_t total = COOKED_HEADER_BYTES + (uint64_t)bvh->node_count * COOKED_NODE_BYTES +
						   (uint64_t)bvh->face_count * 4 + (uint64_t)sample_count * 4;
	if (total > SIZE_MAX)
		return false;
	uint8_t* out = malloc((size_t)total);
	if (!out)
		return false;
	uint8_t* cursor = out;
	put_u32(cursor + 0, AERON_FACE_BVH_COOKED_VERSION);
	put_u32(cursor + 4, bvh->face_count);
	put_u32(cursor + 8, unique_vertex_count);
	put_u32(cursor + 12, bvh->node_count);
	put_u32(cursor + 16, sample_count);
	cursor += COOKED_HEADER_BYTES;
	for (uint32_t index = 0; index < bvh->node_count; ++index) {
		const AeronFaceBvhNode* node = &bvh->nodes[index];
		for (int axis = 0; axis < 3; ++axis) {
			put_f32(cursor + axis * 4, node->min[axis]);
			put_f32(cursor + 12 + axis * 4, node->max[axis]);
		}
		put_f32(cursor + 24, node->max_inv_dominant);
		put_u32(cursor + 28, node->first_or_child);
		put_u32(cursor + 32, node->count);
		cursor += COOKED_NODE_BYTES;
	}
	for (uint32_t index = 0; index < bvh->face_count; ++index, cursor += 4)
		put_u32(cursor, bvh->face_order[index]);
	for (uint32_t index = 0; index < sample_count; ++index, cursor += 4)
		put_u32(cursor, sample[index]);
	*bytes = out;
	*size  = (size_t)total;
	return true;
}

static bool load_error(char* error, size_t error_size, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s", message);
	return false;
}

static bool box_contains(const float outer_min[3], const float outer_max[3], const float inner_min[3],
						 const float inner_max[3]) {
	for (int axis = 0; axis < 3; ++axis)
		if (inner_min[axis] < outer_min[axis] || inner_max[axis] > outer_max[axis])
			return false;
	return true;
}

typedef struct VerifyItem {
	uint32_t node;
	uint32_t parent; /* UINT32_MAX for the root */
} VerifyItem;

/* Single top-down pass over the tree: structure, coverage and
 * conservativeness. */
static bool verify_tree(const AeronFaceBvh* bvh, const AeronFaceBvhGeometry* geometry, bool box_only, char* error,
						size_t error_size) {
	bool        ok           = false;
	uint8_t*    node_visited = calloc(bvh->node_count, 1);
	uint8_t*    face_seen    = calloc(bvh->face_count, 1);
	VerifyItem* stack        = malloc((size_t)bvh->node_count * sizeof *stack);
	if (!node_visited || !face_seen || !stack) {
		load_error(error, error_size, "out of memory verifying cooked BVH");
		goto done;
	}
	uint32_t depth = 0, visited = 0, seen = 0;
	stack[depth++] = (VerifyItem) { 0, UINT32_MAX };
	while (depth) {
		const VerifyItem        item = stack[--depth];
		const AeronFaceBvhNode* node = &bvh->nodes[item.node];
		if (node_visited[item.node]) {
			load_error(error, error_size, "cooked BVH node referenced twice");
			goto done;
		}
		node_visited[item.node] = 1;
		++visited;
		for (int axis = 0; axis < 3; ++axis) {
			if (!isfinite(node->min[axis]) || !isfinite(node->max[axis]) || node->min[axis] > node->max[axis]) {
				load_error(error, error_size, "cooked BVH node has invalid bounds");
				goto done;
			}
		}
		if (!isfinite(node->max_inv_dominant) || node->max_inv_dominant < 0.0f ||
			(box_only && node->max_inv_dominant != 0.0f)) {
			load_error(error, error_size, "cooked BVH node has an invalid normal factor");
			goto done;
		}
		if (item.parent != UINT32_MAX) {
			const AeronFaceBvhNode* parent = &bvh->nodes[item.parent];
			if (!box_contains(parent->min, parent->max, node->min, node->max) ||
				node->max_inv_dominant > parent->max_inv_dominant) {
				load_error(error, error_size, "cooked BVH node is not contained by its parent");
				goto done;
			}
		}
		if (node->count == 0) {
			const uint32_t left = node->first_or_child;
			if (left <= item.node || left >= bvh->node_count - 1) {
				load_error(error, error_size, "cooked BVH node has an invalid child index");
				goto done;
			}
			if (depth + 2 > bvh->node_count) {
				load_error(error, error_size, "cooked BVH has too many child references");
				goto done;
			}
			stack[depth++] = (VerifyItem) { left, item.node };
			stack[depth++] = (VerifyItem) { left + 1, item.node };
			continue;
		}
		if ((uint64_t)node->first_or_child + node->count > bvh->face_count) {
			load_error(error, error_size, "cooked BVH leaf range is out of bounds");
			goto done;
		}
		for (uint32_t entry = 0; entry < node->count; ++entry) {
			const uint32_t face = bvh->face_order[node->first_or_child + entry];
			float          face_min[3], face_max[3], inv_dominant;
			if (face >= bvh->face_count || face_seen[face]) {
				load_error(error, error_size, "cooked BVH face order is not a permutation");
				goto done;
			}
			if (entry > 0 && face <= bvh->face_order[node->first_or_child + entry - 1]) {
				load_error(error, error_size, "cooked BVH leaf does not list its triangles in ascending order");
				goto done;
			}
			face_seen[face] = 1;
			++seen;
			if (!face_bounds(geometry, face, face_min, face_max) ||
				!box_contains(node->min, node->max, face_min, face_max)) {
				load_error(error, error_size, "cooked BVH leaf does not contain its triangle");
				goto done;
			}
			if (!box_only && (!face_inv_dominant(geometry, face, &inv_dominant) ||
							  inv_dominant > node->max_inv_dominant)) {
				load_error(error, error_size, "cooked BVH leaf normal factor is too small");
				goto done;
			}
		}
	}
	if (visited != bvh->node_count || seen != bvh->face_count) {
		load_error(error, error_size, "cooked BVH does not cover every node and triangle");
		goto done;
	}
	ok = true;
done:
	free(node_visited);
	free(face_seen);
	free(stack);
	return ok;
}

static bool load_cooked(const uint8_t* bytes, size_t size, uint64_t expected_hash,
						const AeronFaceBvhGeometry* geometry, bool box_only, uint32_t unique_vertex_count,
						AeronFaceBvh** out_bvh, uint32_t** out_sample, uint32_t* out_sample_count, char* error,
						size_t error_size) {
	if (!bytes || !out_bvh || !geometry_valid(geometry, box_only))
		return load_error(error, error_size, "invalid cooked BVH arguments");
	if (size < COOKED_HEADER_BYTES)
		return load_error(error, error_size, "cooked BVH is truncated");
	const uint32_t version      = get_u32(bytes + 0);
	const uint32_t face_count   = get_u32(bytes + 4);
	const uint32_t vertex_count = get_u32(bytes + 8);
	const uint32_t node_count   = get_u32(bytes + 12);
	const uint32_t sample_count = get_u32(bytes + 16);
	if (version != AERON_FACE_BVH_COOKED_VERSION)
		return load_error(error, error_size, "unsupported cooked BVH version");
	const uint32_t expected_vertices = box_only ? geometry->position_count : unique_vertex_count;
	const uint32_t expected_samples =
		box_only ? 0u : (unique_vertex_count < AERON_FACE_BVH_SAMPLE_MAX ? unique_vertex_count : AERON_FACE_BVH_SAMPLE_MAX);
	if (face_count != geometry->triangle_count || vertex_count != expected_vertices ||
		sample_count != expected_samples || node_count == 0)
		return load_error(error, error_size, "cooked BVH counts do not match the geometry");
	const uint64_t expected_size = COOKED_HEADER_BYTES + (uint64_t)node_count * COOKED_NODE_BYTES +
								   (uint64_t)face_count * 4 + (uint64_t)sample_count * 4;
	if ((uint64_t)size != expected_size)
		return load_error(error, error_size, "cooked BVH size does not match its counts");
	const uint64_t hash = box_only ? AeronFaceBvh_MeshHash(geometry) : AeronFaceBvh_TopologyHash(geometry);
	if (hash != expected_hash)
		return load_error(error, error_size, "cooked BVH is stale: geometry hash mismatch");

	AeronFaceBvh* bvh    = calloc(1, sizeof *bvh);
	uint32_t*     sample = NULL;
	if (!bvh)
		return load_error(error, error_size, "out of memory loading cooked BVH");
	bvh->node_count = node_count;
	bvh->face_count = face_count;
	bvh->nodes      = malloc((size_t)node_count * sizeof *bvh->nodes);
	bvh->face_order = malloc((size_t)face_count * sizeof *bvh->face_order);
	if (sample_count)
		sample = malloc((size_t)sample_count * sizeof *sample);
	if (!bvh->nodes || !bvh->face_order || (sample_count && !sample)) {
		load_error(error, error_size, "out of memory loading cooked BVH");
		goto fail;
	}
	const uint8_t* cursor = bytes + COOKED_HEADER_BYTES;
	for (uint32_t index = 0; index < node_count; ++index, cursor += COOKED_NODE_BYTES) {
		AeronFaceBvhNode* node = &bvh->nodes[index];
		for (int axis = 0; axis < 3; ++axis) {
			node->min[axis] = get_f32(cursor + axis * 4);
			node->max[axis] = get_f32(cursor + 12 + axis * 4);
		}
		node->max_inv_dominant = get_f32(cursor + 24);
		node->first_or_child   = get_u32(cursor + 28);
		node->count            = get_u32(cursor + 32);
	}
	for (uint32_t index = 0; index < face_count; ++index, cursor += 4)
		bvh->face_order[index] = get_u32(cursor);
	for (uint32_t index = 0; index < sample_count; ++index, cursor += 4)
		sample[index] = get_u32(cursor);

	if (!verify_tree(bvh, geometry, box_only, error, error_size))
		goto fail;
	if (sample_count) {
		uint8_t* used = calloc(unique_vertex_count, 1);
		if (!used) {
			load_error(error, error_size, "out of memory verifying cooked vertex sample");
			goto fail;
		}
		for (uint32_t index = 0; index < sample_count; ++index) {
			if (sample[index] >= unique_vertex_count || used[sample[index]]) {
				free(used);
				load_error(error, error_size, "cooked vertex sample is out of range or repeats a vertex");
				goto fail;
			}
			used[sample[index]] = 1;
		}
		free(used);
	}
	*out_bvh = bvh;
	if (out_sample)
		*out_sample = sample;
	else
		free(sample);
	if (out_sample_count)
		*out_sample_count = sample_count;
	return true;

fail:
	free(sample);
	AeronFaceBvh_Free(bvh);
	return false;
}

bool AeronFaceBvh_LoadCooked(const uint8_t* bytes, size_t size, uint64_t expected_topology_hash,
							 const AeronFaceBvhGeometry* geometry, uint32_t unique_vertex_count,
							 AeronFaceBvh** out_bvh, uint32_t** out_sample, uint32_t* out_sample_count,
							 char* error, size_t error_size) {
	return load_cooked(bytes, size, expected_topology_hash, geometry, false, unique_vertex_count, out_bvh,
					   out_sample, out_sample_count, error, error_size);
}

bool AeronFaceBvh_LoadCookedBoxes(const uint8_t* bytes, size_t size, uint64_t expected_mesh_hash,
								  const AeronFaceBvhGeometry* geometry, AeronFaceBvh** out_bvh, char* error,
								  size_t error_size) {
	return load_cooked(bytes, size, expected_mesh_hash, geometry, true, 0, out_bvh, NULL, NULL, error,
					   error_size);
}

void AeronFaceBvh_ScaleBounds(AeronFaceBvh* bvh, double scale) {
	if (!bvh || !(scale > 0.0))
		return;
	for (uint32_t index = 0; index < bvh->node_count; ++index) {
		AeronFaceBvhNode* node = &bvh->nodes[index];
		for (int axis = 0; axis < 3; ++axis) {
			node->min[axis] = (float)((double)node->min[axis] * scale);
			node->max[axis] = (float)((double)node->max[axis] * scale);
		}
	}
}

bool AeronFaceBvh_ReorderToLeafOrder(AeronFaceBvh* bvh, void* items, size_t item_size, uint32_t* old_to_new) {
	if (!bvh || !items || !item_size || bvh->face_count > SIZE_MAX / item_size)
		return false;
	if (bvh->leaf_ordered) {
		if (old_to_new)
			for (uint32_t index = 0; index < bvh->face_count; ++index)
				old_to_new[index] = index;
		return true;
	}
	uint8_t*  reordered = malloc((size_t)bvh->face_count * item_size);
	uint32_t* starts    = malloc((size_t)bvh->node_count * sizeof *starts);
	uint32_t* stack     = malloc((size_t)bvh->node_count * sizeof *stack);
	if (!reordered || !starts || !stack) {
		free(reordered);
		free(starts);
		free(stack);
		return false;
	}
	/* Every node is pushed once, so node_count bounds the stack. */
	uint32_t depth = 0, next = 0;
	stack[depth++] = 0;
	while (depth) {
		const uint32_t          node_index = stack[--depth];
		const AeronFaceBvhNode* node       = &bvh->nodes[node_index];
		if (node->count == 0) {
			stack[depth++] = node->first_or_child + 1;
			stack[depth++] = node->first_or_child;
			continue;
		}
		for (uint32_t entry = 0; entry < node->count; ++entry) {
			const uint32_t face = bvh->face_order[node->first_or_child + entry];
			memcpy(reordered + (size_t)(next + entry) * item_size, (const uint8_t*)items + (size_t)face * item_size,
				   item_size);
			if (old_to_new)
				old_to_new[face] = next + entry;
		}
		starts[node_index] = next;
		next += node->count;
	}
	for (uint32_t index = 0; index < bvh->node_count; ++index)
		if (bvh->nodes[index].count != 0)
			bvh->nodes[index].first_or_child = starts[index];
	for (uint32_t index = 0; index < bvh->face_count; ++index)
		bvh->face_order[index] = index;
	memcpy(items, reordered, (size_t)bvh->face_count * item_size);
	bvh->leaf_ordered = true;
	free(reordered);
	free(starts);
	free(stack);
	return true;
}

/* ===== Queries ======================================================= */

static bool scratch_reserve(uint32_t** items, uint32_t* capacity, uint32_t needed) {
	if (needed <= *capacity)
		return true;
	uint32_t grown = *capacity ? *capacity : 64;
	while (grown < needed) {
		if (grown > UINT32_MAX / 2)
			return false;
		grown *= 2;
	}
	uint32_t* resized = realloc(*items, (size_t)grown * sizeof **items);
	if (!resized)
		return false;
	*items    = resized;
	*capacity = grown;
	return true;
}

/* A segment query, set up once: the reciprocal direction replaces a division
 * per axis and node. */
typedef struct SegmentQuery {
	double  start[3];
	double  inv_delta[3]; /* 1 / (end - start); unused on flat axes */
	uint8_t flat[3];      /* end == start on this axis */
	double  tolerance;
	double  q; /* 2^AERON_FACE_BVH_ROUNDING_SCALE * the largest coordinate; see AeronFaceBvh_Collect */
} SegmentQuery;

/* Slack on the parameter comparisons. Multiplying by the reciprocal rounds
 * differently from dividing, by a few ulps of parameters near [0, 1]; this
 * keeps the test from rejecting a box an exact test accepts. Accepting a few
 * more boxes only adds candidates. */
#define SEGMENT_T_SLACK 1.0e-9

/* Closed slab test of the segment against the node box expanded by
 * tolerance * max_inv_dominant + AERON_FACE_BVH_ABSOLUTE_SLACK + q. */
static bool segment_meets_node(const SegmentQuery* query, const AeronFaceBvhNode* node) {
	const double margin =
		query->tolerance * (double)node->max_inv_dominant + AERON_FACE_BVH_ABSOLUTE_SLACK + query->q;
	double       t_min  = 0.0;
	double       t_max  = 1.0;
	for (int axis = 0; axis < 3; ++axis) {
		const double lo = (double)node->min[axis] - margin;
		const double hi = (double)node->max[axis] + margin;
		if (query->flat[axis]) {
			if (query->start[axis] < lo || query->start[axis] > hi)
				return false;
			continue;
		}
		double t0 = (lo - query->start[axis]) * query->inv_delta[axis];
		double t1 = (hi - query->start[axis]) * query->inv_delta[axis];
		if (t0 > t1) {
			const double swap = t0;
			t0                = t1;
			t1                = swap;
		}
		if (t0 > t_min)
			t_min = t0;
		if (t1 < t_max)
			t_max = t1;
		if (t_min > t_max + SEGMENT_T_SLACK)
			return false;
	}
	return true;
}

static bool node_meets_slabs(const AeronFaceBvhNode* node, const AeronFaceBvhSlab* slabs, uint32_t slab_count) {
	for (uint32_t index = 0; index < slab_count; ++index) {
		const AeronFaceBvhSlab* slab       = &slabs[index];
		double                  center     = 0.0;
		double                  half_width = 0.0;
		for (int axis = 0; axis < 3; ++axis) {
			const double mid  = 0.5 * ((double)node->min[axis] + (double)node->max[axis]);
			const double half = 0.5 * ((double)node->max[axis] - (double)node->min[axis]);
			center += mid * (double)slab->normal[axis];
			half_width += fabs(half * (double)slab->normal[axis]);
		}
		const double widen = ldexp(fabs((double)slab->min) + fabs((double)slab->max), -10) + 1.0e-3;
		if (center + half_width < (double)slab->min - widen || center - half_width > (double)slab->max + widen)
			return false;
	}
	return true;
}

typedef struct Query {
	const SegmentQuery*     segment; /* NULL for a slab query */
	const AeronFaceBvhSlab* slabs;
	uint32_t                slab_count;
} Query;

static bool query_meets_node(const Query* query, const AeronFaceBvhNode* node) {
	return query->segment ? segment_meets_node(query->segment, node)
						  : node_meets_slabs(node, query->slabs, query->slab_count);
}

/* Merges the ascending runs a[0..a_count) and b[0..b_count) into out. */
static void merge_runs(const uint32_t* a, uint32_t a_count, const uint32_t* b, uint32_t b_count, uint32_t* out) {
	uint32_t i = 0, j = 0, k = 0;
	while (i < a_count && j < b_count)
		out[k++] = a[i] < b[j] ? a[i++] : b[j++];
	while (i < a_count)
		out[k++] = a[i++];
	while (j < b_count)
		out[k++] = b[j++];
}

/* Leaves list their faces ascending (built trees sort them, cooked trees are
 * verified), so the candidates are a few ascending runs, one or more leaves
 * each. A natural merge sort orders them in O(count * log runs); leaves
 * partition the faces, so the result is also unique. `items` has room for
 * 2 * count entries, the upper half being the merge buffer. */
static void sort_runs(uint32_t* items, uint32_t count) {
	uint32_t* from = items;
	uint32_t* to   = items + count;
	for (;;) {
		uint32_t start = 0;
		while (start < count) {
			uint32_t middle = start + 1;
			while (middle < count && from[middle - 1] < from[middle])
				++middle;
			if (middle == count) {
				if (start == 0) {
					if (from != items)
						memcpy(items, from, (size_t)count * sizeof *items);
					return;
				}
				memcpy(to + start, from + start, (size_t)(count - start) * sizeof *items);
				break;
			}
			uint32_t end = middle + 1;
			while (end < count && from[end - 1] < from[end])
				++end;
			merge_runs(from + start, middle - start, from + middle, end - middle, to + start);
			start = end;
		}
		uint32_t* swap = from;
		from           = to;
		to             = swap;
	}
}

/* Depth-first walk, left child first; a child is tested before it is
 * pushed. In a leaf-ordered tree the leaves come out in ascending face order. */
static uint32_t run_query(const AeronFaceBvh* bvh, const Query* query, AeronFaceBvhScratch* scratch) {
	if (!scratch)
		return UINT32_MAX;
	scratch->count = 0;
	if (!bvh || !bvh->node_count || !query_meets_node(query, &bvh->nodes[0]))
		return 0;
	uint32_t depth = 0;
	if (!scratch_reserve(&scratch->stack, &scratch->stack_capacity, 1))
		return UINT32_MAX;
	scratch->stack[depth++] = 0;
	while (depth) {
		const AeronFaceBvhNode* node = &bvh->nodes[scratch->stack[--depth]];
		if (node->count != 0) {
			if (!scratch_reserve(&scratch->indices, &scratch->capacity, scratch->count + node->count))
				return UINT32_MAX;
			uint32_t* out = scratch->indices + scratch->count;
			if (bvh->leaf_ordered) {
				for (uint32_t entry = 0; entry < node->count; ++entry)
					out[entry] = node->first_or_child + entry;
			} else {
				memcpy(out, bvh->face_order + node->first_or_child, (size_t)node->count * sizeof *out);
			}
			scratch->count += node->count;
			continue;
		}
		if (!scratch_reserve(&scratch->stack, &scratch->stack_capacity, depth + 2))
			return UINT32_MAX;
		const uint32_t left = node->first_or_child;
		if (query_meets_node(query, &bvh->nodes[left + 1]))
			scratch->stack[depth++] = left + 1;
		if (query_meets_node(query, &bvh->nodes[left]))
			scratch->stack[depth++] = left;
	}
	if (!bvh->leaf_ordered && scratch->count > 1) {
		if (scratch->count > UINT32_MAX / 2 ||
			!scratch_reserve(&scratch->indices, &scratch->capacity, 2 * scratch->count))
			return UINT32_MAX;
		sort_runs(scratch->indices, scratch->count);
	}
	return scratch->count;
}

/* The margin q covers rounding in the game's float face test, which grows
 * with the size of the coordinates. Let M be the largest coordinate of the
 * segment ends and the face vertices, and u = 2^-24 the float rounding unit.
 * Bounding each operation of the test gives:
 * - the plane distances of the two ends are off by at most 14 u M;
 * - so the computed hit point is within 30 u M of the face plane, which is
 *   at most 52 u M along the projection axis (1 / |n_dominant| <= 1.74);
 * - the computed hit point is off the exact segment by at most 5 u M.
 * A face that passes the test therefore meets the segment within about
 * 57 u M (about 2^-18 M) of its box, plus the plane epsilon term. q is
 * 2^-15 M, 9 times that bound. M uses the root box for the vertices, so it
 * holds for very large models too. Rounding that does not grow with the
 * coordinates (the 2D edge tests, the bounds scaling) stays in
 * AERON_FACE_BVH_ABSOLUTE_SLACK. */
#define AERON_FACE_BVH_ROUNDING_SCALE (-15)

uint32_t AeronFaceBvh_Collect(const AeronFaceBvh* bvh, const float start[3], const float end[3],
							  float plane_tolerance, AeronFaceBvhScratch* scratch) {
	SegmentQuery segment = { .tolerance = (double)plane_tolerance };
	double       largest = 0.0;
	if (bvh && bvh->node_count) {
		for (int axis = 0; axis < 3; ++axis) {
			largest = fmax(largest, fabs((double)bvh->nodes[0].min[axis]));
			largest = fmax(largest, fabs((double)bvh->nodes[0].max[axis]));
		}
	}
	for (int axis = 0; axis < 3; ++axis) {
		const double from  = (double)start[axis];
		const double to    = (double)end[axis];
		const double delta = to - from;
		segment.start[axis]     = from;
		segment.flat[axis]      = delta == 0.0;
		segment.inv_delta[axis] = delta == 0.0 ? 0.0 : 1.0 / delta;
		if (fabs(from) > largest)
			largest = fabs(from);
		if (fabs(to) > largest)
			largest = fabs(to);
	}
	segment.q         = ldexp(largest, AERON_FACE_BVH_ROUNDING_SCALE);
	const Query query = { .segment = &segment };
	return run_query(bvh, &query, scratch);
}

uint32_t AeronFaceBvh_CollectSlabs(const AeronFaceBvh* bvh, const AeronFaceBvhSlab* slabs, uint32_t slab_count,
								   AeronFaceBvhScratch* scratch) {
	const Query query = { .slabs = slabs, .slab_count = slab_count };
	return run_query(bvh, &query, scratch);
}

void AeronFaceBvhScratch_Free(AeronFaceBvhScratch* scratch) {
	if (!scratch)
		return;
	free(scratch->indices);
	free(scratch->stack);
	memset(scratch, 0, sizeof *scratch);
}
