/* AeronSceneMesh_Pick: where a ray first meets a mesh, for the buttons of
 * its materials. */

#include <math.h>
#include <string.h>

#include "aeron/collision/face_bvh.h"
#include "aeron/scene/mesh.h"
#include "aeron/scene/scene3d.h"

static const float kIdentityRow[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f },
										  { 0.0f, 1.0f, 0.0f, 0.0f },
										  { 0.0f, 0.0f, 1.0f, 0.0f } };

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross3(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Brings the ray into the frame that a mesh table row moves: the row's
 * triangles can then be tested at rest. The direction isn't normalised, so a
 * distance along the ray is the same in both frames. Returns 0 for a row
 * that can't be inverted. */
static int ray_to_row(const float row[3][4], const float origin[3], const float direction[3],
					  float out_origin[3], float out_direction[3]) {
	float inverse[3][3];
	cross3(row[1], row[2], inverse[0]);
	cross3(row[2], row[0], inverse[1]);
	cross3(row[0], row[1], inverse[2]);
	const float determinant = dot3(row[0], inverse[0]);
	if (!(fabsf(determinant) > 1e-12f))
		return 0;
	const float moved[3] = { origin[0] - row[0][3], origin[1] - row[1][3], origin[2] - row[2][3] };
	for (int axis = 0; axis < 3; axis++) {
		/* The inverse's rows are the cross products' components. */
		const float column[3] = { inverse[0][axis], inverse[1][axis], inverse[2][axis] };
		out_origin[axis]      = dot3(column, moved) / determinant;
		out_direction[axis]   = dot3(column, direction) / determinant;
	}
	return 1;
}

/* The slabs that hold the ray's segment, for the tree of boxes. */
static void ray_slabs(const float origin[3], const float direction[3], float max_distance,
					  AeronFaceBvhSlab slabs[3]) {
	const float length = sqrtf(dot3(direction, direction));
	float       along[3], side[3], up[3];
	for (int axis = 0; axis < 3; axis++)
		along[axis] = direction[axis] / length;
	/* Any axis that isn't near the ray gives the two directions across it. */
	const float helper[3] = { fabsf(along[0]) < 0.6f ? 1.0f : 0.0f, fabsf(along[0]) < 0.6f ? 0.0f : 1.0f,
							  0.0f };
	cross3(along, helper, side);
	const float side_length = sqrtf(dot3(side, side));
	for (int axis = 0; axis < 3; axis++)
		side[axis] /= side_length;
	cross3(along, side, up);
	memcpy(slabs[0].normal, side, sizeof side);
	slabs[0].min = slabs[0].max = dot3(origin, side);
	memcpy(slabs[1].normal, up, sizeof up);
	slabs[1].min = slabs[1].max = dot3(origin, up);
	memcpy(slabs[2].normal, along, sizeof along);
	slabs[2].min = dot3(origin, along);
	slabs[2].max = slabs[2].min + max_distance * length;
}

/* Tests the ray against one triangle (Möller-Trumbore), both faces. Returns
 * the distance and the weights of the second and third corners. */
static int ray_triangle(const float origin[3], const float direction[3], const float a[3], const float b[3],
						const float c[3], float* distance, float* u, float* v) {
	const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	float       p[3], q[3];
	cross3(direction, ac, p);
	const float determinant = dot3(ab, p);
	if (fabsf(determinant) < 1e-20f)
		return 0;
	const float from_a[3] = { origin[0] - a[0], origin[1] - a[1], origin[2] - a[2] };
	const float weight_b  = dot3(from_a, p) / determinant;
	if (weight_b < 0.0f || weight_b > 1.0f)
		return 0;
	cross3(from_a, ab, q);
	const float weight_c = dot3(direction, q) / determinant;
	if (weight_c < 0.0f || weight_b + weight_c > 1.0f)
		return 0;
	*distance = dot3(ac, q) / determinant;
	*u        = weight_b;
	*v        = weight_c;
	return 1;
}

int AeronSceneMesh_Pick(const AeronSceneMesh* mesh, const AeronSceneMeshTable* table, uint32_t variant,
						uint32_t hidden_tag_groups, const float origin[3], const float direction[3],
						float max_distance, AeronSceneMeshPick* out) {
	/* One scratch for the tree's queries: the picking runs on one thread. */
	static AeronFaceBvhScratch scratch;
	if (!mesh || !out || !mesh->button_count || !mesh->cpu_vertices || !mesh->cpu_indices || !mesh->cpu_uvs ||
		!(dot3(direction, direction) > 0.0f) || !(max_distance > 0.0f) || mesh->row_count == 0)
		return 0;
	const uint32_t triangle_count = mesh->index_count / 3u;
	const uint32_t rows =
		mesh->row_count < AERON_MAX_MESH_COMPONENTS ? mesh->row_count : AERON_MAX_MESH_COMPONENTS;
	const uint32_t table_rows =
		table && table->component_count && table->component_count < AERON_MAX_MESH_COMPONENTS
			? table->component_count
			: AERON_MAX_MESH_COMPONENTS;
	/* An index past the last variant index uses the last. */
	const uint32_t  variant_rows  = mesh->variant_rows ? mesh->variant_rows : 1u;
	const uint32_t  variant_index = variant < variant_rows ? variant : variant_rows - 1u;
	const uint32_t* materials     = mesh->cpu_prim_materials + (size_t)variant_index * mesh->total_prim_count;
	const int       use_tree      = mesh->render_bvh && mesh->render_bvh->face_count == triangle_count;

	/* The rows that share a transform are tested together: `group` gives each
	 * row the first row with its transform. Most rows have the identity. */
	uint16_t group[AERON_MAX_MESH_COMPONENTS];
	uint8_t  hidden[AERON_MAX_MESH_COMPONENTS];
	for (uint32_t row = 0; row < rows; row++) {
		const uint32_t table_row    = row < table_rows ? row : table_rows - 1u;
		hidden[row]                 = table && table->visibility_packed[table_row >> 2][table_row & 3] < 0.5f;
		group[row]                  = (uint16_t)row;
		const float (*transform)[4] = table ? table->rows[table_row] : kIdentityRow;
		for (uint32_t earlier = 0; earlier < row; earlier++) {
			const uint32_t earlier_row = earlier < table_rows ? earlier : table_rows - 1u;
			const float (*other)[4]    = table ? table->rows[earlier_row] : kIdentityRow;
			if (group[earlier] == earlier && memcmp(transform, other, sizeof kIdentityRow) == 0) {
				group[row] = (uint16_t)earlier;
				break;
			}
		}
	}

	float    best          = max_distance;
	uint32_t best_triangle = UINT32_MAX;
	float    best_u = 0.0f, best_v = 0.0f;
	for (uint32_t leader = 0; leader < rows; leader++) {
		if (group[leader] != leader)
			continue;
		const uint32_t table_row    = leader < table_rows ? leader : table_rows - 1u;
		const float (*transform)[4] = table ? table->rows[table_row] : kIdentityRow;
		float local_origin[3], local_direction[3];
		if (!ray_to_row(transform, origin, direction, local_origin, local_direction))
			continue;
		const uint32_t* candidates      = NULL;
		uint32_t        candidate_count = triangle_count;
		if (use_tree) {
			AeronFaceBvhSlab slabs[3];
			ray_slabs(local_origin, local_direction, best, slabs);
			const uint32_t count = AeronFaceBvh_CollectSlabs(mesh->render_bvh, slabs, 3, &scratch);
			if (count != UINT32_MAX) {
				candidates      = scratch.indices;
				candidate_count = count;
			}
		}
		for (uint32_t c = 0; c < candidate_count; c++) {
			const uint32_t                 triangle = candidates ? candidates[c] : c;
			const uint32_t*                corner   = &mesh->cpu_indices[3u * triangle];
			const AeronSceneMeshCpuVertex* a        = &mesh->cpu_vertices[corner[0]];
			const int                      row      = (int)floorf(a->component + 0.5f);
			if (row < 0 || row >= (int)rows || group[row] != leader || hidden[row])
				continue;
			const uint32_t primitive = mesh->cpu_prims[corner[0]];
			const uint32_t material =
				primitive < mesh->total_prim_count ? materials[primitive] : AERON_GLTF_NO_MATERIAL;
			if (material >= mesh->material_count || mesh->cpu_materials[material].see_through ||
				(mesh->cpu_materials[material].tag_groups & hidden_tag_groups))
				continue;
			float distance, u, v;
			if (ray_triangle(local_origin, local_direction, a->pos, mesh->cpu_vertices[corner[1]].pos,
							 mesh->cpu_vertices[corner[2]].pos, &distance, &u, &v) &&
				distance >= 0.0f && distance < best) {
				best          = distance;
				best_triangle = triangle;
				best_u        = u;
				best_v        = v;
			}
		}
	}
	if (best_triangle == UINT32_MAX)
		return 0;

	const uint32_t* corner    = &mesh->cpu_indices[3u * best_triangle];
	const uint32_t  primitive = mesh->cpu_prims[corner[0]];
	memset(out, 0, sizeof *out);
	out->distance = best;
	out->material = materials[primitive];
	out->button   = -1;
	for (int axis = 0; axis < 3; axis++)
		out->point[axis] = origin[axis] + direction[axis] * best;
	for (int axis = 0; axis < 2; axis++) {
		const float value = (1.0f - best_u - best_v) * mesh->cpu_uvs[corner[0]][axis] +
							best_u * mesh->cpu_uvs[corner[1]][axis] + best_v * mesh->cpu_uvs[corner[2]][axis];
		out->uv[axis]     = value - floorf(value);
	}
	const AeronSceneMeshPickMaterial* material = &mesh->cpu_materials[out->material];
	out->has_buttons                           = material->button_count > 0;
	for (uint32_t b = 0; b < material->button_count; b++) {
		const AeronGltfButton* button = &mesh->buttons[material->button_first + b];
		if (out->uv[0] >= button->rect[0] && out->uv[0] <= button->rect[2] && out->uv[1] >= button->rect[1] &&
			out->uv[1] <= button->rect[3]) {
			out->button = (int)(material->button_first + b);
			break;
		}
	}
	return 1;
}
