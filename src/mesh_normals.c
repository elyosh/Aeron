#include "aeron/mesh_normals.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NormalVec3 {
	float x, y, z;
} NormalVec3;

typedef struct NormalFace {
	uint32_t vertices[3];
	NormalVec3 normal;
	float corner_angles[3];
} NormalFace;

typedef struct NormalIncident {
	uint32_t face;
	uint8_t corner;
} NormalIncident;

typedef struct NormalContext {
	const AeronMeshNormalsInput *input;
	NormalFace *faces;
	uint32_t *incident_offsets;
	NormalIncident *incidents;
	uint32_t *visit_marks;
	uint32_t *stack;
	uint32_t visit_token;
} NormalContext;

static bool normals_error(AeronMeshNormalsError *error, int code,
						  const char *format, ...) {
	if (error) {
		va_list args;
		error->code = code;
		va_start(args, format);
		vsnprintf(error->message, sizeof error->message, format, args);
		va_end(args);
	}
	return false;
}

static NormalVec3 normal_normalize(NormalVec3 value) {
	const float length = sqrtf(value.x * value.x + value.y * value.y +
							 value.z * value.z);
	if (length > 1e-9f) {
		value.x /= length;
		value.y /= length;
		value.z /= length;
	}
	return value;
}

static NormalVec3 normal_stable_face_normal(NormalVec3 value) {
	return value.x * value.x + value.y * value.y + value.z * value.z > 0.0f
			? value : (NormalVec3){0.0f, 0.0f, 1.0f};
}

static float normal_dot(NormalVec3 lhs, NormalVec3 rhs) {
	return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

static const float *normal_position(const NormalContext *context,
									uint32_t index) {
	return (const float *)((const uint8_t *)context->input->positions +
						   (size_t)index * context->input->position_stride);
}

static NormalVec3 normal_face_normal(const NormalContext *context,
									 const uint32_t vertices[3]) {
	const float *a = normal_position(context, vertices[0]);
	const float *b = normal_position(context, vertices[1]);
	const float *c = normal_position(context, vertices[2]);
	const double ux = (double)b[0] - a[0];
	const double uy = (double)b[1] - a[1];
	const double uz = (double)b[2] - a[2];
	const double vx = (double)c[0] - a[0];
	const double vy = (double)c[1] - a[1];
	const double vz = (double)c[2] - a[2];
	return normal_normalize((NormalVec3){
			(float)(uy * vz - uz * vy),
			(float)(uz * vx - ux * vz),
			(float)(ux * vy - uy * vx),
	});
}

static float normal_corner_angle(const NormalContext *context,
								 const uint32_t vertices[3], uint32_t corner) {
	const float *center = normal_position(context, vertices[corner]);
	const float *previous = normal_position(context, vertices[(corner + 2) % 3]);
	const float *next = normal_position(context, vertices[(corner + 1) % 3]);
	const double px = (double)previous[0] - center[0];
	const double py = (double)previous[1] - center[1];
	const double pz = (double)previous[2] - center[2];
	const double nx = (double)next[0] - center[0];
	const double ny = (double)next[1] - center[1];
	const double nz = (double)next[2] - center[2];
	const double previous_length = sqrt(px * px + py * py + pz * pz);
	const double next_length = sqrt(nx * nx + ny * ny + nz * nz);
	double cosine;
	if (previous_length <= 1e-12 || next_length <= 1e-12)
		return 0.0f;
	cosine = (px * nx + py * ny + pz * nz) /
			 (previous_length * next_length);
	if (cosine < -1.0) cosine = -1.0;
	if (cosine > 1.0) cosine = 1.0;
	return (float)acos(cosine);
}

static void normal_context_release(NormalContext *context) {
	free(context->faces);
	free(context->incident_offsets);
	free(context->incidents);
	free(context->visit_marks);
	free(context->stack);
	memset(context, 0, sizeof *context);
}

static bool normal_context_init(NormalContext *context,
								const AeronMeshNormalsInput *input,
								AeronMeshNormalsError *error) {
	uint32_t *cursor = NULL;
	size_t incident_count = 0;
	memset(context, 0, sizeof *context);
	context->input = input;
	context->faces = calloc(input->triangle_count, sizeof *context->faces);
	context->incident_offsets = calloc((size_t)input->position_count + 1,
										  sizeof *context->incident_offsets);
	context->visit_marks = calloc(input->triangle_count,
								 sizeof *context->visit_marks);
	context->stack = malloc((size_t)input->triangle_count * sizeof *context->stack);
	if (!context->faces || !context->incident_offsets ||
		!context->visit_marks || !context->stack)
		goto allocation_failed;

	for (uint32_t face_index = 0; face_index < input->triangle_count;
		 ++face_index) {
		NormalFace *face = &context->faces[face_index];
		for (uint32_t corner = 0; corner < 3; ++corner) {
			const uint32_t vertex =
					input->triangle_position_indices[(size_t)face_index * 3 + corner];
			if (vertex >= input->position_count) {
				normal_context_release(context);
				return normals_error(error, 2,
						"triangle %u corner %u has position index %u, count is %u",
						face_index, corner, vertex, input->position_count);
			}
			face->vertices[corner] = vertex;
			bool duplicate = false;
			for (uint32_t earlier = 0; earlier < corner; ++earlier)
				duplicate |= face->vertices[earlier] == vertex;
			if (!duplicate)
				context->incident_offsets[vertex + 1]++;
		}
		face->normal = normal_face_normal(context, face->vertices);
		for (uint32_t corner = 0; corner < 3; ++corner)
			face->corner_angles[corner] =
					normal_corner_angle(context, face->vertices, corner);
	}
	for (uint32_t vertex = 0; vertex < input->position_count; ++vertex) {
		context->incident_offsets[vertex + 1] +=
				context->incident_offsets[vertex];
	}
	incident_count = context->incident_offsets[input->position_count];
	context->incidents = calloc(incident_count ? incident_count : 1,
									 sizeof *context->incidents);
	cursor = malloc((size_t)input->position_count * sizeof *cursor);
	if (!context->incidents || !cursor)
		goto allocation_failed;
	memcpy(cursor, context->incident_offsets,
		   (size_t)input->position_count * sizeof *cursor);
	for (uint32_t face_index = 0; face_index < input->triangle_count;
		 ++face_index) {
		const NormalFace *face = &context->faces[face_index];
		for (uint32_t corner = 0; corner < 3; ++corner) {
			const uint32_t vertex = face->vertices[corner];
			bool duplicate = false;
			for (uint32_t earlier = 0; earlier < corner; ++earlier)
				duplicate |= face->vertices[earlier] == vertex;
			if (duplicate) continue;
			const uint32_t index = cursor[vertex]++;
			context->incidents[index] =
					(NormalIncident){face_index, (uint8_t)corner};
		}
	}
	free(cursor);
	return true;

allocation_failed:
	free(cursor);
	normal_context_release(context);
	return normals_error(error, 3, "normal-builder allocation failed");
}

static bool normal_faces_share_edge(const NormalFace *lhs, uint32_t lhs_corner,
									const NormalFace *rhs, uint32_t rhs_corner) {
	const uint32_t lhs_previous = lhs->vertices[(lhs_corner + 2) % 3];
	const uint32_t lhs_next = lhs->vertices[(lhs_corner + 1) % 3];
	const uint32_t rhs_previous = rhs->vertices[(rhs_corner + 2) % 3];
	const uint32_t rhs_next = rhs->vertices[(rhs_corner + 1) % 3];
	return lhs_previous == rhs_previous || lhs_previous == rhs_next ||
		   lhs_next == rhs_previous || lhs_next == rhs_next;
}

static uint32_t normal_face_corner(const NormalFace *face, uint32_t vertex) {
	for (uint32_t corner = 0; corner < 3; ++corner)
		if (face->vertices[corner] == vertex) return corner;
	return UINT32_MAX;
}

static NormalVec3 normal_sum_fan(const NormalContext *context,
								 uint32_t begin, uint32_t end,
								 const NormalFace *reference,
								 bool restrict_hemisphere,
								 float smooth_cosine) {
	double x = 0.0, y = 0.0, z = 0.0;
	const float local_cosine = smooth_cosine > 0.0f ? smooth_cosine : 0.0f;
	for (uint32_t index = begin; index < end; ++index) {
		const NormalIncident *incident = &context->incidents[index];
		const NormalFace *face = &context->faces[incident->face];
		if (context->visit_marks[incident->face] != context->visit_token)
			continue;
		if (restrict_hemisphere &&
			normal_dot(face->normal, reference->normal) + 1e-6f < local_cosine)
			continue;
		const double weight = face->corner_angles[incident->corner];
		x += (double)face->normal.x * weight;
		y += (double)face->normal.y * weight;
		z += (double)face->normal.z * weight;
	}
	return normal_normalize((NormalVec3){(float)x, (float)y, (float)z});
}

static NormalVec3 normal_build_corner(NormalContext *context,
								  uint32_t reference_face, uint32_t vertex,
								  float smooth_cosine) {
	const NormalFace *reference = &context->faces[reference_face];
	const uint32_t begin = context->incident_offsets[vertex];
	const uint32_t end = context->incident_offsets[vertex + 1];
	uint32_t stack_count = 0;
	if (++context->visit_token == 0) {
		memset(context->visit_marks, 0,
			   (size_t)context->input->triangle_count * sizeof *context->visit_marks);
		context->visit_token = 1;
	}
	context->visit_marks[reference_face] = context->visit_token;
	context->stack[stack_count++] = reference_face;
	while (stack_count) {
		const uint32_t current_index = context->stack[--stack_count];
		const NormalFace *current = &context->faces[current_index];
		const uint32_t current_corner = normal_face_corner(current, vertex);
		if (current_corner == UINT32_MAX || normal_dot(current->normal, current->normal) == 0.0f)
			continue;
		for (uint32_t index = begin; index < end; ++index) {
			const NormalIncident *incident = &context->incidents[index];
			const NormalFace *candidate = &context->faces[incident->face];
			if (context->visit_marks[incident->face] == context->visit_token ||
				normal_dot(candidate->normal, candidate->normal) == 0.0f ||
				!normal_faces_share_edge(current, current_corner, candidate,
									 incident->corner) ||
				normal_dot(current->normal, candidate->normal) + 1e-6f <
						smooth_cosine)
				continue;
			context->visit_marks[incident->face] = context->visit_token;
			context->stack[stack_count++] = incident->face;
		}
	}
	NormalVec3 result = normal_sum_fan(context, begin, end, reference, false,
									 smooth_cosine);
	if (normal_dot(result, result) == 0.0f)
		result = normal_stable_face_normal(reference->normal);
	if (normal_dot(result, reference->normal) <= 1e-4f) {
		NormalVec3 local = normal_sum_fan(context, begin, end, reference, true,
									  smooth_cosine);
		result = normal_dot(local, reference->normal) > 1e-4f
				 ? local
				 : normal_stable_face_normal(reference->normal);
	}
	return result;
}

bool Aeron_MeshNormalsBuildCorners(const AeronMeshNormalsInput *input,
								   float *corner_normals,
								   AeronMeshNormalsError *error) {
	NormalContext context;
	if (error) memset(error, 0, sizeof *error);
	if (!input || !corner_normals || !input->positions ||
		!input->triangle_position_indices || input->position_count == 0 ||
		input->triangle_count == 0 || input->position_stride < 3 * sizeof(float) ||
		!isfinite(input->smooth_angle_degrees) ||
		input->smooth_angle_degrees < 0.0f ||
		input->smooth_angle_degrees > 180.0f)
		return normals_error(error, 1, "invalid normal-builder input");
	if ((size_t)input->position_count > SIZE_MAX / input->position_stride ||
		input->triangle_count > UINT32_MAX / 3u ||
		(size_t)input->triangle_count > SIZE_MAX / (3 * sizeof(float)) ||
		(size_t)input->triangle_count > SIZE_MAX / (3 * sizeof(uint32_t)))
		return normals_error(error, 1, "normal-builder input size overflow");
	if (!normal_context_init(&context, input, error))
		return false;
	const float smooth_cosine = cosf(input->smooth_angle_degrees *
									  3.14159265358979323846f / 180.0f);
	for (uint32_t face = 0; face < input->triangle_count; ++face) {
		for (uint32_t corner = 0; corner < 3; ++corner) {
			const NormalVec3 normal = normal_build_corner(
					&context, face, context.faces[face].vertices[corner],
					smooth_cosine);
			float *destination = &corner_normals[((size_t)face * 3 + corner) * 3];
			destination[0] = normal.x;
			destination[1] = normal.y;
			destination[2] = normal.z;
		}
	}
	normal_context_release(&context);
	return true;
}
