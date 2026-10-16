#include "aeron/asset/opt_sim_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-provided log (SDL3 via SDL_Log; standalone tools stub it). */
extern void SDL_Log(const char* fmt, ...);

static bool sim_error(char* error, size_t error_size, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s", message);
	return false;
}

static bool component_error(char* error, size_t error_size, uint32_t component, const char* message,
							const char* detail) {
	if (error && error_size)
		snprintf(error, error_size, "component %u: %s%s%s", component, message, detail ? ": " : "",
				 detail ? detail : "");
	return false;
}

/* ===== Encodings ===================================================== */

static float to_opt_units(float metres) { return (float)((double)metres * AERON_OPT_SIM_UNITS_PER_METER); }

static void vec_to_opt_units(const AeronFlightVec3* in, float out[3]) {
	out[0] = to_opt_units(in->x);
	out[1] = to_opt_units(in->y);
	out[2] = to_opt_units(in->z);
}

static float to_q15(float unit) {
	double scaled = round((double)unit * 32768.0);
	if (scaled > 32767.0)
		scaled = 32767.0;
	if (scaled < -32767.0)
		scaled = -32767.0;
	return (float)scaled;
}

static void vec_to_q15(const AeronFlightVec3* in, float out[3]) {
	out[0] = to_q15(in->x);
	out[1] = to_q15(in->y);
	out[2] = to_q15(in->z);
}

static uint32_t color_channel(float value) {
	const double scaled = round((double)value * 255.0);
	return scaled <= 0.0 ? 0u : scaled >= 255.0 ? 255u : (uint32_t)scaled;
}

/* rgba floats to 0xAARRGGBB, the inverse of the OPT import's unpacking. */
static uint32_t pack_color(const float rgba[4]) {
	return color_channel(rgba[3]) << 24 | color_channel(rgba[0]) << 16 | color_channel(rgba[1]) << 8 |
		   color_channel(rgba[2]);
}

static void copy_vec(const AeronFlightVec3* in, float out[3]) {
	out[0] = in->x;
	out[1] = in->y;
	out[2] = in->z;
}

/* ===== Deduplication and sampling ==================================== */

static uint32_t float_bits(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static uint32_t position_hash(const AeronFlightVec3* position) {
	uint32_t hash = float_bits(position->x) * 0x9E3779B1u;
	hash ^= float_bits(position->y) * 0x85EBCA77u;
	hash ^= float_bits(position->z) * 0xC2B2AE3Du;
	return hash ^ (hash >> 15);
}

static bool same_position(const AeronFlightVec3* a, const AeronFlightVec3* b) {
	return float_bits(a->x) == float_bits(b->x) && float_bits(a->y) == float_bits(b->y) &&
		   float_bits(a->z) == float_bits(b->z);
}

uint32_t Aeron_FlightTopologyDeduplicate(const AeronFlightTopology* topology, uint32_t* remap, uint32_t* unique) {
	if (!topology || !topology->position_count || !remap || !unique || topology->position_count > UINT32_MAX / 4)
		return 0;
	uint32_t capacity = 16;
	while (capacity < topology->position_count * 2)
		capacity *= 2;
	uint32_t* table = malloc((size_t)capacity * sizeof *table);
	if (!table)
		return 0;
	memset(table, 0xff, (size_t)capacity * sizeof *table);
	uint32_t unique_count = 0;
	for (uint32_t index = 0; index < topology->position_count; ++index) {
		const AeronFlightVec3* position = &topology->positions[index];
		uint32_t               slot     = position_hash(position) & (capacity - 1);
		for (;;) {
			const uint32_t entry = table[slot];
			if (entry == UINT32_MAX) {
				table[slot]            = unique_count;
				unique[unique_count]   = index;
				remap[index]           = unique_count++;
				break;
			}
			if (same_position(&topology->positions[unique[entry]], position)) {
				remap[index] = entry;
				break;
			}
			slot = (slot + 1) & (capacity - 1);
		}
	}
	free(table);
	return unique_count;
}

static bool lexicographically_less(const AeronFlightVec3* a, const AeronFlightVec3* b) {
	if (a->x != b->x)
		return a->x < b->x;
	if (a->y != b->y)
		return a->y < b->y;
	return a->z < b->z;
}

uint32_t Aeron_FlightTopologySample(const AeronFlightTopology* topology, const uint32_t* unique,
									uint32_t unique_count, uint32_t count, uint32_t* sample) {
	if (!topology || !unique || !sample)
		return UINT32_MAX;
	if (count > unique_count)
		count = unique_count;
	if (count == 0)
		return 0;
	double* nearest = malloc((size_t)unique_count * sizeof *nearest);
	if (!nearest)
		return UINT32_MAX;
	uint32_t seed = 0;
	for (uint32_t index = 1; index < unique_count; ++index)
		if (lexicographically_less(&topology->positions[unique[index]], &topology->positions[unique[seed]]))
			seed = index;
	sample[0] = seed;
	for (uint32_t index = 0; index < unique_count; ++index)
		nearest[index] = INFINITY;
	for (uint32_t picked = 1; picked <= count; ++picked) {
		const AeronFlightVec3* last = &topology->positions[unique[sample[picked - 1]]];
		uint32_t               best = UINT32_MAX;
		for (uint32_t index = 0; index < unique_count; ++index) {
			const AeronFlightVec3* position = &topology->positions[unique[index]];
			const double           dx       = (double)position->x - (double)last->x;
			const double           dy       = (double)position->y - (double)last->y;
			const double           dz       = (double)position->z - (double)last->z;
			const double           distance = dx * dx + dy * dy + dz * dz;
			if (distance < nearest[index])
				nearest[index] = distance;
			if (picked < count && (best == UINT32_MAX || nearest[index] > nearest[best]))
				best = index;
		}
		if (picked < count)
			sample[picked] = best;
	}
	free(nearest);
	return count;
}

/* ===== Mesh conversion =============================================== */

static void free_mesh(AeronOptSimMesh* mesh) {
	free(mesh->vertices);
	free(mesh->normals);
	free(mesh->faces);
	free(mesh->hardpoints);
	free(mesh->glows);
	free(mesh->authored_order);
	AeronFaceBvh_Free(mesh->bvh);
	memset(mesh, 0, sizeof *mesh);
}

static AeronFaceBvhGeometry topology_geometry(const AeronFlightTopology* topology) {
	return (AeronFaceBvhGeometry) {
		.positions       = &topology->positions[0].x,
		.position_stride = sizeof *topology->positions,
		.position_count  = topology->position_count,
		.triangles       = topology->faces[0].indices,
		.triangle_stride = sizeof *topology->faces,
		.triangle_count  = topology->face_count,
		.normals         = &topology->faces[0].normal.x,
		.normal_stride   = sizeof *topology->faces,
	};
}

/* Loads the cooked BVH and sample, or builds them for an uncooked component.
 * A component without faces has no BVH. */
static bool mesh_bvh_and_sample(const AeronFlightComponent* component, uint32_t component_index,
								uint32_t unique_count, const uint32_t* unique, uint32_t sample_count,
								AeronOptSimMesh* mesh, uint32_t** sample, char* error, size_t error_size) {
	*sample = NULL;
	if (!component->topology.face_count) {
		/* No collision faces, so no BVH; cooked data has nothing to cover. */
		if (component->has_simulation_data)
			return component_error(error, error_size, component_index,
								   "cooked simulation data on a component without faces", NULL);
	} else if (component->has_simulation_data) {
		const AeronFaceBvhGeometry geometry     = topology_geometry(&component->topology);
		char                       detail[160]  = { 0 };
		uint32_t                   cooked_count = 0;
		if (!AeronFaceBvh_LoadCooked(component->simulation_data, component->simulation_data_size,
									 component->simulation_topology_hash, &geometry, unique_count, &mesh->bvh,
									 sample, &cooked_count, detail, sizeof detail))
			return component_error(error, error_size, component_index, "invalid cooked simulation data",
								   detail);
		mesh->bvh_cooked = true;
		return cooked_count >= sample_count ||
			   component_error(error, error_size, component_index, "cooked vertex sample is too short", NULL);
	} else {
		const AeronFaceBvhGeometry geometry = topology_geometry(&component->topology);
		SDL_Log("[opt_sim] component %u has no cooked simulation data; building its BVH at load",
				component_index);
		mesh->bvh = AeronFaceBvh_Build(&geometry);
		if (!mesh->bvh)
			return component_error(error, error_size, component_index, "BVH build failed", NULL);
	}
	if (!sample_count)
		return true;
	*sample = malloc((size_t)sample_count * sizeof **sample);
	return (*sample && Aeron_FlightTopologySample(&component->topology, unique, unique_count, sample_count, *sample) ==
						   sample_count) ||
		   component_error(error, error_size, component_index, "vertex sampling failed", NULL);
}

static bool build_mesh(const AeronFlightModel* model, uint32_t component_index, const AeronOptSimOptions* options,
					   AeronOptSimMesh* mesh, char* error, size_t error_size) {
	const AeronFlightComponent* component = &model->components[component_index];
	const AeronFlightTopology*  topology  = &component->topology;
	const uint32_t              positions = topology->position_count;
	bool                        ok        = false;
	uint32_t*                   remap     = malloc((size_t)positions * sizeof *remap);
	uint32_t*                   unique    = malloc((size_t)positions * sizeof *unique);
	uint32_t*                   final     = NULL;
	uint32_t*                   sample    = NULL;
	uint32_t*                   first_face = NULL;
	double*                     normal_sum = NULL;
	if (!remap || !unique) {
		component_error(error, error_size, component_index, "out of memory", NULL);
		goto done;
	}
	const uint32_t unique_count = Aeron_FlightTopologyDeduplicate(topology, remap, unique);
	if (!unique_count) {
		component_error(error, error_size, component_index, "position deduplication failed", NULL);
		goto done;
	}
	const uint32_t sample_count =
		options->vertex_sample_count < unique_count ? options->vertex_sample_count : unique_count;
	if (!mesh_bvh_and_sample(component, component_index, unique_count, unique, sample_count, mesh, &sample, error,
							 error_size))
		goto done;
	AeronFaceBvh_ScaleBounds(mesh->bvh, AERON_OPT_SIM_UNITS_PER_METER);

	/* Final vertex order: the sample in selection order, then the rest in
	 * first-occurrence order. */
	final = malloc((size_t)unique_count * sizeof *final);
	if (!final)
		goto oom;
	memset(final, 0xff, (size_t)unique_count * sizeof *final);
	uint32_t next = 0;
	for (uint32_t index = 0; index < sample_count; ++index)
		final[sample[index]] = next++;
	for (uint32_t index = 0; index < unique_count; ++index)
		if (final[index] == UINT32_MAX)
			final[index] = next++;

	mesh->vertex_count = unique_count;
	mesh->vertices     = malloc(((size_t)unique_count + 2) * sizeof *mesh->vertices);
	mesh->normals      = malloc((size_t)unique_count * sizeof *mesh->normals);
	mesh->face_count   = topology->face_count;
	mesh->faces = topology->face_count ? malloc((size_t)topology->face_count * sizeof *mesh->faces) : NULL;
	normal_sum  = calloc((size_t)unique_count * 3, sizeof *normal_sum);
	first_face  = malloc((size_t)unique_count * sizeof *first_face);
	if (!mesh->vertices || !mesh->normals || (topology->face_count && !mesh->faces) || !normal_sum || !first_face)
		goto oom;
	memset(first_face, 0xff, (size_t)unique_count * sizeof *first_face);
	for (uint32_t index = 0; index < unique_count; ++index)
		vec_to_opt_units(&topology->positions[unique[index]], mesh->vertices[final[index]]);

	for (uint32_t face_index = 0; face_index < topology->face_count; ++face_index) {
		const AeronFlightFace* face   = &topology->faces[face_index];
		AeronOptFace*          out    = &mesh->faces[face_index];
		const AeronFlightVec3* n      = &face->normal;
		const double           length = sqrt((double)n->x * n->x + (double)n->y * n->y + (double)n->z * n->z);
		if (!isfinite(length) || fabs(1.0 - length) > 1.0e-3) {
			component_error(error, error_size, component_index, "triangle normal is not unit length", NULL);
			goto done;
		}
		copy_vec(n, out->normal);
		out->vertex_index[3] = -1;
		const AeronFlightVec3* a = &topology->positions[face->indices[0]];
		const AeronFlightVec3* b = &topology->positions[face->indices[1]];
		const AeronFlightVec3* c = &topology->positions[face->indices[2]];
		const double ab[3] = { (double)b->x - a->x, (double)b->y - a->y, (double)b->z - a->z };
		const double ac[3] = { (double)c->x - a->x, (double)c->y - a->y, (double)c->z - a->z };
		const double cross[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
								  ab[0] * ac[1] - ab[1] * ac[0] };
		const double area = 0.5 * sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
		for (int corner = 0; corner < 3; ++corner) {
			const uint32_t vertex       = final[remap[face->indices[corner]]];
			out->vertex_index[corner]   = (int32_t)vertex;
			normal_sum[vertex * 3 + 0] += area * n->x;
			normal_sum[vertex * 3 + 1] += area * n->y;
			normal_sum[vertex * 3 + 2] += area * n->z;
			if (first_face[vertex] == UINT32_MAX)
				first_face[vertex] = face_index;
		}
	}
	for (uint32_t vertex = 0; vertex < unique_count; ++vertex) {
		const double* sum    = &normal_sum[vertex * 3];
		const double  length = sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
		if (length > 0.0 && isfinite(length)) {
			for (int axis = 0; axis < 3; ++axis)
				mesh->normals[vertex][axis] = (float)(sum[axis] / length);
		} else if (first_face[vertex] != UINT32_MAX) {
			copy_vec(&topology->faces[first_face[vertex]].normal, mesh->normals[vertex]);
		} else {
			/* A position no triangle references. */
			mesh->normals[vertex][0] = 0.0f;
			mesh->normals[vertex][1] = 0.0f;
			mesh->normals[vertex][2] = 1.0f;
		}
	}

	AeronOptMeshDescriptor* descriptor = &mesh->descriptor;
	descriptor->mesh_type              = component->mesh_type;
	descriptor->explosion_type         = (int32_t)component->explosion_flags;
	vec_to_opt_units(&component->descriptor_span, descriptor->span);
	vec_to_opt_units(&component->descriptor_center, descriptor->center);
	vec_to_opt_units(&component->descriptor_bounds.min, descriptor->box_min);
	vec_to_opt_units(&component->descriptor_bounds.max, descriptor->box_max);
	descriptor->target_id = component->target_id;
	vec_to_opt_units(&component->target, descriptor->target);

	float* box_min = mesh->vertices[unique_count];
	float* box_max = mesh->vertices[unique_count + 1];
	if (options->trailing_box == AERON_OPT_TRAILING_BOX_DESCRIPTOR) {
		memcpy(box_min, descriptor->box_min, sizeof descriptor->box_min);
		memcpy(box_max, descriptor->box_max, sizeof descriptor->box_max);
	} else {
		for (uint32_t vertex = 0; vertex < unique_count; ++vertex) {
			for (int axis = 0; axis < 3; ++axis) {
				if (vertex == 0 || mesh->vertices[vertex][axis] < box_min[axis])
					box_min[axis] = mesh->vertices[vertex][axis];
				if (vertex == 0 || mesh->vertices[vertex][axis] > box_max[axis])
					box_max[axis] = mesh->vertices[vertex][axis];
			}
		}
	}

	AeronOptRotScale* rot_scale = &mesh->rot_scale;
	if (component->has_rotation) {
		vec_to_opt_units(&component->rotation.pivot, rot_scale->pivot);
		vec_to_q15(&component->rotation.rotation_axis, rot_scale->rotation_axis);
		vec_to_q15(&component->rotation.direction_axis, rot_scale->direction_axis);
		vec_to_q15(&component->rotation.up_axis, rot_scale->up_axis);
	} else {
		/* The identity frame of static stock meshes. */
		memcpy(rot_scale->pivot, descriptor->center, sizeof descriptor->center);
		const float rotation[3] = { 0.0f, 0.0f, 32767.0f }, direction[3] = { 0.0f, 32767.0f, 0.0f },
					up[3] = { 32767.0f, 0.0f, 0.0f };
		memcpy(rot_scale->rotation_axis, rotation, sizeof rotation);
		memcpy(rot_scale->direction_axis, direction, sizeof direction);
		memcpy(rot_scale->up_axis, up, sizeof up);
	}

	if (component->hardpoint_count) {
		mesh->hardpoints = malloc((size_t)component->hardpoint_count * sizeof *mesh->hardpoints);
		if (!mesh->hardpoints)
			goto oom;
		for (uint32_t index = 0; index < component->hardpoint_count; ++index) {
			mesh->hardpoints[index].type = component->hardpoints[index].type;
			vec_to_opt_units(&component->hardpoints[index].position, mesh->hardpoints[index].position);
		}
		mesh->hardpoint_count = component->hardpoint_count;
	}

	if (component->engine_glow_count) {
		mesh->glows = malloc((size_t)component->engine_glow_count * sizeof *mesh->glows);
		if (!mesh->glows)
			goto oom;
		for (uint32_t index = 0; index < component->engine_glow_count; ++index) {
			const AeronFlightEngineGlow* glow = &model->engine_glows[component->first_engine_glow + index];
			AeronOptEngineGlow*          out  = &mesh->glows[index];
			out->is_disabled                  = glow->enabled ? 0 : 1;
			out->core_color                   = pack_color(glow->core_rgba);
			out->outer_color                  = pack_color(glow->outer_rgba);
			vec_to_opt_units(&glow->dimensions, out->dimensions);
			vec_to_opt_units(&glow->position, out->position);
			copy_vec(&glow->look, out->look);
			copy_vec(&glow->up, out->up);
			copy_vec(&glow->right, out->right);
		}
		mesh->glow_count = component->engine_glow_count;
	}
	ok = true;
	goto done;

oom:
	component_error(error, error_size, component_index, "out of memory", NULL);
done:
	free(remap);
	free(unique);
	free(final);
	free(sample);
	free(first_face);
	free(normal_sum);
	return ok;
}

bool Aeron_OptSimModelBuild(const AeronFlightModel* model, const AeronOptSimOptions* options,
							AeronOptSimModel* out, char* error, size_t error_size) {
	if (!out)
		return false;
	memset(out, 0, sizeof *out);
	if (!model || !options || !model->component_count || options->vertex_sample_count > AERON_FACE_BVH_SAMPLE_MAX ||
		(options->trailing_box != AERON_OPT_TRAILING_BOX_DESCRIPTOR &&
		 options->trailing_box != AERON_OPT_TRAILING_BOX_GEOMETRY))
		return sim_error(error, error_size, "invalid OPT simulation model arguments");
	out->meshes = calloc(model->component_count, sizeof *out->meshes);
	if (!out->meshes)
		return sim_error(error, error_size, "out of memory");
	out->mesh_count = model->component_count;
	for (uint32_t index = 0; index < model->component_count; ++index) {
		if (!build_mesh(model, index, options, &out->meshes[index], error, error_size)) {
			Aeron_OptSimModelFree(out);
			return false;
		}
	}
	return true;
}

bool Aeron_OptSimModelReorderFacesToLeafOrder(AeronOptSimModel* model) {
	if (!model)
		return false;
	for (uint32_t index = 0; index < model->mesh_count; ++index) {
		AeronOptSimMesh* mesh = &model->meshes[index];
		if (!mesh->bvh || mesh->authored_order)
			continue;
		uint32_t* authored_order = malloc((size_t)mesh->face_count * sizeof *authored_order);
		if (!authored_order ||
			!AeronFaceBvh_ReorderToLeafOrder(mesh->bvh, mesh->faces, sizeof *mesh->faces, authored_order)) {
			free(authored_order);
			return false;
		}
		mesh->authored_order = authored_order;
	}
	return true;
}

void Aeron_OptSimModelTrimToFaces(AeronOptSimModel* model) {
	if (!model)
		return;
	for (uint32_t index = 0; index < model->mesh_count; ++index) {
		AeronOptSimMesh* mesh = &model->meshes[index];
		free(mesh->vertices);
		free(mesh->normals);
		free(mesh->hardpoints);
		free(mesh->glows);
		mesh->vertices        = NULL;
		mesh->normals         = NULL;
		mesh->hardpoints      = NULL;
		mesh->glows           = NULL;
		mesh->hardpoint_count = 0;
		mesh->glow_count      = 0;
	}
}

void Aeron_OptSimModelFree(AeronOptSimModel* model) {
	if (!model)
		return;
	for (uint32_t index = 0; index < model->mesh_count; ++index)
		free_mesh(&model->meshes[index]);
	free(model->meshes);
	memset(model, 0, sizeof *model);
}
