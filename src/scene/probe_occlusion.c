/* Occlusion of the first probe set's light by what stands inside its grid.
 *
 * A probe set is baked without the ships in its model (a hangar), so their
 * surroundings keep all the probe light. The occluders, the instances inside
 * the set's grid box that cast shadows other than the set's owner (in the
 * bake), fill an occupancy grid of 0.25 m cells over the box, in the set's
 * model frame. The GPU builds it, as Godot's VoxelGI does for moving
 * objects: each occluder's depth is drawn along the box's three axes from
 * both sides, and a compute pass fills the cells between the first and the
 * last surface along all three. Where compute cannot write the grid, the CPU
 * voxelizes the occluders instead. A compute pass then gives each 1 m cell
 * of the box, for the faces of an axis-aligned cube, the share of the probe
 * irradiance that still reaches them along 32 rays through the occupancy:
 * the probe shaders scale their diffuse light by it, blending the faces
 * toward their normal. A second pass darkens what the rays see of the floor
 * by the floor's own share there: under a hull, the floor sends up little
 * light. The visibility pass traces a cone through the occupancy's mips for
 * their reflections. Both grids are rebuilt only where an occluder changed:
 * parked ships cost nothing. */
#include "internal.h"

#include "aeron/log.h"
#include "aeron/scene/environment.h"
#include "aeron/scene/mesh.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How far an occluder's effect on the grid reaches (metres): a ship hides a
 * share of the sky from cells this far from it. */
#define OCCLUSION_REACH 24.0f
#define GRID_THREADS 4
/* While occluders keep changing (a ship taking off, its gear turning), the
 * CPU path rebuilds the grids at most once in this many frames: their light
 * lags by up to two frames, for a third of the cost. */
#define REBUILD_FRAMES 3
/* The depth views: 2 texels a cell, so thin parts are not missed, in an
 * atlas of this size. */
#define VIEW_TEXELS_PER_CELL 2
#define VIEW_ATLAS 2048
#define BUILD_THREADS 4

/* An occluder's views: their box on the occupancy's lattice, grown by a
 * cell, and per axis the atlas tiles of the views from its high side (x, y)
 * and from its low side (z, w). Mirrors scene_probe_occupancy_compose's. */
typedef struct ViewRecord {
	float    lo[4];
	float    hi[4];
	uint32_t tiles[3][4];
} ViewRecord;

typedef struct ComposeUniform {
	float   origin_cell[4]; /* the occupancy's low corner (model metres), cell size */
	int32_t region_lo[4];
	int32_t region_size[4]; /* w: the record count */
	float   texels[4];      /* x: view texels per metre */
} ComposeUniform;

typedef struct DownsampleUniform {
	int32_t region_lo[4];
	int32_t region_size[4]; /* w: the level read */
	int32_t source_dims[4];
} DownsampleUniform;

typedef struct OcclusionGridUniform {
	float    origin_cell[4]; /* the box's low corner (model metres), occupancy cell size */
	uint32_t occupancy_dims[4];
	uint32_t grid_dims[4];                                /* w: the grid cell size in metres, as float bits */
	int32_t  region[4];                                   /* the first grid cell of this dispatch */
	float    occluders_lo[4];                             /* the occluders' bounds (model metres); w: reach */
	float    occluders_hi[4];                             /* w: the probe cube mip to read */
	float boxes[AERON_SCENE_OCCLUSION_CONE_BOXES * 2][4]; /* lo, hi pairs (model metres); lo.w: 1 when used */
	/* What changed since the cells were built, as `boxes`: a cell none of
	 * whose rays meets them keeps its value. None: every cell is built. */
	float changes[AERON_SCENE_OCCLUSION_CONE_BOXES * 2][4];
} OcclusionGridUniform;

typedef struct Crossing {
	uint32_t column;
	float    z;     /* occupancy cells */
	int      enter; /* the surface faces down: going up enters the occluder */
} Crossing;

typedef struct CrossingList {
	Crossing* items;
	size_t    count;
	size_t    capacity;
} CrossingList;

static int crossing_push(CrossingList* list, uint32_t column, float z, int enter) {
	if (list->count == list->capacity) {
		const size_t capacity = list->capacity ? list->capacity * 2 : 4096;
		Crossing*    items    = (Crossing*)realloc(list->items, capacity * sizeof *items);
		if (!items) {
			return 0;
		}
		list->items    = items;
		list->capacity = capacity;
	}
	list->items[list->count++] = (Crossing) { column, z, enter };
	return 1;
}

static int crossing_compare(const void* a, const void* b) {
	const Crossing* x = (const Crossing*)a;
	const Crossing* y = (const Crossing*)b;
	if (x->column != y->column) {
		return x->column < y->column ? -1 : 1;
	}
	return x->z < y->z ? -1 : (x->z > y->z ? 1 : 0);
}

static uint64_t hash_bytes(uint64_t h, const void* data, size_t size) {
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < size; ++i) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

/* The placement, articulation and visibility that decide an occluder's cells. */
static uint64_t occluder_hash(const AeronSceneMeshInstance* in) {
	uint64_t h                       = hash_bytes(1469598103934665603ull, &in->mesh, sizeof in->mesh);
	h                                = hash_bytes(h, in->transform, sizeof in->transform);
	const AeronSceneMeshTable* table = in->mesh_table;
	if (table) {
		const uint32_t count = table->component_count < AERON_MAX_MESH_COMPONENTS ? table->component_count
																				  : AERON_MAX_MESH_COMPONENTS;
		h                    = hash_bytes(h, &table->component_count, sizeof table->component_count);
		h                    = hash_bytes(h, table->rows, count * sizeof table->rows[0]);
		h = hash_bytes(h, table->visibility_packed, ((count + 3u) / 4u) * sizeof table->visibility_packed[0]);
	}
	return h;
}

static void transform_point(const float m[12], const float p[3], float out[3]) {
	for (int r = 0; r < 3; ++r) {
		out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
	}
}

/* An instance's bounds in the probe set's model frame (metres), its box
 * along the model's axes (AeronSceneInternal_InstanceBox); 0 when it draws
 * nothing. */
static int occluder_bounds(const AeronSceneMeshInstance* in, const float world_to_model[12], float lo[3],
						   float hi[3]) {
	float axes[3][3], scale[3];
	for (int r = 0; r < 3; ++r) {
		const float* row = &world_to_model[r * 4];
		scale[r]         = sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
		if (!(scale[r] > 0.0f)) {
			return 0;
		}
		for (int k = 0; k < 3; ++k) {
			axes[r][k] = row[k] / scale[r];
		}
	}
	AeronSceneInstanceBox box;
	if (!AeronSceneInternal_InstanceBox(in, axes[0], axes[1], axes[2], &box)) {
		return 0;
	}
	float center[3];
	transform_point(world_to_model, box.center, center);
	for (int k = 0; k < 3; ++k) {
		lo[k] = center[k] - box.half[k] * scale[k];
		hi[k] = center[k] + box.half[k] * scale[k];
	}
	return 1;
}

static int boxes_overlap(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3]) {
	return alo[0] <= bhi[0] && blo[0] <= ahi[0] && alo[1] <= bhi[1] && blo[1] <= ahi[1] && alo[2] <= bhi[2] &&
		   blo[2] <= ahi[2];
}

static void grow_box(float lo[3], float hi[3], const float add_lo[3], const float add_hi[3]) {
	for (int k = 0; k < 3; ++k) {
		lo[k] = fminf(lo[k], add_lo[k]);
		hi[k] = fmaxf(hi[k], add_hi[k]);
	}
}

/* The frame's occluders: shadow casters inside the box, not its owner. */
static int gather_occluders(AeronScene3D* s, const AeronSceneProbeSet* set, const float box_lo[3],
							const float box_hi[3], AeronSceneOccluder* out,
							const AeronSceneMeshInstance** instances) {
	int count = 0;
	for (int list = 0; list < 2; ++list) {
		const AeronSceneMeshInstance* items = list == 0 ? s->instances : s->shadow_only;
		const int                     n     = list == 0 ? s->instance_count : s->shadow_only_count;
		for (int i = 0; i < n; ++i) {
			const AeronSceneMeshInstance* in = &items[i];
			if (!in->mesh || !in->mesh->cpu_vertices || !in->mesh->cpu_indices || in->probe_set_owner == 1 ||
				(in->shadow_flags & AERON_SCENE_INSTANCE_NO_CAST_SHADOW)) {
				continue;
			}
			AeronSceneOccluder occluder = { in->mesh,
											(uint16_t)(list == 0 ? i : AERON_SCENE_MAX_INSTANCES + i),
											occluder_hash(in),
											{ 0 },
											{ 0 } };
			if (!occluder_bounds(in, set->world_to_model, occluder.lo, occluder.hi) ||
				!boxes_overlap(occluder.lo, occluder.hi, box_lo, box_hi)) {
				continue;
			}
			if (count == AERON_SCENE_OCCLUSION_MAX_OCCLUDERS) {
				if (!s->probe_occlusion.dropped_warned) {
					Aeron_LogWarn("aeron.scene",
								  "probe occlusion: more than %d occluders; the rest are ignored",
								  AERON_SCENE_OCCLUSION_MAX_OCCLUDERS);
					s->probe_occlusion.dropped_warned = 1;
				}
				return count;
			}
			out[count]       = occluder;
			instances[count] = in;
			++count;
		}
	}
	return count;
}

/* Voxelizes one occluder into the occupancy (cells of 0.25 m) and gives
 * its posed bounds (model metres). Each column is filled from the cell
 * where a surface facing down is crossed (entering it going up) to the
 * cell of the next facing up; a crossing
 * left unpaired (a thin plate) marks its cell, and steep triangles, which
 * columns miss, mark the cells they cross. */
static int voxelize(AeronSceneProbeOcclusion* o, const AeronSceneMeshInstance* in,
					const float world_to_model[12], CrossingList* crossings, float bounds_lo[3],
					float bounds_hi[3]) {
	const AeronSceneMesh*      mesh  = in->mesh;
	const AeronSceneMeshTable* table = in->mesh_table;
	const uint32_t             table_components =
		table && table->component_count && table->component_count < AERON_MAX_MESH_COMPONENTS
			? table->component_count
			: AERON_MAX_MESH_COMPONENTS;
	const float inv_cell = 1.0f / AERON_SCENE_OCCUPANCY_CELL;
	const int*  dims     = o->occupancy_dims;
	uint8_t*    cells    = o->cells;
	/* Posed vertices in occupancy cells, and their normals' z in the model
	 * frame (NaN position: a hidden component). */
	float* posed = (float*)malloc((size_t)mesh->vertex_count * 4u * sizeof(float));
	if (!posed) {
		return 0;
	}
	float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (uint32_t v = 0; v < mesh->vertex_count; ++v) {
		const AeronSceneMeshCpuVertex* vertex    = &mesh->cpu_vertices[v];
		const long                     last      = (long)mesh->component_count - 1;
		long                           component = lroundf(vertex->component);
		component                                = component < 0 ? 0 : (component > last ? last : component);
		const uint32_t row =
			(uint32_t)component < table_components ? (uint32_t)component : table_components - 1u;
		float* out = &posed[v * 4u];
		if (table && table->visibility_packed[row >> 2][row & 3] < 0.5f) {
			out[0] = NAN;
			continue;
		}
		float local[3], local_normal[3];
		for (int r = 0; r < 3; ++r) {
			if (table) {
				const float* m = table->rows[row][r];
				local[r]       = m[0] * vertex->pos[0] + m[1] * vertex->pos[1] + m[2] * vertex->pos[2] + m[3];
				local_normal[r] =
					m[0] * vertex->normal[0] + m[1] * vertex->normal[1] + m[2] * vertex->normal[2];
			} else {
				local[r]        = vertex->pos[r];
				local_normal[r] = vertex->normal[r];
			}
		}
		float world[3], world_normal[3], model[3];
		for (int r = 0; r < 3; ++r) {
			const float* m  = &in->transform[r * 4];
			world[r]        = m[0] * local[0] + m[1] * local[1] + m[2] * local[2] + m[3];
			world_normal[r] = m[0] * local_normal[0] + m[1] * local_normal[1] + m[2] * local_normal[2];
		}
		transform_point(world_to_model, world, model);
		for (int k = 0; k < 3; ++k) {
			lo[k]  = fminf(lo[k], model[k]);
			hi[k]  = fmaxf(hi[k], model[k]);
			out[k] = (model[k] - o->origin[k]) * inv_cell;
		}
		out[3] = world_to_model[8] * world_normal[0] + world_to_model[9] * world_normal[1] +
				 world_to_model[10] * world_normal[2];
	}
	memcpy(bounds_lo, lo, sizeof lo);
	memcpy(bounds_hi, hi, sizeof hi);

	crossings->count = 0;
	for (uint32_t t = 0; t + 2 < mesh->index_count; t += 3) {
		const float* p[3] = { &posed[mesh->cpu_indices[t] * 4u], &posed[mesh->cpu_indices[t + 1] * 4u],
							  &posed[mesh->cpu_indices[t + 2] * 4u] };
		if (isnan(p[0][0]) || isnan(p[1][0]) || isnan(p[2][0])) {
			continue;
		}
		/* The mean of the unit vertex normals' z. */
		const float facing = (p[0][3] + p[1][3] + p[2][3]) / 3.0f;
		if (fabsf(facing) < 0.5f) {
			/* Steep: points no farther apart than half a cell. */
			const float e1[3]   = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
			const float e2[3]   = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
			const float longest = fmaxf(sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]),
										sqrtf(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]));
			const int   steps   = (int)ceilf(longest * 2.0f) + 1;
			for (int i = 0; i <= steps && steps <= 4096; ++i) {
				for (int j = 0; i + j <= steps; ++j) {
					const float a = (float)i / (float)steps, b = (float)j / (float)steps;
					const int   x = (int)floorf(p[0][0] + e1[0] * a + e2[0] * b);
					const int   y = (int)floorf(p[0][1] + e1[1] * a + e2[1] * b);
					const int   z = (int)floorf(p[0][2] + e1[2] * a + e2[2] * b);
					if (x >= 0 && y >= 0 && z >= 0 && x < dims[0] && y < dims[1] && z < dims[2]) {
						cells[((size_t)z * dims[1] + y) * dims[0] + x] = 255;
					}
				}
			}
		}
		if (fabsf(facing) < 1.0e-3f) {
			continue;
		}
		/* Columns: where the column's centre meets the triangle, oriented
		 * counter-clockwise in xy, owning its top and left edges. */
		const float* a    = p[0];
		const float* b    = p[1];
		const float* c    = p[2];
		float        area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
		if (fabsf(area) < 1.0e-8f) {
			continue;
		}
		if (area < 0.0f) {
			const float* swap = b;
			b                 = c;
			c                 = swap;
			area              = -area;
		}
		const float* v[3] = { a, b, c };
		const int    x0   = (int)fmaxf(ceilf(fminf(fminf(a[0], b[0]), c[0]) - 0.5f), 0.0f);
		const int    x1   = (int)fminf(floorf(fmaxf(fmaxf(a[0], b[0]), c[0]) - 0.5f), (float)dims[0] - 1);
		const int    y0   = (int)fmaxf(ceilf(fminf(fminf(a[1], b[1]), c[1]) - 0.5f), 0.0f);
		const int    y1   = (int)fminf(floorf(fmaxf(fmaxf(a[1], b[1]), c[1]) - 0.5f), (float)dims[1] - 1);
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const float px = (float)x + 0.5f, py = (float)y + 0.5f;
				float       w[3];
				int         inside = 1;
				for (int e = 0; e < 3 && inside; ++e) {
					const float* s0 = v[(e + 1) % 3];
					const float* s1 = v[(e + 2) % 3];
					const float  dx = s1[0] - s0[0], dy = s1[1] - s0[1];
					const int    top_left = (dy == 0.0f && dx < 0.0f) || dy < 0.0f;
					w[e]                  = dx * (py - s0[1]) - dy * (px - s0[0]);
					inside                = w[e] > 0.0f || (w[e] == 0.0f && top_left);
				}
				if (inside &&
					!crossing_push(crossings, (uint32_t)(y * dims[0] + x),
								   (w[0] * a[2] + w[1] * b[2] + w[2] * c[2]) / area, facing < 0.0f)) {
					free(posed);
					return 0;
				}
			}
		}
	}
	free(posed);

	qsort(crossings->items, crossings->count, sizeof *crossings->items, crossing_compare);
	const size_t layer = (size_t)dims[0] * dims[1];
	for (size_t i = 0; i < crossings->count;) {
		const uint32_t column = crossings->items[i].column;
		int            depth  = 0;
		float          start  = 0.0f;
		for (; i < crossings->count && crossings->items[i].column == column; ++i) {
			const Crossing* x = &crossings->items[i];
			const int       z = (int)floorf(x->z);
			if (x->enter && depth++ == 0) {
				start = x->z;
			} else if (!x->enter && depth > 0 && --depth == 0) {
				/* Every cell the solid reaches into, its surfaces' too. */
				const int z0 = (int)fmaxf(floorf(start), 0.0f);
				const int z1 = (int)fminf(floorf(x->z), (float)dims[2] - 1);
				for (int k = z0; k <= z1; ++k) {
					cells[(size_t)k * layer + column] = 255;
				}
			} else if (!x->enter && z >= 0 && z < dims[2]) {
				cells[(size_t)z * layer + column] = 255; /* unpaired: a plate seen from below */
			}
		}
		if (depth > 0) {
			const int z = (int)floorf(start);
			if (z >= 0 && z < dims[2]) {
				cells[(size_t)z * layer + column] = 255; /* unpaired: a plate seen from above */
			}
		}
	}
	return 1;
}

static void release_grids(AeronSceneProbeOcclusion* o) {
	Aeron_DestroyTexture(o->occupancy);
	Aeron_DestroyTexture(o->grid);
	Aeron_DestroyTexture(o->unshaded);
	Aeron_DestroyTexture(o->staging);
	Aeron_DestroyDepthTarget(o->views);
	Aeron_DestroyBuffer(o->records);
	free(o->cells);
	o->occupancy      = NULL;
	o->grid           = NULL;
	o->unshaded       = NULL;
	o->staging        = NULL;
	o->views          = NULL;
	o->records        = NULL;
	o->cells          = NULL;
	o->package        = NULL;
	o->occluder_count = 0;
	o->valid          = 0;
}

void AeronSceneProbeOcclusion_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	AeronSceneProbeOcclusion* o = &s->probe_occlusion;
	release_grids(o);
	Aeron_DestroyTexture(o->placeholder);
	Aeron_DestroySampler(o->sampler);
	Aeron_DestroyComputePipeline(o->pipeline);
	Aeron_DestroyComputePipeline(o->compose);
	Aeron_DestroyComputePipeline(o->downsample);
	memset(o, 0, sizeof *o);
}

AeronTexture* AeronSceneProbeOcclusion_Occupancy(AeronScene3D* s) {
	AeronSceneProbeOcclusion* o = &s->probe_occlusion;
	if (o->active) {
		return o->occupancy;
	}
	if (!o->placeholder) {
		o->placeholder             = Aeron_CreateTexture(&(AeronTextureDesc) {
			.width      = 1,
			.height     = 1,
			.depth      = 1,
			.mip_count  = 1,
			.format     = AERON_TEXTURE_FORMAT_R8_UNORM,
			.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
			.debug_name = "scene.probe_occlusion_placeholder",
		});
		static const uint8_t empty = 0;
		if (o->placeholder) {
			Aeron_UploadTextureData(&(AeronTextureUploadDesc) {
				.texture = o->placeholder, .width = 1, .height = 1, .raw_data = &empty, .raw_size = 1 });
		}
	}
	return o->placeholder;
}

AeronTexture* AeronSceneProbeOcclusion_Grid(AeronScene3D* s) {
	return s->probe_occlusion.active ? s->probe_occlusion.grid : AeronSceneProbeOcclusion_Occupancy(s);
}

AeronSampler* AeronSceneProbeOcclusion_Sampler(AeronScene3D* s) {
	AeronSceneProbeOcclusion* o = &s->probe_occlusion;
	if (!o->sampler) {
		o->sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_LINEAR,
			.mag_filter = AERON_FILTER_LINEAR,
			.mip_filter = AERON_FILTER_LINEAR,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.max_lod    = (float)(AERON_SCENE_OCCUPANCY_MIPS - 1),
		});
	}
	return o->sampler;
}

void AeronSceneProbeOcclusion_LightingFields(const AeronScene3D* s, int set, float origin[4], float dims[4]) {
	const AeronSceneProbeOcclusion* o = &s->probe_occlusion;
	for (int k = 0; k < 3; ++k) {
		origin[k] = o->origin[k];
		dims[k]   = (float)o->grid_dims[k];
	}
	origin[3] = o->active && set == 0 ? 1.0f : 0.0f;
	dims[3]   = AERON_SCENE_OCCLUSION_GRID_CELL;
}

static float box_volume(const float lo[3], const float hi[3]) {
	return (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
}

/* The boxes [lo, hi), the two whose union adds the least volume merged
 * until AERON_SCENE_OCCLUSION_CONE_BOXES remain; their count. */
static int merge_boxes(float (*lo)[3], float (*hi)[3], int count) {
	while (count > AERON_SCENE_OCCLUSION_CONE_BOXES) {
		int   best_a = 0, best_b = 1;
		float best = FLT_MAX;
		for (int a = 0; a < count; ++a) {
			for (int b = a + 1; b < count; ++b) {
				float ulo[3], uhi[3];
				memcpy(ulo, lo[a], sizeof ulo);
				memcpy(uhi, hi[a], sizeof uhi);
				grow_box(ulo, uhi, lo[b], hi[b]);
				const float added =
					box_volume(ulo, uhi) - box_volume(lo[a], hi[a]) - box_volume(lo[b], hi[b]);
				if (added < best) {
					best   = added;
					best_a = a;
					best_b = b;
				}
			}
		}
		grow_box(lo[best_a], hi[best_a], lo[best_b], hi[best_b]);
		count--;
		memcpy(lo[best_b], lo[count], sizeof lo[best_b]);
		memcpy(hi[best_b], hi[count], sizeof hi[best_b]);
	}
	return count;
}

/* The occluders' bounds grown by `grow` metres, merged (merge_boxes). */
static int merged_boxes(const AeronSceneProbeOcclusion* o, float grow,
						float lo[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3],
						float hi[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3]) {
	for (int i = 0; i < o->occluder_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			lo[i][k] = o->occluders[i].lo[k] - grow;
			hi[i][k] = o->occluders[i].hi[k] + grow;
		}
	}
	return merge_boxes(lo, hi, o->occluder_count);
}

void AeronSceneProbeOcclusion_OccupancyMapping(const AeronScene3D* s, float rows[3][4], float dims[4],
											   float boxes[AERON_SCENE_OCCLUSION_CONE_BOXES * 2][4]) {
	const AeronSceneProbeOcclusion* o        = &s->probe_occlusion;
	const float                     inv_cell = 1.0f / AERON_SCENE_OCCUPANCY_CELL;
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 4; ++c) {
			rows[r][c] = o->world_to_model[r * 4 + c] * inv_cell;
		}
		rows[r][3] -= o->origin[r] * inv_cell;
		dims[r] = (float)o->occupancy_dims[r];
	}
	dims[3] = o->active && o->occluder_count > 0 ? 1.0f : 0.0f;
	/* In cells: a cone samples the coarsest mip's cells (16 occupancy
	 * cells) up to one away from what they hold. */
	float     lo[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3], hi[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3];
	const int count =
		merged_boxes(o, (float)(1 << (AERON_SCENE_OCCUPANCY_MIPS - 1)) * AERON_SCENE_OCCUPANCY_CELL, lo, hi);
	for (int b = 0; b < AERON_SCENE_OCCLUSION_CONE_BOXES; ++b) {
		for (int k = 0; k < 3; ++k) {
			boxes[2 * b][k]     = b < count ? (lo[b][k] - o->origin[k]) * inv_cell : 0.0f;
			boxes[2 * b + 1][k] = b < count ? (hi[b][k] - o->origin[k]) * inv_cell : 0.0f;
		}
		boxes[2 * b][3]     = b < count ? 1.0f : 0.0f;
		boxes[2 * b + 1][3] = 0.0f;
	}
}

/* (Re)creates the grids for the probe set's box; 0 on failure. */
static int ensure_grids(AeronSceneProbeOcclusion* o, const AeronSceneProbeSet* set) {
	if (o->package == set->package && o->occupancy &&
		memcmp(o->world_to_model, set->world_to_model, sizeof o->world_to_model) == 0) {
		return 1;
	}
	release_grids(o);
	const AeronEnvironmentDefinition* d = &set->package->settings;
	for (int k = 0; k < 3; ++k) {
		o->origin[k]         = d->grid_center[k] - 0.5f * d->grid_size[k];
		o->occupancy_dims[k] = (int)ceilf(d->grid_size[k] / AERON_SCENE_OCCUPANCY_CELL);
		o->grid_dims[k]      = (int)ceilf(d->grid_size[k] / AERON_SCENE_OCCLUSION_GRID_CELL);
		if (o->occupancy_dims[k] < 1 || o->occupancy_dims[k] > 2048 || o->grid_dims[k] < 1 ||
			2 * o->grid_dims[0] > 2048) {
			Aeron_LogWarn("aeron.scene", "probe occlusion: the grid box is too large");
			return 0;
		}
	}
	AeronTextureDesc occupancy = {
		.width      = o->occupancy_dims[0],
		.height     = o->occupancy_dims[1],
		.depth      = o->occupancy_dims[2],
		.mip_count  = AERON_SCENE_OCCUPANCY_MIPS,
		.format     = AERON_TEXTURE_FORMAT_R8_UNORM,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST |
					  AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.probe_occupancy",
	};
	const AeronTextureDesc staging = {
		.width      = o->occupancy_dims[0] / 2 > 0 ? o->occupancy_dims[0] / 2 : 1,
		.height     = o->occupancy_dims[1] / 2 > 0 ? o->occupancy_dims[1] / 2 : 1,
		.depth      = o->occupancy_dims[2] / 2 > 0 ? o->occupancy_dims[2] / 2 : 1,
		.mip_count  = AERON_SCENE_OCCUPANCY_MIPS - 1,
		.format     = AERON_TEXTURE_FORMAT_R8_UNORM,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.probe_occupancy_staging",
	};
	/* The GPU path needs compute to write the occupancy. */
	o->gpu = Aeron_TextureDescSupported(&occupancy) && Aeron_TextureDescSupported(&staging);
	if (!o->gpu) {
		occupancy.usage &= ~(uint32_t)AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE;
	}
	const AeronTextureDesc grid = {
		.width      = 2 * o->grid_dims[0],
		.height     = o->grid_dims[1],
		.depth      = o->grid_dims[2],
		.mip_count  = 1,
		.format     = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.probe_occlusion_grid",
	};
	if (!Aeron_TextureDescSupported(&occupancy) || !Aeron_TextureDescSupported(&grid)) {
		Aeron_LogWarn("aeron.scene", "probe occlusion: the grid formats are not supported");
		return 0;
	}
	o->occupancy = Aeron_CreateTexture(&occupancy);
	o->grid      = Aeron_CreateTexture(&grid);
	o->unshaded  = Aeron_CreateTexture(&grid);
	if (o->gpu) {
		o->staging = Aeron_CreateTexture(&staging);
		o->views   = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
			.width      = VIEW_ATLAS,
			.height     = VIEW_ATLAS,
			.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.sampled    = 1,
			.debug_name = "scene.probe_occupancy_views",
		});
		/* Dynamic: each build uploads to a fresh buffer, as a frame still in
		 * flight may be reading the last build's. */
		o->records = Aeron_CreateBuffer(&(AeronBufferDesc) {
			.size         = AERON_SCENE_OCCLUSION_MAX_OCCLUDERS * (uint32_t)sizeof(ViewRecord),
			.usage        = AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ,
			.memory_usage = AERON_MEMORY_USAGE_DYNAMIC,
			.debug_name   = "scene.probe_occupancy_views",
		});
	} else {
		o->cells =
			(uint8_t*)calloc((size_t)o->occupancy_dims[0] * o->occupancy_dims[1] * o->occupancy_dims[2], 1);
	}
	if (!o->occupancy || !o->grid || !o->unshaded ||
		(o->gpu ? !o->staging || !o->views || !o->records : !o->cells)) {
		release_grids(o);
		return 0;
	}
	o->package = set->package;
	memcpy(o->world_to_model, set->world_to_model, sizeof o->world_to_model);
	return 1;
}

/* Uploads the occupancy's cells in [lo, hi) and its mips over them (each
 * the mean of the 2x2x2 cells below it); lo is a multiple of the coarsest
 * mip's span. */
static int upload_occupancy(AeronSceneProbeOcclusion* o, AeronCommandBuffer* cmd, const int lo[3],
							const int hi[3]) {
	int      level_lo[3], level_hi[3], level_dims[3];
	int      size[3];
	uint8_t* level = NULL;
	for (int k = 0; k < 3; ++k) {
		level_lo[k]   = lo[k];
		level_hi[k]   = hi[k];
		level_dims[k] = o->occupancy_dims[k];
		size[k]       = hi[k] - lo[k];
	}
	/* Mip 0's region, copied out of the full grid. */
	level = (uint8_t*)malloc((size_t)size[0] * size[1] * size[2]);
	if (!level) {
		return 0;
	}
	for (int z = 0; z < size[2]; ++z) {
		for (int y = 0; y < size[1]; ++y) {
			memcpy(&level[((size_t)z * size[1] + y) * size[0]],
				   &o->cells[(((size_t)(z + lo[2]) * o->occupancy_dims[1]) + (y + lo[1])) *
								 o->occupancy_dims[0] +
							 lo[0]],
				   (size_t)size[0]);
		}
	}
	for (int mip = 0; mip < AERON_SCENE_OCCUPANCY_MIPS; ++mip) {
		if (mip > 0) {
			int      next_lo[3], next_hi[3], next_dims[3], next_size[3];
			uint8_t* next;
			for (int k = 0; k < 3; ++k) {
				next_dims[k] = level_dims[k] / 2 > 0 ? level_dims[k] / 2 : 1;
				next_lo[k]   = level_lo[k] / 2;
				next_hi[k]   = (level_hi[k] + 1) / 2;
				next_hi[k]   = next_hi[k] > next_dims[k] ? next_dims[k] : next_hi[k];
				next_size[k] = next_hi[k] - next_lo[k];
			}
			next = (uint8_t*)malloc((size_t)next_size[0] * next_size[1] * next_size[2]);
			if (!next) {
				free(level);
				return 0;
			}
			for (int z = 0; z < next_size[2]; ++z) {
				for (int y = 0; y < next_size[1]; ++y) {
					for (int x = 0; x < next_size[0]; ++x) {
						unsigned sum = 0, count = 0;
						for (int c = 0; c < 8; ++c) {
							const int sx = 2 * (x + next_lo[0]) + (c & 1) - level_lo[0];
							const int sy = 2 * (y + next_lo[1]) + ((c >> 1) & 1) - level_lo[1];
							const int sz = 2 * (z + next_lo[2]) + ((c >> 2) & 1) - level_lo[2];
							if (sx < size[0] && sy < size[1] && sz < size[2]) {
								sum += level[((size_t)sz * size[1] + sy) * size[0] + sx];
								count++;
							}
						}
						next[((size_t)z * next_size[1] + y) * next_size[0] + x] =
							(uint8_t)(count ? (sum + count / 2) / count : 0);
					}
				}
			}
			free(level);
			level = next;
			for (int k = 0; k < 3; ++k) {
				level_lo[k]   = next_lo[k];
				level_hi[k]   = next_hi[k];
				level_dims[k] = next_dims[k];
				size[k]       = next_size[k];
			}
		}
		if (size[0] > 0 && size[1] > 0 && size[2] > 0 &&
			!Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
												 .texture   = o->occupancy,
												 .mip_level = mip,
												 .x         = level_lo[0],
												 .y         = level_lo[1],
												 .z         = level_lo[2],
												 .width     = size[0],
												 .height    = size[1],
												 .depth     = size[2],
												 .raw_data  = level,
												 .raw_size  = (uint32_t)((size_t)size[0] * size[1] * size[2]),
											 })) {
			free(level);
			return 0;
		}
	}
	free(level);
	return 1;
}

static int ensure_pipeline(AeronSceneProbeOcclusion* o) {
	if (o->gpu && !o->compose) {
		o->compose = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_probe_occupancy_compose.comp",
			.sampler_count                   = 1,
			.readonly_storage_buffer_count   = 1,
			.readwrite_storage_texture_count = 1,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = BUILD_THREADS,
			.thread_count_y                  = BUILD_THREADS,
			.thread_count_z                  = BUILD_THREADS,
		});
	}
	if (o->gpu && !o->downsample) {
		o->downsample = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_probe_occupancy_downsample.comp",
			.sampler_count                   = 1,
			.readwrite_storage_texture_count = 1,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = BUILD_THREADS,
			.thread_count_y                  = BUILD_THREADS,
			.thread_count_z                  = BUILD_THREADS,
		});
	}
	if (o->gpu && (!o->compose || !o->downsample)) {
		return 0;
	}
	if (!o->pipeline) {
		o->pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_probe_occlusion_grid.comp",
			.sampler_count                   = 3,
			.readonly_storage_buffer_count   = 1,
			.readwrite_storage_texture_count = 1,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = GRID_THREADS,
			.thread_count_y                  = GRID_THREADS,
			.thread_count_z                  = GRID_THREADS,
		});
	}
	return o->pipeline != NULL;
}

/* Rebuilds the grid's cells within the model-metre box [lo, hi] whose rays
 * meet one of the `change_count` boxes [change_lo, change_hi] (every cell
 * without any): into `unshaded`, or with `shade_floor`, into the grid with
 * the floor darkened from `unshaded`. */
static int build_grid(AeronScene3D* s, AeronCommandBuffer* cmd, const AeronSceneProbeSet* set,
					  const float lo[3], const float   hi[3], const float (*change_lo)[3],
					  const float (*change_hi)[3], int change_count, int shade_floor) {
	AeronSceneProbeOcclusion*      o   = &s->probe_occlusion;
	const AeronEnvironmentPackage* pkg = set->package;
	OcclusionGridUniform           u   = { 0 };
	int                            first[3], count[3];
	for (int k = 0; k < 3; ++k) {
		first[k] = (int)floorf((lo[k] - o->origin[k]) / AERON_SCENE_OCCLUSION_GRID_CELL);
		first[k] = first[k] < 0 ? 0 : first[k];
		int last = (int)ceilf((hi[k] - o->origin[k]) / AERON_SCENE_OCCLUSION_GRID_CELL);
		last     = last > o->grid_dims[k] ? o->grid_dims[k] : last;
		count[k] = last - first[k];
		if (count[k] <= 0) {
			return 1;
		}
		u.origin_cell[k]    = o->origin[k];
		u.occupancy_dims[k] = (uint32_t)o->occupancy_dims[k];
		u.grid_dims[k]      = (uint32_t)o->grid_dims[k];
		u.region[k]         = first[k];
		u.occluders_lo[k]   = FLT_MAX;
		u.occluders_hi[k]   = -FLT_MAX;
	}
	for (int i = 0; i < o->occluder_count; ++i) {
		grow_box(u.occluders_lo, u.occluders_hi, o->occluders[i].lo, o->occluders[i].hi);
	}
	u.region[3]           = shade_floor;
	const float grid_cell = AERON_SCENE_OCCLUSION_GRID_CELL;
	memcpy(&u.grid_dims[3], &grid_cell, sizeof grid_cell);
	u.origin_cell[3]  = AERON_SCENE_OCCUPANCY_CELL;
	u.occluders_lo[3] = OCCLUSION_REACH;
	/* A cube mip of about 8 x 8 texels a face: the light's broad layout. */
	u.occluders_hi[3] = fmaxf((float)pkg->probe_mip_count - 4.0f, 0.0f);
	/* The boxes holding every occupied cell: the views reach two cells past
	 * an occluder's bounds. */
	float     blo[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3], bhi[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS][3];
	const int boxes = merged_boxes(o, 2.0f * AERON_SCENE_OCCUPANCY_CELL, blo, bhi);
	for (int b = 0; b < boxes; ++b) {
		memcpy(u.boxes[2 * b], blo[b], sizeof blo[b]);
		memcpy(u.boxes[2 * b + 1], bhi[b], sizeof bhi[b]);
		u.boxes[2 * b][3] = 1.0f;
	}
	for (int b = 0; b < change_count; ++b) {
		memcpy(u.changes[2 * b], change_lo[b], sizeof change_lo[b]);
		memcpy(u.changes[2 * b + 1], change_hi[b], sizeof change_hi[b]);
		u.changes[2 * b][3] = 1.0f;
	}

	AeronComputeTextureBinding output = { shade_floor ? o->grid : o->unshaded, 0, 0, 0 };
	AeronComputePass*          pass   = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &output,
		.write_texture_count = 1,
		.debug_label         = "Probe occlusion grid",
	});
	if (!pass) {
		return 0;
	}
	Aeron_BindComputePipeline(pass, o->pipeline);
	Aeron_BindComputeTextureSampler(pass, 0, o->occupancy, AeronSceneProbeOcclusion_Sampler(s));
	Aeron_BindComputeTextureSampler(pass, 1, pkg->probe_cubes, pkg->sampler);
	/* Unread in the first pass, which writes `unshaded`. */
	Aeron_BindComputeTextureSampler(pass, 2, shade_floor ? o->unshaded : o->grid,
									AeronSceneProbeOcclusion_Sampler(s));
	Aeron_BindComputeStorageBuffer(pass, 0, pkg->probe_buffer);
	Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
	Aeron_DispatchCompute(pass, (uint32_t)(count[0] + GRID_THREADS - 1) / GRID_THREADS,
						  (uint32_t)(count[1] + GRID_THREADS - 1) / GRID_THREADS,
						  (uint32_t)(count[2] + GRID_THREADS - 1) / GRID_THREADS);
	Aeron_EndComputePass(pass);
	return 1;
}

static void disable(AeronSceneProbeOcclusion* o, const char* why) {
	Aeron_LogWarn("aeron.scene", "probe occlusion off: %s", why);
	release_grids(o);
	o->failed = 1;
}

/* A list of model-metre boxes, kept with no two overlapping: each change's
 * own area, so far-apart changes do not rebuild everything between them. */
typedef struct BoxList {
	float lo[2 * AERON_SCENE_OCCLUSION_MAX_OCCLUDERS + 1][3];
	float hi[2 * AERON_SCENE_OCCLUSION_MAX_OCCLUDERS + 1][3];
	int   count;
} BoxList;

static void box_list_add(BoxList* list, const float lo[3], const float hi[3]) {
	float add_lo[3], add_hi[3];
	memcpy(add_lo, lo, sizeof add_lo);
	memcpy(add_hi, hi, sizeof add_hi);
	/* Absorb every box it overlaps, again until none is left. */
	for (int i = 0; i < list->count;) {
		if (boxes_overlap(list->lo[i], list->hi[i], add_lo, add_hi)) {
			grow_box(add_lo, add_hi, list->lo[i], list->hi[i]);
			list->count--;
			memcpy(list->lo[i], list->lo[list->count], sizeof list->lo[i]);
			memcpy(list->hi[i], list->hi[list->count], sizeof list->hi[i]);
			i = 0;
		} else {
			++i;
		}
	}
	memcpy(list->lo[list->count], add_lo, sizeof add_lo);
	memcpy(list->hi[list->count], add_hi, sizeof add_hi);
	list->count++;
}

/* The occupancy cells holding the model-metre box [blo, bhi], widened to
 * the `span`-cell lattice; 0 when empty. */
static int cell_range(const AeronSceneProbeOcclusion* o, const float blo[3], const float bhi[3], int span,
					  int lo[3], int hi[3]) {
	for (int k = 0; k < 3; ++k) {
		if (!(blo[k] <= bhi[k])) {
			return 0;
		}
		lo[k] = (int)floorf((blo[k] - o->origin[k]) / AERON_SCENE_OCCUPANCY_CELL) - 1;
		hi[k] = (int)ceilf((bhi[k] - o->origin[k]) / AERON_SCENE_OCCUPANCY_CELL) + 1;
		lo[k] = lo[k] < 0 ? 0 : (lo[k] / span) * span;
		hi[k] = ((hi[k] + span - 1) / span) * span;
		hi[k] = hi[k] > o->occupancy_dims[k] ? o->occupancy_dims[k] : hi[k];
		if (lo[k] >= hi[k]) {
			return 0;
		}
	}
	return 1;
}

/* The model-metre box of occupancy cells [lo, hi). */
static void cell_box(const AeronSceneProbeOcclusion* o, const int lo[3], const int hi[3], float blo[3],
					 float bhi[3]) {
	for (int k = 0; k < 3; ++k) {
		blo[k] = o->origin[k] + (float)lo[k] * AERON_SCENE_OCCUPANCY_CELL;
		bhi[k] = o->origin[k] + (float)hi[k] * AERON_SCENE_OCCUPANCY_CELL;
	}
}

/* The occupancy cells [lo, hi) of a box cell_box gave: cell_range would
 * widen it again, past the occluders drawn for it. */
static void box_cells(const AeronSceneProbeOcclusion* o, const float blo[3], const float bhi[3], int lo[3],
					  int hi[3]) {
	for (int k = 0; k < 3; ++k) {
		lo[k] = (int)lroundf((blo[k] - o->origin[k]) / AERON_SCENE_OCCUPANCY_CELL);
		hi[k] = (int)lroundf((bhi[k] - o->origin[k]) / AERON_SCENE_OCCUPANCY_CELL);
	}
}

/* The view along axis `a` from its high side (keeping the last surface
 * along it) or its low side (the first) of the model box [lo, hi]: world to
 * clip, row-major, depth 1 at the side it looks from, as the shadow views. */
static void view_matrix(const float w2m[12], const float lo[3], const float hi[3], int a, int high,
						float out[16]) {
	const int   u  = (a + 1) % 3;
	const int   v  = (a + 2) % 3;
	const float su = 2.0f / (hi[u] - lo[u]);
	const float sv = 2.0f / (hi[v] - lo[v]);
	const float sa = (high ? 1.0f : -1.0f) / (hi[a] - lo[a]);
	memset(out, 0, 16 * sizeof(float));
	for (int c = 0; c < 4; ++c) {
		out[0 * 4 + c] = su * w2m[u * 4 + c];
		out[1 * 4 + c] = sv * w2m[v * 4 + c];
		out[2 * 4 + c] = sa * w2m[a * 4 + c];
	}
	out[0 * 4 + 3] -= su * lo[u] + 1.0f;
	out[1 * 4 + 3] -= sv * lo[v] + 1.0f;
	out[2 * 4 + 3] -= sa * (high ? lo[a] : hi[a]);
	out[15] = 1.0f;
}

typedef struct Shelf {
	uint32_t x, y, height;
} Shelf;

/* Places a w x h tile in the view atlas, row by row; 0 when it is full. */
static int shelf_place(Shelf* shelf, uint32_t w, uint32_t h, uint32_t* x, uint32_t* y) {
	if (w > VIEW_ATLAS || h > VIEW_ATLAS) {
		return 0;
	}
	if (shelf->x + w > VIEW_ATLAS) {
		shelf->y += shelf->height;
		shelf->x      = 0;
		shelf->height = 0;
	}
	if (shelf->y + h > VIEW_ATLAS) {
		return 0;
	}
	*x = shelf->x;
	*y = shelf->y;
	shelf->x += w;
	shelf->height = shelf->height > h ? shelf->height : h;
	return 1;
}

/* Builds the occupancy on the GPU over `areas` (model metres), which come
 * back on the coarsest mip's lattice: the depth views of the occluders
 * reaching them, then their cells (compose) and their mips (downsample,
 * through the staging texture). 0 on failure. */
static int gpu_build(AeronScene3D* s, AeronCommandBuffer* cmd, const AeronSceneProbeSet* set,
					 BoxList* areas) {
	AeronSceneProbeOcclusion* o    = &s->probe_occlusion;
	const int                 span = 1 << (AERON_SCENE_OCCUPANCY_MIPS - 1);
	const float               cell = AERON_SCENE_OCCUPANCY_CELL;
	const float               tpm  = (float)VIEW_TEXELS_PER_CELL / cell;
	BoxList                   regions;
	int                       lo[3], hi[3];
	regions.count = 0;
	for (int i = 0; i < areas->count; ++i) {
		float blo[3], bhi[3];
		if (cell_range(o, areas->lo[i], areas->hi[i], span, lo, hi)) {
			cell_box(o, lo, hi, blo, bhi);
			box_list_add(&regions, blo, bhi);
		}
	}
	*areas = regions;
	if (regions.count == 0) {
		return 1;
	}

	/* The occluders reaching the areas, their views' boxes and tiles. */
	ViewRecord records[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS];
	uint16_t   casters[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS];
	int        count = 0;
	Shelf      shelf = { 0, 0, 0 };
	for (int i = 0; i < o->occluder_count; ++i) {
		const AeronSceneOccluder* occluder = &o->occluders[i];
		int                       reaches  = 0;
		for (int r = 0; r < regions.count && !reaches; ++r) {
			reaches = boxes_overlap(occluder->lo, occluder->hi, regions.lo[r], regions.hi[r]);
		}
		if (!reaches) {
			continue;
		}
		ViewRecord* record = &records[count];
		memset(record, 0, sizeof *record);
		for (int k = 0; k < 3; ++k) {
			record->lo[k] = o->origin[k] + (floorf((occluder->lo[k] - o->origin[k]) / cell) - 1.0f) * cell;
			record->hi[k] = o->origin[k] + (ceilf((occluder->hi[k] - o->origin[k]) / cell) + 1.0f) * cell;
		}
		int placed = 1;
		for (int a = 0; a < 3 && placed; ++a) {
			const uint32_t w = (uint32_t)lroundf((record->hi[(a + 1) % 3] - record->lo[(a + 1) % 3]) * tpm);
			const uint32_t h = (uint32_t)lroundf((record->hi[(a + 2) % 3] - record->lo[(a + 2) % 3]) * tpm);
			placed           = shelf_place(&shelf, w, h, &record->tiles[a][0], &record->tiles[a][1]) &&
							   shelf_place(&shelf, w, h, &record->tiles[a][2], &record->tiles[a][3]);
		}
		if (!placed) {
			if (!o->dropped_warned) {
				Aeron_LogWarn("aeron.scene",
							  "probe occlusion: the occluders' views exceed their atlas; some are left out");
				o->dropped_warned = 1;
			}
			break;
		}
		casters[count++] = occluder->caster;
	}

	AeronRenderPass* views = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.depth_target      = o->views,
		.viewport          = { 0, 0, VIEW_ATLAS, VIEW_ATLAS },
		.scissor           = { 0, 0, VIEW_ATLAS, VIEW_ATLAS },
		.clear_depth       = 1,
		.clear_depth_value = 0.0f,
		.command_buffer    = cmd,
		.debug_label       = "Probe occupancy views",
	});
	if (!views) {
		return 0;
	}
	for (int r = 0; r < count; ++r) {
		for (int a = 0; a < 3; ++a) {
			const int w = (int)lroundf((records[r].hi[(a + 1) % 3] - records[r].lo[(a + 1) % 3]) * tpm);
			const int h = (int)lroundf((records[r].hi[(a + 2) % 3] - records[r].lo[(a + 2) % 3]) * tpm);
			for (int side = 0; side < 2; ++side) {
				const AeronRectI tile = { (int)records[r].tiles[a][2 * side],
										  (int)records[r].tiles[a][2 * side + 1], w, h };
				float            view_proj[16];
				view_matrix(set->world_to_model, records[r].lo, records[r].hi, a, side == 0, view_proj);
				Aeron_SetViewport(views, &tile);
				Aeron_SetScissor(views, &tile);
				if (!AeronSceneDirectionalShadow_DrawDepth(s, views, view_proj, &casters[r], 1)) {
					Aeron_EndRenderPass(views);
					return 0;
				}
			}
		}
	}
	Aeron_EndRenderPass(views);
	if (count > 0 &&
		!Aeron_UploadBufferDataCmd(cmd, o->records, 0, records, (uint32_t)(count * sizeof *records))) {
		return 0;
	}

	/* The cells of the areas. */
	AeronComputeTextureBinding level0  = { o->occupancy, 0, 0, 0 };
	AeronComputePass*          compose = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &level0,
		.write_texture_count = 1,
		.debug_label         = "Probe occupancy compose",
	});
	if (!compose) {
		return 0;
	}
	Aeron_BindComputePipeline(compose, o->compose);
	Aeron_BindComputeTextureSampler(compose, 0, Aeron_DepthTargetGetTexture(o->views),
									AeronSceneProbeOcclusion_Sampler(s));
	Aeron_BindComputeStorageBuffer(compose, 0, o->records);
	for (int r = 0; r < regions.count; ++r) {
		box_cells(o, regions.lo[r], regions.hi[r], lo, hi);
		ComposeUniform u = { { o->origin[0], o->origin[1], o->origin[2], cell },
							 { lo[0], lo[1], lo[2], 0 },
							 { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], count },
							 { tpm, 0.0f, 0.0f, 0.0f } };
		Aeron_BindComputeUniformData(compose, 0, &u, sizeof u);
		Aeron_DispatchCompute(compose, (uint32_t)(u.region_size[0] + BUILD_THREADS - 1) / BUILD_THREADS,
							  (uint32_t)(u.region_size[1] + BUILD_THREADS - 1) / BUILD_THREADS,
							  (uint32_t)(u.region_size[2] + BUILD_THREADS - 1) / BUILD_THREADS);
	}
	Aeron_EndComputePass(compose);

	/* Their mips, each level from the one above. */
	for (int level = 1; level < AERON_SCENE_OCCUPANCY_MIPS; ++level) {
		int dims[3], source[3], region_lo[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS * 2 + 1][3],
			region_hi[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS * 2 + 1][3];
		for (int k = 0; k < 3; ++k) {
			dims[k]   = o->occupancy_dims[k] >> level > 0 ? o->occupancy_dims[k] >> level : 1;
			source[k] = o->occupancy_dims[k] >> (level - 1) > 0 ? o->occupancy_dims[k] >> (level - 1) : 1;
		}
		/* A frame still in flight may be copying the last build's levels:
		 * the first level starts on a fresh staging texture. */
		AeronComputeTextureBinding out  = { o->staging, (uint32_t)(level - 1), 0, level == 1 };
		AeronComputePass*          pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
			.command_buffer      = cmd,
			.write_textures      = &out,
			.write_texture_count = 1,
			.debug_label         = "Probe occupancy mip",
		});
		if (!pass) {
			return 0;
		}
		Aeron_BindComputePipeline(pass, o->downsample);
		Aeron_BindComputeTextureSampler(pass, 0, o->occupancy, AeronSceneProbeOcclusion_Sampler(s));
		for (int r = 0; r < regions.count; ++r) {
			box_cells(o, regions.lo[r], regions.hi[r], lo, hi);
			for (int k = 0; k < 3; ++k) {
				region_lo[r][k] = lo[k] >> level;
				region_hi[r][k] = (hi[k] + (1 << level) - 1) >> level;
				region_hi[r][k] = region_hi[r][k] > dims[k] ? dims[k] : region_hi[r][k];
			}
			DownsampleUniform u = {
				{ region_lo[r][0], region_lo[r][1], region_lo[r][2], 0 },
				{ region_hi[r][0] - region_lo[r][0], region_hi[r][1] - region_lo[r][1],
				  region_hi[r][2] - region_lo[r][2], level - 1 },
				{ source[0], source[1], source[2], 0 },
			};
			Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
			Aeron_DispatchCompute(pass, (uint32_t)(u.region_size[0] + BUILD_THREADS - 1) / BUILD_THREADS,
								  (uint32_t)(u.region_size[1] + BUILD_THREADS - 1) / BUILD_THREADS,
								  (uint32_t)(u.region_size[2] + BUILD_THREADS - 1) / BUILD_THREADS);
		}
		Aeron_EndComputePass(pass);
		for (int r = 0; r < regions.count; ++r) {
			if (!Aeron_CopyTextureCmd(cmd, &(AeronTextureCopyDesc) {
											   .source                = o->staging,
											   .source_mip_level      = (uint32_t)(level - 1),
											   .source_x              = (uint32_t)region_lo[r][0],
											   .source_y              = (uint32_t)region_lo[r][1],
											   .source_z              = (uint32_t)region_lo[r][2],
											   .destination           = o->occupancy,
											   .destination_mip_level = (uint32_t)level,
											   .destination_x         = (uint32_t)region_lo[r][0],
											   .destination_y         = (uint32_t)region_lo[r][1],
											   .destination_z         = (uint32_t)region_lo[r][2],
											   .width  = (uint32_t)(region_hi[r][0] - region_lo[r][0]),
											   .height = (uint32_t)(region_hi[r][1] - region_lo[r][1]),
											   .depth  = (uint32_t)(region_hi[r][2] - region_lo[r][2]),
										   })) {
				return 0;
			}
		}
	}
	return 1;
}

void AeronSceneProbeOcclusion_Update(AeronScene3D* s, AeronCommandBuffer* cmd) {
	AeronSceneProbeOcclusion* o = &s->probe_occlusion;
	o->active                   = 0;
	if (!s->probe_occlusion_enabled || o->failed || s->probe_set_count == 0) {
		return;
	}
	const AeronSceneProbeSet* set = &s->probe_sets[0];
	if (!set->package || !set->package->probe_buffer || !set->package->probe_cubes) {
		return;
	}
	if (!ensure_grids(o, set)) {
		disable(o, "its grids could not be created");
		return;
	}
	if (!ensure_pipeline(o)) {
		disable(o, "its compute shader did not load");
		return;
	}
	float box_lo[3], box_hi[3];
	for (int k = 0; k < 3; ++k) {
		box_lo[k] = o->origin[k];
		box_hi[k] = o->origin[k] + set->package->settings.grid_size[k];
	}

	AeronSceneOccluder            current[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS];
	const AeronSceneMeshInstance* instances[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS];
	unsigned char                 changed[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS] = { 0 };
	const int                     count = gather_occluders(s, set, box_lo, box_hi, current, instances);

	/* What changed: the occluders without a match in the other list. The
	 * removed ones' areas are cleared; the others keep their posed bounds. */
	BoxList clears, uploads, grids;
	clears.count    = 0;
	int any_changed = !o->valid;
	if (!o->valid) {
		box_list_add(&clears, box_lo, box_hi);
	}
	unsigned char matched[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS] = { 0 };
	for (int i = 0; i < count; ++i) {
		changed[i] = 1;
		for (int j = 0; o->valid && j < o->occluder_count && changed[i]; ++j) {
			if (!matched[j] && o->occluders[j].mesh == current[i].mesh &&
				o->occluders[j].hash == current[i].hash) {
				matched[j] = 1;
				changed[i] = 0;
				memcpy(current[i].lo, o->occluders[j].lo, sizeof current[i].lo);
				memcpy(current[i].hi, o->occluders[j].hi, sizeof current[i].hi);
			}
		}
		any_changed |= changed[i];
	}
	for (int j = 0; o->valid && j < o->occluder_count; ++j) {
		if (!matched[j]) {
			box_list_add(&clears, o->occluders[j].lo, o->occluders[j].hi);
			any_changed = 1;
		}
	}
	if (!any_changed) {
		memcpy(o->occluders, current, (size_t)count * sizeof *current);
		o->occluder_count = count;
		o->active         = 1;
		o->frames_since_build += o->frames_since_build < REBUILD_FRAMES;
		return;
	}
	/* The CPU path's previous occluders stay recorded until its rebuild,
	 * which then takes every change since. */
	o->frames_since_build += o->frames_since_build < REBUILD_FRAMES;
	if (!o->gpu && o->valid && o->frames_since_build < REBUILD_FRAMES) {
		o->active = 1;
		return;
	}
	o->frames_since_build = 0;

	if (o->gpu) {
		/* The removed occluders' areas and the changed ones'. */
		uploads = clears;
		for (int i = 0; i < count; ++i) {
			if (changed[i]) {
				box_list_add(&uploads, current[i].lo, current[i].hi);
			}
		}
		memcpy(o->occluders, current, (size_t)count * sizeof *current);
		o->occluder_count = count;
		if (!gpu_build(s, cmd, set, &uploads)) {
			disable(o, "the occupancy build failed");
			return;
		}
	} else {
		/* Clear the removed occluders' areas, on the coarsest mip's lattice,
		 * restamp the unchanged occluders there, then stamp the changed ones. */
		const int span = 1 << (AERON_SCENE_OCCUPANCY_MIPS - 1);
		int       lo[3], hi[3];
		uploads.count = 0;
		for (int c = 0; c < clears.count; ++c) {
			if (!cell_range(o, clears.lo[c], clears.hi[c], span, lo, hi)) {
				continue;
			}
			for (int z = lo[2]; z < hi[2]; ++z) {
				for (int y = lo[1]; y < hi[1]; ++y) {
					memset(&o->cells[((size_t)z * o->occupancy_dims[1] + y) * o->occupancy_dims[0] + lo[0]],
						   0, (size_t)(hi[0] - lo[0]));
				}
			}
			cell_box(o, lo, hi, clears.lo[c], clears.hi[c]);
			box_list_add(&uploads, clears.lo[c], clears.hi[c]);
		}
		CrossingList crossings = { 0 };
		for (int i = 0; i < count; ++i) {
			int restamp = 0;
			for (int c = 0; c < clears.count && !changed[i] && !restamp; ++c) {
				restamp = boxes_overlap(current[i].lo, current[i].hi, clears.lo[c], clears.hi[c]);
			}
			if (!changed[i] && !restamp) {
				continue;
			}
			if (!voxelize(o, instances[i], set->world_to_model, &crossings, current[i].lo, current[i].hi)) {
				free(crossings.items);
				disable(o, "out of memory");
				return;
			}
			if (changed[i]) {
				box_list_add(&uploads, current[i].lo, current[i].hi);
			}
		}
		free(crossings.items);
		memcpy(o->occluders, current, (size_t)count * sizeof *current);
		o->occluder_count = count;
		for (int u = 0; u < uploads.count; ++u) {
			if (cell_range(o, uploads.lo[u], uploads.hi[u], span, lo, hi)) {
				if (!upload_occupancy(o, cmd, lo, hi)) {
					disable(o, "the occupancy upload failed");
					return;
				}
			}
		}
	}
	/* The grid cells whose rays reach a change. */
	grids.count = 0;
	for (int u = 0; u < uploads.count; ++u) {
		float glo[3], ghi[3];
		for (int k = 0; k < 3; ++k) {
			glo[k] = uploads.lo[u][k] - (o->valid ? OCCLUSION_REACH : 0.0f);
			ghi[k] = uploads.hi[u][k] + (o->valid ? OCCLUSION_REACH : 0.0f);
		}
		box_list_add(&grids, glo, ghi);
	}
	/* What changed: the cells of the removed and changed occluders, which
	 * reach two cells past their bounds. A cell none of whose rays meets
	 * them would come out the same. */
	float change_lo[2 * AERON_SCENE_OCCLUSION_MAX_OCCLUDERS + 1][3];
	float change_hi[2 * AERON_SCENE_OCCLUSION_MAX_OCCLUDERS + 1][3];
	int   change_count = 0;
	if (o->valid) {
		const float grow = 2.0f * AERON_SCENE_OCCUPANCY_CELL;
		for (int c = 0; c < clears.count + count; ++c) {
			const int    cleared = c < clears.count;
			const float* blo     = cleared ? clears.lo[c] : current[c - clears.count].lo;
			const float* bhi     = cleared ? clears.hi[c] : current[c - clears.count].hi;
			if (!cleared && !changed[c - clears.count]) {
				continue;
			}
			for (int k = 0; k < 3; ++k) {
				change_lo[change_count][k] = blo[k] - grow;
				change_hi[change_count][k] = bhi[k] + grow;
			}
			++change_count;
		}
		change_count = merge_boxes(change_lo, change_hi, change_count);
	}
	/* All of the first pass, then the second, which reads its floor. */
	for (int shade_floor = 0; shade_floor < 2; ++shade_floor) {
		for (int g = 0; g < grids.count; ++g) {
			if (!build_grid(s, cmd, set, grids.lo[g], grids.hi[g], (const float (*)[3])change_lo,
							(const float (*)[3])change_hi, change_count, shade_floor)) {
				disable(o, "the grid build failed");
				return;
			}
		}
	}
	o->valid  = 1;
	o->active = 1;
}
