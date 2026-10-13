/* Each OPT mesh's LOD 0 as glTF vertices and per-face-group indices:
 * normals (stored, remapped and repaired as the classic renderer, or
 * regenerated), tangents, and the meshes without faces. */

#include <math.h>

#include "aeron/mesh_normals.h"
#include "mikktspace.h"
#include "opt_gltf_internal.h"

/* ===== Per-OPT-mesh emit ============================================ */

void opt_gltf_built_mesh_free(BuiltMesh* bm) {
	free(bm->vertices);
	free(bm->indices);
	free(bm->fg_index_start);
	free(bm->fg_index_count_arr);
	memset(bm, 0, sizeof *bm);
}

static uint32_t built_mesh_intern_vertex(BuiltMesh* bm, const GltfVertex* v) {
	/* Linear dedup. fine for OPT mesh sizes. */
	for (uint32_t i = 0; i < bm->vertex_count; ++i) {
		const GltfVertex* e = &bm->vertices[i];
		if (e->pos[0] == v->pos[0] && e->pos[1] == v->pos[1] && e->pos[2] == v->pos[2] &&
			e->nrm[0] == v->nrm[0] && e->nrm[1] == v->nrm[1] && e->nrm[2] == v->nrm[2] &&
			e->uv[0] == v->uv[0] && e->uv[1] == v->uv[1] && e->tan[0] == v->tan[0] &&
			e->tan[1] == v->tan[1] && e->tan[2] == v->tan[2] && e->tan[3] == v->tan[3])
			return i;
	}
	if (bm->vertex_count >= bm->vertex_cap) {
		uint32_t    ncap  = bm->vertex_cap ? bm->vertex_cap * 2 : 64;
		GltfVertex* grown = (GltfVertex*)realloc(bm->vertices, ncap * sizeof *grown);
		if (!grown) {
			bm->out_of_memory = true;
			return 0;
		}
		bm->vertices   = grown;
		bm->vertex_cap = ncap;
	}
	bm->vertices[bm->vertex_count] = *v;
	return bm->vertex_count++;
}

static void built_mesh_emit_index(BuiltMesh* bm, uint32_t v) {
	if (bm->index_count >= bm->index_cap) {
		uint32_t  ncap  = bm->index_cap ? bm->index_cap * 2 : 128;
		uint32_t* grown = (uint32_t*)realloc(bm->indices, ncap * sizeof *grown);
		if (!grown) {
			bm->out_of_memory = true;
			return;
		}
		bm->indices   = grown;
		bm->index_cap = ncap;
	}
	bm->indices[bm->index_count++] = v;
}

/* ===== Angle-based normal regeneration =============================
 *
 * This path deliberately ignores all normals stored in the OPT. Some
 * shipped models contain mutually incompatible per-corner normals, and
 * even their stored face normals need not be derived consistently from
 * the polygon geometry.
 *
 * A smoothing angle describes EDGES, not an arbitrary set of faces that
 * happen to reference the same position. Build the connected smoothing
 * fan containing the current corner by walking shared edges whose
 * geometric face normals meet the threshold. Faces that touch only at a
 * point, or belong to a different fan separated by a hard edge, cannot
 * contaminate the result.
 *
 * Within the fan, weight each geometric face normal by its corner angle.
 * This is invariant under ordinary triangulation of a flat polygon: the
 * diagonal of a two-triangle panel no longer gives one side extra weight.
 * It also avoids making a dense patch of small triangles dominate a
 * neighbouring coarse patch. */
/* Sets `error`'s message; returns false. */
static bool mesh_fail(opt_error_t* error, const char* format, ...) {
	if (error) {
		va_list args;
		va_start(args, format);
		vsnprintf(error->msg, sizeof error->msg, format, args);
		va_end(args);
	}
	return false;
}

static float* built_mesh_build_corner_normals(const opt_mesh_t* mesh, const opt_lod_t* lod,
											  float smooth_angle_degrees, opt_error_t* error) {
	uint32_t triangle_count = 0;
	for (int32_t group = 0; group < lod->group_count; ++group) {
		const opt_face_group_t* face_group = &lod->groups[group];
		for (int32_t face = 0; face < face_group->face_count; ++face)
			triangle_count += face_group->faces[face].verts[3] >= 0 ? 2u : 1u;
	}
	if (!triangle_count || mesh->vertex_count <= 0)
		return NULL;

	uint32_t* indices = malloc((size_t)triangle_count * 3 * sizeof *indices);
	float*    normals = malloc((size_t)triangle_count * 9 * sizeof *normals);
	if (!indices || !normals) {
		free(indices);
		free(normals);
		mesh_fail(error, "out of memory");
		return NULL;
	}
	uint32_t triangle = 0;
	for (int32_t group = 0; group < lod->group_count; ++group) {
		const opt_face_group_t* face_group = &lod->groups[group];
		for (int32_t face = 0; face < face_group->face_count; ++face) {
			const opt_face_t* source              = &face_group->faces[face];
			const int         source_corner[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
			const int         count               = source->verts[3] >= 0 ? 2 : 1;
			for (int item = 0; item < count; ++item, ++triangle) {
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t vertex = source->verts[source_corner[item][corner]];
					if (vertex < 0 || vertex >= mesh->vertex_count) {
						free(indices);
						free(normals);
						return NULL;
					}
					indices[(size_t)triangle * 3 + corner] = (uint32_t)vertex;
				}
			}
		}
	}
	AeronMeshNormalsError       normals_error = { 0 };
	const AeronMeshNormalsInput input         = {
		.positions                 = &mesh->vertices[0].x,
		.position_stride           = sizeof mesh->vertices[0],
		.position_count            = (uint32_t)mesh->vertex_count,
		.triangle_position_indices = indices,
		.triangle_count            = triangle_count,
		.smooth_angle_degrees      = smooth_angle_degrees,
	};
	const bool built = Aeron_MeshNormalsBuildCorners(&input, normals, &normals_error);
	free(indices);
	if (!built) {
		mesh_fail(error, "normal build failed: %s", normals_error.message);
		free(normals);
		return NULL;
	}
	return normals;
}

static uint32_t built_mesh_emit_corner(BuiltMesh* bm, const opt_mesh_t* m, int32_t vi, int32_t ui,
									   const opt_vec3_t* normal, float meters_per_opt_unit) {
	const opt_vec3_t* p  = &m->vertices[vi];
	const opt_vec2_t* uv = (ui >= 0 && ui < m->uv_count) ? &m->uvs[ui] : NULL;
	GltfVertex        gv = { 0 };
	swap_axis_v3(p, gv.pos);
	gv.pos[0] *= meters_per_opt_unit;
	gv.pos[1] *= meters_per_opt_unit;
	gv.pos[2] *= meters_per_opt_unit;
	swap_axis_v3(normal, gv.nrm);
	/* Stored OPT normals are decoded from Q15 and may be slightly off
	 * unit length; regenerated normals are already unit length, but use
	 * the same final normalization for both paths. */
	float nl = sqrtf(gv.nrm[0] * gv.nrm[0] + gv.nrm[1] * gv.nrm[1] + gv.nrm[2] * gv.nrm[2]);
	if (nl > 1e-9f) {
		gv.nrm[0] /= nl;
		gv.nrm[1] /= nl;
		gv.nrm[2] /= nl;
	}
	if (uv) {
		gv.uv[0] = uv->u;
		gv.uv[1] = uv->v;
	}
	return built_mesh_intern_vertex(bm, &gv);
}

/* The triangles a mesh has emitted, by their corners (rotated to start at
 * the smallest index, so a reversed triangle, the back of a two-sided
 * surface, is another key), with the face group that emitted each. */
typedef struct {
	uint32_t v[3];
	int32_t  group; /* -1: empty slot */
} SeenTriangle;

typedef struct {
	SeenTriangle* slots;
	uint32_t      cap; /* a power of 2 */
} TriangleSet;

static bool triangle_set_init(TriangleSet* set, const opt_lod_t* lod) {
	uint32_t triangles = 0;
	for (int32_t g = 0; g < lod->group_count; ++g)
		for (int32_t f = 0; f < lod->groups[g].face_count; ++f)
			triangles += lod->groups[g].faces[f].verts[3] >= 0 ? 2u : 1u;
	set->cap = 16;
	while (set->cap < triangles * 2u)
		set->cap *= 2;
	set->slots = (SeenTriangle*)malloc(set->cap * sizeof *set->slots);
	for (uint32_t i = 0; set->slots && i < set->cap; ++i)
		set->slots[i].group = -1;
	return set->slots != NULL;
}

/* The slot of triangle `v` (rotated), found or empty. */
static SeenTriangle* triangle_set_slot(TriangleSet* set, const uint32_t v[3]) {
	uint32_t hash = (v[0] * 73856093u) ^ (v[1] * 19349663u) ^ (v[2] * 83492791u);
	for (uint32_t i = hash & (set->cap - 1);; i = (i + 1) & (set->cap - 1)) {
		SeenTriangle* slot = &set->slots[i];
		if (slot->group < 0 || (slot->v[0] == v[0] && slot->v[1] == v[1] && slot->v[2] == v[2]))
			return slot;
	}
}

static bool same_textures(const opt_face_group_t* a, const opt_face_group_t* b) {
	if (a->texture_index != b->texture_index || a->state_count != b->state_count)
		return false;
	for (int32_t s = 0; a->state_textures && b->state_textures && s < a->state_count; ++s)
		if (a->state_textures[s] != b->state_textures[s])
			return false;
	return true;
}

/* Emits triangle (a, b, c) of face group `g`, unless the mesh already has
 * it with the same textures: some shipped OPTs list a face twice, which
 * draws the same pixels, and which Blender cannot hold (two faces on the
 * same vertices). A repeat with another texture is kept: which one shows
 * is not data to drop. */
static void built_mesh_emit_triangle(BuiltMesh* bm, TriangleSet* seen, const opt_lod_t* lod, int32_t g,
									 uint32_t a, uint32_t b, uint32_t c) {
	uint32_t v[3] = { a, b, c };
	while (v[0] > v[1] || v[0] > v[2]) {
		const uint32_t first = v[0];
		v[0]                 = v[1];
		v[1]                 = v[2];
		v[2]                 = first;
	}
	SeenTriangle* slot = triangle_set_slot(seen, v);
	if (slot->group >= 0) {
		if (same_textures(&lod->groups[slot->group], &lod->groups[g])) {
			bm->skipped_repeats++;
			return;
		}
		bm->kept_repeats++;
	} else {
		memcpy(slot->v, v, sizeof v);
		slot->group = g;
	}
	built_mesh_emit_index(bm, a);
	built_mesh_emit_index(bm, b);
	built_mesh_emit_index(bm, c);
}

/* Walk LOD 0 of an OPT mesh and emit deduplicated vertices + indices
 * grouped by face group. Skips degenerate faces and empty groups.
 * `regen` enables angle-based normal regeneration; when false the stored
 * OPT normals are used. */
bool opt_gltf_built_mesh_from_opt(BuiltMesh* bm, const opt_mesh_t* m, float meters_per_opt_unit, bool regen,
								  float smooth_angle_degrees, bool repair_normals, opt_error_t* error) {
	memset(bm, 0, sizeof *bm);
	if (error)
		error->msg[0] = '\0';
	if (m->lod_count <= 0 || m->lods[0].group_count <= 0)
		return mesh_fail(error, "no LOD 0 triangles");
	const opt_lod_t* lod = &m->lods[0];

	bm->fg_index_start     = (uint32_t*)calloc(lod->group_count, sizeof(uint32_t));
	bm->fg_index_count_arr = (uint32_t*)calloc(lod->group_count, sizeof(uint32_t));
	if (!bm->fg_index_start || !bm->fg_index_count_arr)
		return mesh_fail(error, "out of memory");

	TriangleSet seen = { 0 };
	if (!triangle_set_init(&seen, lod)) {
		opt_gltf_built_mesh_free(bm);
		return mesh_fail(error, "out of memory");
	}
	float* corner_normals =
		regen ? built_mesh_build_corner_normals(m, lod, smooth_angle_degrees, error) : NULL;
	if (regen && !corner_normals) {
		free(seen.slots);
		opt_gltf_built_mesh_free(bm);
		return error && error->msg[0] ? false : mesh_fail(error, "no LOD 0 triangles");
	}

	/* The classic renderer resets g_vertexRemap for every FaceData group,
	 * then computes lighting only when a POSITION index is first seen in
	 * the visible, file-ordered face list. Later corners with the same
	 * position reuse that already-lit vertex even when their normal index
	 * differs. Many shipped OPTs rely on this: the ignored corner normals
	 * can disagree by more than 90 degrees across the diagonal of an
	 * otherwise coplanar two-triangle panel.
	 *
	 * A static glTF cannot reproduce the view-dependent "first visible"
	 * choice at a non-coplanar junction. File order is exact for coplanar
	 * pairs, however, because both faces have identical culling, and is the
	 * closest stable representation elsewhere. Keep repair_normals false as
	 * the raw per-corner diagnostic path, so this compatibility remap is
	 * enabled together with the normal sanitization default. */
	opt_vec3_t* classic_normals    = NULL;
	uint8_t*    classic_normal_set = NULL;
	if (!regen && repair_normals && m->vertex_count > 0) {
		classic_normals    = (opt_vec3_t*)calloc((size_t)m->vertex_count, sizeof *classic_normals);
		classic_normal_set = (uint8_t*)calloc((size_t)m->vertex_count, sizeof *classic_normal_set);
		if (!classic_normals || !classic_normal_set) {
			free(classic_normals);
			free(classic_normal_set);
			free(corner_normals);
			free(seen.slots);
			return mesh_fail(error, "out of memory");
		}
	}

	int32_t smooth_face_index = 0;
	for (int32_t g = 0; g < lod->group_count; ++g) {
		const opt_face_group_t* grp = &lod->groups[g];
		if (grp->face_count <= 0)
			continue;
		if (classic_normal_set) {
			memset(classic_normal_set, 0, (size_t)m->vertex_count);
		}
		uint32_t start = bm->index_count;
		for (int32_t fi = 0; fi < grp->face_count; ++fi) {
			const opt_face_t* f       = &grp->faces[fi];
			int               corners = (f->verts[3] >= 0) ? 4 : 3;

			if (regen) {
				int source_corner[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
				int triangle_count      = (corners == 4) ? 2 : 1;
				for (int t = 0; t < triangle_count; ++t, ++smooth_face_index) {
					uint32_t v[3] = { 0 };
					int      ok   = 1;
					for (int q = 0; q < 3; ++q) {
						int     k  = source_corner[t][q];
						int32_t vi = f->verts[k];
						if (vi < 0 || vi >= m->vertex_count) {
							ok = 0;
							break;
						}
						static const int normal_corner[3] = { 0, 2, 1 };
						const float*     normal =
							&corner_normals[((size_t)smooth_face_index * 3 + normal_corner[q]) * 3];
						opt_vec3_t n = { normal[0], normal[1], normal[2] };
						v[q]         = built_mesh_emit_corner(bm, m, vi, f->uvs[k], &n, meters_per_opt_unit);
					}
					if (!ok)
						continue;
					built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[1], v[2]);
				}
				continue;
			}

			uint32_t v[4] = { 0 };
			int      ok   = 1;
			for (int k = 0; k < corners; ++k) {
				int32_t vi = f->verts[k];
				int32_t ni = f->normals[k];
				if (vi < 0 || vi >= m->vertex_count) {
					ok = 0;
					break;
				}
				const opt_vec3_t* n = (ni >= 0 && ni < m->normal_count) ? &m->normals[ni] : &f->face_normal;
				if (classic_normal_set) {
					if (!classic_normal_set[vi]) {
						/* Sanitize only the canonical, first-use normal.
						 * Later per-corner normals are ignored by the
						 * classic renderer and must not be repaired
						 * independently: doing so turns them into real
						 * glTF seams. */
						double nlen2 = (double)n->x * n->x + (double)n->y * n->y + (double)n->z * n->z;
						if (nlen2 < 1e-12) {
							classic_normals[vi] = f->face_normal;
							bm->fixed_zero_normals++;
						} else {
							double d = (double)n->x * f->face_normal.x + (double)n->y * f->face_normal.y +
									   (double)n->z * f->face_normal.z;
							if (d < 0.0) {
								classic_normals[vi] = (opt_vec3_t) { -n->x, -n->y, -n->z };
								bm->fixed_flipped_normals++;
							} else {
								classic_normals[vi] = *n;
							}
						}
						classic_normal_set[vi] = 1;
					}
					n = &classic_normals[vi];
				}
				v[k] = built_mesh_emit_corner(bm, m, vi, f->uvs[k], n, meters_per_opt_unit);
			}
			if (!ok)
				continue;
			built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[1], v[2]);
			if (corners == 4)
				built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[2], v[3]);
		}
		bm->fg_index_start[g]     = start;
		bm->fg_index_count_arr[g] = bm->index_count - start;
		if (bm->fg_index_count_arr[g] > 0)
			bm->fg_count++;
	}
	free(corner_normals);
	free(classic_normals);
	free(classic_normal_set);
	free(seen.slots);
	if (bm->out_of_memory)
		return mesh_fail(error, "out of memory");
	return bm->index_count > 0 || mesh_fail(error, "no LOD 0 triangles");
}

/* ===== Tangents ======================================================
 *
 * MikkTSpace over the mesh's final triangle list, in the glTF frame. glTF
 * puts the texture's first row at v = 0 and wants the bitangent
 * cross(normal, tangent) * w to point toward that row (+Y of a normal map is
 * "up" in the image), so MikkTSpace sees (u, 1 - v): its bitangent then
 * follows decreasing v and its sign is the glTF w. Vertices are re-interned
 * with their tangent, splitting where corner frames differ. */

typedef struct TangentBuild {
	const BuiltMesh* bm;
	float*           corner_tangents; /* index_count * 4 */
} TangentBuild;

static const GltfVertex* tangent_corner(const SMikkTSpaceContext* ctx, int face, int vert) {
	const TangentBuild* tb = (const TangentBuild*)ctx->m_pUserData;
	return &tb->bm->vertices[tb->bm->indices[(size_t)face * 3 + (size_t)vert]];
}

static int tangent_face_count(const SMikkTSpaceContext* ctx) {
	return (int)(((const TangentBuild*)ctx->m_pUserData)->bm->index_count / 3);
}

static int tangent_face_vertices(const SMikkTSpaceContext* ctx, const int face) {
	(void)ctx;
	(void)face;
	return 3;
}

static void tangent_position(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	memcpy(out, tangent_corner(ctx, face, vert)->pos, 3 * sizeof(float));
}

static void tangent_normal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	memcpy(out, tangent_corner(ctx, face, vert)->nrm, 3 * sizeof(float));
}

static void tangent_texcoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	const GltfVertex* v = tangent_corner(ctx, face, vert);
	out[0]              = v->uv[0];
	out[1]              = 1.0f - v->uv[1];
}

static void tangent_store(const SMikkTSpaceContext* ctx, const float tangent[], const float sign,
						  const int face, const int vert) {
	float* out = &((TangentBuild*)ctx->m_pUserData)->corner_tangents[((size_t)face * 3 + (size_t)vert) * 4];
	if (isfinite(tangent[0]) && isfinite(tangent[1]) && isfinite(tangent[2])) {
		memcpy(out, tangent, 3 * sizeof(float));
	} else {
		out[0] = 1.0f;
		out[1] = out[2] = 0.0f;
	}
	out[3] = sign < 0.0f ? -1.0f : 1.0f;
}

bool opt_gltf_generate_tangents(BuiltMesh* bm) {
	if (bm->index_count == 0)
		return true;
	TangentBuild tb = { bm, (float*)calloc((size_t)bm->index_count * 4, sizeof(float)) };
	if (!tb.corner_tangents)
		return false;
	SMikkTSpaceInterface iface = {
		.m_getNumFaces          = tangent_face_count,
		.m_getNumVerticesOfFace = tangent_face_vertices,
		.m_getPosition          = tangent_position,
		.m_getNormal            = tangent_normal,
		.m_getTexCoord          = tangent_texcoord,
		.m_setTSpaceBasic       = tangent_store,
	};
	SMikkTSpaceContext ctx = { &iface, &tb };
	if (!genTangSpaceDefault(&ctx)) {
		free(tb.corner_tangents);
		return false;
	}
	BuiltMesh rebuilt = { 0 };
	for (uint32_t k = 0; k < bm->index_count; ++k) {
		GltfVertex v = bm->vertices[bm->indices[k]];
		memcpy(v.tan, &tb.corner_tangents[(size_t)k * 4], sizeof v.tan);
		bm->indices[k] = built_mesh_intern_vertex(&rebuilt, &v);
	}
	free(tb.corner_tangents);
	free(bm->vertices);
	bm->vertices     = rebuilt.vertices;
	bm->vertex_count = rebuilt.vertex_count;
	bm->vertex_cap   = rebuilt.vertex_cap;
	bm->has_tangents = true;
	return !rebuilt.out_of_memory;
}

/* ===== Faceless meshes ============================================== */

static int32_t opt_mesh_lod0_face_count(const opt_mesh_t* m) {
	int32_t faces = 0;
	if (m->lod_count > 0) {
		for (int32_t g = 0; g < m->lods[0].group_count; ++g)
			faces += m->lods[0].groups[g].face_count;
	}
	return faces;
}

void opt_gltf_faceless_patch_free(FacelessPatch* patch) {
	free(patch->meshes);
	free(patch->lods);
	free(patch->groups);
	free(patch->faces);
}

/* Returns `opt` when nothing needs patching, the patched copy otherwise, or
 * NULL when out of memory. */
const opt_file_t* opt_gltf_faceless_patch_apply(const opt_file_t* opt, const OptGltfBuildOptions* options,
												FacelessPatch* patch) {
	memset(patch, 0, sizeof *patch);
	int32_t count = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		count += opt->meshes[mi].vertex_count > 0 && opt_mesh_lod0_face_count(&opt->meshes[mi]) == 0;
	if (count == 0)
		return opt;
	patch->file   = *opt;
	patch->meshes = (opt_mesh_t*)malloc((size_t)opt->mesh_count * sizeof *patch->meshes);
	patch->lods   = (opt_lod_t*)calloc((size_t)count, sizeof *patch->lods);
	patch->groups = (opt_face_group_t*)calloc((size_t)count, sizeof *patch->groups);
	patch->faces  = (opt_face_t*)calloc((size_t)count, sizeof *patch->faces);
	if (!patch->meshes || !patch->lods || !patch->groups || !patch->faces) {
		opt_gltf_faceless_patch_free(patch);
		return NULL;
	}
	memcpy(patch->meshes, opt->meshes, (size_t)opt->mesh_count * sizeof *patch->meshes);
	int32_t next = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		opt_mesh_t* m = &patch->meshes[mi];
		if (m->vertex_count <= 0 || opt_mesh_lod0_face_count(m) != 0)
			continue;
		const int32_t     last  = m->vertex_count > 1 ? 1 : 0;
		opt_face_t*       face  = &patch->faces[next];
		opt_face_group_t* group = &patch->groups[next];
		opt_lod_t*        lod   = &patch->lods[next];
		for (int k = 0; k < 4; ++k) {
			face->verts[k]   = k == 0 ? 0 : k < 3 ? last : -1;
			face->edges[k]   = -1;
			face->uvs[k]     = -1;
			face->normals[k] = -1;
		}
		face->face_normal    = (opt_vec3_t) { 0.0f, 0.0f, 1.0f };
		group->texture_index = -1;
		group->state_count   = 1;
		group->face_count    = 1;
		group->faces         = face;
		lod->group_count     = 1;
		lod->groups          = group;
		m->lod_count         = 1;
		m->lods              = lod;
		opt_gltf_log(options, true, "mesh %d has %d vertices and no LOD 0 faces; converted without faces",
					 (int)mi, (int)m->vertex_count);
		++next;
	}
	patch->file.meshes = patch->meshes;
	return &patch->file;
}
