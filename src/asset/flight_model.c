#include "aeron/asset/flight_model.h"

#include "cJSON.h"
#include "cgltf.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void SDL_Log(const char* fmt, ...);

typedef enum FlightNodeRole {
	FLIGHT_ROLE_NONE,
	FLIGHT_ROLE_MODEL,
	FLIGHT_ROLE_COMPONENT,
	FLIGHT_ROLE_HARDPOINT,
	FLIGHT_ROLE_ENGINE_GLOW,
	FLIGHT_ROLE_INVALID,
} FlightNodeRole;

static const char* flight_extension(const cgltf_node* node) {
	if (!node)
		return NULL;
	for (cgltf_size index = 0; index < node->extensions_count; ++index) {
		const cgltf_extension* extension = &node->extensions[index];
		if (extension->name && extension->data && strcmp(extension->name, AERON_FLIGHT_MODEL_EXTENSION) == 0)
			return extension->data;
	}
	return NULL;
}

static cJSON* parse_flight_extension(const cgltf_node* node) {
	const char* extension = flight_extension(node);
	if (!extension)
		return NULL;
	cJSON* object = cJSON_ParseWithOpts(extension, NULL, true);
	if (!cJSON_IsObject(object)) {
		cJSON_Delete(object);
		return NULL;
	}
	return object;
}

static FlightNodeRole json_role(const cJSON* object) {
	const cJSON* role = cJSON_GetObjectItemCaseSensitive(object, "role");
	if (!cJSON_IsString(role) || !role->valuestring)
		return FLIGHT_ROLE_INVALID;
	if (strcmp(role->valuestring, "model") == 0)
		return FLIGHT_ROLE_MODEL;
	if (strcmp(role->valuestring, "component") == 0)
		return FLIGHT_ROLE_COMPONENT;
	if (strcmp(role->valuestring, "hardpoint") == 0)
		return FLIGHT_ROLE_HARDPOINT;
	if (strcmp(role->valuestring, "engineGlow") == 0)
		return FLIGHT_ROLE_ENGINE_GLOW;
	return FLIGHT_ROLE_INVALID;
}

static bool json_int(const cJSON* value, int32_t* out) {
	if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < INT32_MIN ||
		value->valuedouble > INT32_MAX || trunc(value->valuedouble) != value->valuedouble)
		return false;
	*out = (int32_t)value->valuedouble;
	return true;
}

static bool json_uint(const cJSON* value, uint32_t* out) {
	if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0.0 ||
		value->valuedouble > UINT32_MAX || trunc(value->valuedouble) != value->valuedouble)
		return false;
	*out = (uint32_t)value->valuedouble;
	return true;
}

static bool json_vector(const cJSON* value, float* out, int count) {
	if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != count)
		return false;
	for (int index = 0; index < count; ++index) {
		const cJSON* item = cJSON_GetArrayItem(value, index);
		if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < -FLT_MAX ||
			item->valuedouble > FLT_MAX)
			return false;
		out[index] = (float)item->valuedouble;
	}
	return true;
}

static FlightNodeRole node_role(const cgltf_node* node) {
	if (!flight_extension(node))
		return FLIGHT_ROLE_NONE;
	cJSON* object = parse_flight_extension(node);
	if (!object)
		return FLIGHT_ROLE_INVALID;
	const FlightNodeRole role = json_role(object);
	cJSON_Delete(object);
	return role;
}

static bool finite_node_transform(const cgltf_node* node) {
	if (!node)
		return false;
	if (node->has_translation)
		for (int axis = 0; axis < 3; ++axis)
			if (!isfinite(node->translation[axis]))
				return false;
	if (node->has_rotation)
		for (int axis = 0; axis < 4; ++axis)
			if (!isfinite(node->rotation[axis]))
				return false;
	if (node->has_scale)
		for (int axis = 0; axis < 3; ++axis)
			if (!isfinite(node->scale[axis]))
				return false;
	return true;
}

static bool uniform_positive_transform(const cgltf_node* node) {
	if (!node || node->has_matrix || !finite_node_transform(node))
		return false;
	if (!node->has_scale)
		return true;
	return isfinite(node->scale[0]) && node->scale[0] > 0.0f &&
		   fabsf(node->scale[0] - node->scale[1]) <= 1.0e-6f &&
		   fabsf(node->scale[0] - node->scale[2]) <= 1.0e-6f;
}

static bool identity_rotation_scale(const cgltf_node* node) {
	if (!node)
		return false;
	if (node->has_rotation &&
		(fabsf(node->rotation[0]) > 1.0e-6f || fabsf(node->rotation[1]) > 1.0e-6f ||
		 fabsf(node->rotation[2]) > 1.0e-6f || fabsf(fabsf(node->rotation[3]) - 1.0f) > 1.0e-6f))
		return false;
	return !node->has_scale ||
		   (fabsf(node->scale[0] - 1.0f) <= 1.0e-6f && fabsf(node->scale[1] - 1.0f) <= 1.0e-6f &&
			fabsf(node->scale[2] - 1.0f) <= 1.0e-6f);
}

static void gltf_to_aeron(const float source[3], AeronFlightVec3* out) {
	out->x = -source[0];
	out->y = -source[2];
	out->z = source[1];
}

static void transform_point(const float matrix[16], const float point[3], float out[3]) {
	out[0] = matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12];
	out[1] = matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13];
	out[2] = matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14];
}

static void transform_direction(const float matrix[16], const float direction[3], AeronFlightVec3* out) {
	const float scale          = sqrtf(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
	float       transformed[3] = {
		(matrix[0] * direction[0] + matrix[4] * direction[1] + matrix[8] * direction[2]) / scale,
		(matrix[1] * direction[0] + matrix[5] * direction[1] + matrix[9] * direction[2]) / scale,
		(matrix[2] * direction[0] + matrix[6] * direction[1] + matrix[10] * direction[2]) / scale,
	};
	gltf_to_aeron(transformed, out);
}

static const cgltf_accessor* primitive_position(const cgltf_primitive* primitive) {
	for (cgltf_size index = 0; index < primitive->attributes_count; ++index) {
		const cgltf_attribute* attribute = &primitive->attributes[index];
		if (attribute->type == cgltf_attribute_type_position && attribute->index == 0)
			return attribute->data;
	}
	return NULL;
}

static void bounds_include(AeronFlightBounds* bounds, const AeronFlightVec3* point, bool* initialized) {
	if (!*initialized) {
		bounds->min = bounds->max = *point;
		*initialized              = true;
		return;
	}
	if (point->x < bounds->min.x)
		bounds->min.x = point->x;
	if (point->y < bounds->min.y)
		bounds->min.y = point->y;
	if (point->z < bounds->min.z)
		bounds->min.z = point->z;
	if (point->x > bounds->max.x)
		bounds->max.x = point->x;
	if (point->y > bounds->max.y)
		bounds->max.y = point->y;
	if (point->z > bounds->max.z)
		bounds->max.z = point->z;
}

static bool transform_descriptor_geometry(const float matrix[16], const float span[3], const float center[3],
										  const float bounds_min[3], const float bounds_max[3],
										  AeronFlightComponent* component) {
	for (int axis = 0; axis < 3; ++axis)
		if (span[axis] < 0.0f || bounds_min[axis] > bounds_max[axis])
			return false;

	float transformed_center[3];
	transform_point(matrix, center, transformed_center);
	gltf_to_aeron(transformed_center, &component->descriptor_center);

	/* Span is an axis-aligned size, so rotation contributes through absolute matrix coefficients. */
	const float transformed_span[3] = {
		fabsf(matrix[0]) * span[0] + fabsf(matrix[4]) * span[1] + fabsf(matrix[8]) * span[2],
		fabsf(matrix[1]) * span[0] + fabsf(matrix[5]) * span[1] + fabsf(matrix[9]) * span[2],
		fabsf(matrix[2]) * span[0] + fabsf(matrix[6]) * span[1] + fabsf(matrix[10]) * span[2],
	};
	component->descriptor_span = (AeronFlightVec3) {
		.x = transformed_span[0],
		.y = transformed_span[2],
		.z = transformed_span[1],
	};

	bool have_bounds = false;
	for (int corner = 0; corner < 8; ++corner) {
		const float point[3] = {
			(corner & 1) ? bounds_max[0] : bounds_min[0],
			(corner & 2) ? bounds_max[1] : bounds_min[1],
			(corner & 4) ? bounds_max[2] : bounds_min[2],
		};
		float transformed[3];
		transform_point(matrix, point, transformed);
		if (!isfinite(transformed[0]) || !isfinite(transformed[1]) || !isfinite(transformed[2]))
			return false;
		AeronFlightVec3 aeron_point;
		gltf_to_aeron(transformed, &aeron_point);
		bounds_include(&component->descriptor_bounds, &aeron_point, &have_bounds);
	}
	return isfinite(component->descriptor_center.x) && isfinite(component->descriptor_center.y) &&
		   isfinite(component->descriptor_center.z) && isfinite(component->descriptor_span.x) &&
		   isfinite(component->descriptor_span.y) && isfinite(component->descriptor_span.z) && have_bounds;
}

static bool build_component_topology(const cgltf_node* node, AeronFlightComponent* component) {
	uint32_t position_count = 0;
	uint32_t face_count     = 0;
	for (cgltf_size index = 0; index < node->mesh->primitives_count; ++index) {
		const cgltf_primitive* primitive = &node->mesh->primitives[index];
		const cgltf_accessor*  positions = primitive_position(primitive);
		const uint32_t         indices   = primitive->indices ? (uint32_t)primitive->indices->count
										   : positions        ? (uint32_t)positions->count
															  : 0;
		if (!positions || primitive->type != cgltf_primitive_type_triangles || indices == 0 ||
			indices % 3 != 0)
			return false;
		if (UINT32_MAX - position_count < positions->count || UINT32_MAX - face_count < indices / 3)
			return false;
		position_count += (uint32_t)positions->count;
		face_count += indices / 3;
	}
	if (!position_count || !face_count)
		return false;
	component->topology.positions = calloc(position_count, sizeof *component->topology.positions);
	component->topology.faces     = calloc(face_count, sizeof *component->topology.faces);
	if (!component->topology.positions || !component->topology.faces)
		return false;
	component->topology.position_count = position_count;

	float matrix[16];
	cgltf_node_transform_world(node, matrix);
	uint32_t position_offset = 0;
	uint32_t face_offset     = 0;
	bool     have_bounds     = false;
	for (cgltf_size primitive_index = 0; primitive_index < node->mesh->primitives_count; ++primitive_index) {
		const cgltf_primitive* primitive = &node->mesh->primitives[primitive_index];
		const cgltf_accessor*  positions = primitive_position(primitive);
		for (cgltf_size index = 0; index < positions->count; ++index) {
			float local[3];
			float transformed[3];
			if (!cgltf_accessor_read_float(positions, index, local, 3) || !isfinite(local[0]) ||
				!isfinite(local[1]) || !isfinite(local[2]))
				return false;
			transform_point(matrix, local, transformed);
			if (!isfinite(transformed[0]) || !isfinite(transformed[1]) || !isfinite(transformed[2]))
				return false;
			AeronFlightVec3* position = &component->topology.positions[position_offset + (uint32_t)index];
			gltf_to_aeron(transformed, position);
			bounds_include(&component->bounds, position, &have_bounds);
		}
		const uint32_t index_count =
			primitive->indices ? (uint32_t)primitive->indices->count : (uint32_t)positions->count;
		for (uint32_t index = 0; index < index_count; index += 3) {
			AeronFlightFace* face = &component->topology.faces[face_offset];
			float            gltf_points[3][3];
			for (uint32_t corner = 0; corner < 3; ++corner) {
				const cgltf_size local_index =
					primitive->indices ? cgltf_accessor_read_index(primitive->indices, index + corner)
									   : index + corner;
				if (local_index >= positions->count)
					return false;
				face->indices[corner] = position_offset + (uint32_t)local_index;
				float local[3];
				cgltf_accessor_read_float(positions, local_index, local, 3);
				transform_point(matrix, local, gltf_points[corner]);
			}
			const float ab[3] = {
				gltf_points[1][0] - gltf_points[0][0],
				gltf_points[1][1] - gltf_points[0][1],
				gltf_points[1][2] - gltf_points[0][2],
			};
			const float ac[3] = {
				gltf_points[2][0] - gltf_points[0][0],
				gltf_points[2][1] - gltf_points[0][1],
				gltf_points[2][2] - gltf_points[0][2],
			};
			float normal[3] = {
				ab[1] * ac[2] - ab[2] * ac[1],
				ab[2] * ac[0] - ab[0] * ac[2],
				ab[0] * ac[1] - ab[1] * ac[0],
			};
			const float length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			if (!isfinite(length))
				return false;
			if (length == 0.0f)
				continue;
			normal[0] /= length;
			normal[1] /= length;
			normal[2] /= length;
			gltf_to_aeron(normal, &face->normal);
			face_offset++;
		}
		position_offset += (uint32_t)positions->count;
	}
	component->topology.face_count = face_offset;
	component->span                = (AeronFlightVec3) {
		component->bounds.max.x - component->bounds.min.x,
		component->bounds.max.y - component->bounds.min.y,
		component->bounds.max.z - component->bounds.min.z,
	};
	component->center = (AeronFlightVec3) {
		(component->bounds.min.x + component->bounds.max.x) * 0.5f,
		(component->bounds.min.y + component->bounds.max.y) * 0.5f,
		(component->bounds.min.z + component->bounds.max.z) * 0.5f,
	};
	return true;
}

static bool build_component_semantics(const cgltf_node* node, AeronFlightComponent* component) {
	if (!node->mesh || node->skin || !uniform_positive_transform(node))
		return false;
	cJSON* extension = parse_flight_extension(node);
	if (!extension || json_role(extension) != FLIGHT_ROLE_COMPONENT) {
		cJSON_Delete(extension);
		return false;
	}
	int32_t      mesh_type       = 0;
	const cJSON* mesh_type_value = cJSON_GetObjectItemCaseSensitive(extension, "meshType");
	if (!json_int(mesh_type_value, &mesh_type) || mesh_type < 0 || mesh_type > 31) {
		cJSON_Delete(extension);
		return false;
	}
	component->mesh_type         = mesh_type;
	const cJSON* explosion_flags = cJSON_GetObjectItemCaseSensitive(extension, "explosionFlags");
	if (explosion_flags && !json_uint(explosion_flags, &component->explosion_flags)) {
		cJSON_Delete(extension);
		return false;
	}
	const cJSON* target_id = cJSON_GetObjectItemCaseSensitive(extension, "targetId");
	if (target_id && !json_int(target_id, &component->target_id)) {
		cJSON_Delete(extension);
		return false;
	}
	float        target[3];
	const cJSON* target_value = cJSON_GetObjectItemCaseSensitive(extension, "target");
	if ((target_value && !json_vector(target_value, target, 3)) ||
		(component->target_id != 0 && !target_value)) {
		cJSON_Delete(extension);
		return false;
	}
	float        descriptor_span[3], descriptor_center[3], descriptor_bounds_min[3], descriptor_bounds_max[3];
	const cJSON* descriptor_span_value       = cJSON_GetObjectItemCaseSensitive(extension, "span");
	const cJSON* descriptor_center_value     = cJSON_GetObjectItemCaseSensitive(extension, "center");
	const cJSON* descriptor_bounds_min_value = cJSON_GetObjectItemCaseSensitive(extension, "boundsMin");
	const cJSON* descriptor_bounds_max_value = cJSON_GetObjectItemCaseSensitive(extension, "boundsMax");
	const int    descriptor_geometry_count =
		(descriptor_span_value != NULL) + (descriptor_center_value != NULL) +
		(descriptor_bounds_min_value != NULL) + (descriptor_bounds_max_value != NULL);
	const bool has_descriptor_geometry = descriptor_geometry_count == 4;
	if ((descriptor_geometry_count != 0 && !has_descriptor_geometry) ||
		(has_descriptor_geometry && (!json_vector(descriptor_span_value, descriptor_span, 3) ||
									 !json_vector(descriptor_center_value, descriptor_center, 3) ||
									 !json_vector(descriptor_bounds_min_value, descriptor_bounds_min, 3) ||
									 !json_vector(descriptor_bounds_max_value, descriptor_bounds_max, 3)))) {
		cJSON_Delete(extension);
		return false;
	}
	float        pivot[3], rotation_axis[3], direction_axis[3], up_axis[3];
	const cJSON* rotation     = cJSON_GetObjectItemCaseSensitive(extension, "rotation");
	const bool   has_rotation = rotation != NULL;
	if (rotation &&
		(!cJSON_IsObject(rotation) ||
		 !json_vector(cJSON_GetObjectItemCaseSensitive(rotation, "pivot"), pivot, 3) ||
		 !json_vector(cJSON_GetObjectItemCaseSensitive(rotation, "rotationAxis"), rotation_axis, 3) ||
		 !json_vector(cJSON_GetObjectItemCaseSensitive(rotation, "directionAxis"), direction_axis, 3) ||
		 !json_vector(cJSON_GetObjectItemCaseSensitive(rotation, "upAxis"), up_axis, 3))) {
		cJSON_Delete(extension);
		return false;
	}
	cJSON_Delete(extension);

	float matrix[16];
	cgltf_node_transform_world(node, matrix);
	component->has_descriptor = true;
	if (has_descriptor_geometry &&
		!transform_descriptor_geometry(matrix, descriptor_span, descriptor_center, descriptor_bounds_min,
									   descriptor_bounds_max, component))
		return false;
	if (component->target_id != 0) {
		float transformed[3];
		transform_point(matrix, target, transformed);
		gltf_to_aeron(transformed, &component->target);
	}
	if (has_rotation) {
		float transformed[3];
		transform_point(matrix, pivot, transformed);
		gltf_to_aeron(transformed, &component->rotation.pivot);
		transform_direction(matrix, rotation_axis, &component->rotation.rotation_axis);
		transform_direction(matrix, direction_axis, &component->rotation.direction_axis);
		transform_direction(matrix, up_axis, &component->rotation.up_axis);
		component->has_rotation = true;
	}
	if (!build_component_topology(node, component))
		return false;
	if (!has_descriptor_geometry) {
		component->descriptor_span   = component->span;
		component->descriptor_center = component->center;
		component->descriptor_bounds = component->bounds;
	}
	return true;
}

static bool build_hardpoints(const cgltf_node* node, AeronFlightComponent* component) {
	uint32_t count = 0;
	for (cgltf_size index = 0; index < node->children_count; ++index)
		if (node_role(node->children[index]) == FLIGHT_ROLE_HARDPOINT)
			++count;
	if (!count)
		return true;
	component->hardpoints = calloc(count, sizeof *component->hardpoints);
	if (!component->hardpoints)
		return false;
	for (cgltf_size index = 0; index < node->children_count; ++index) {
		const cgltf_node* child = node->children[index];
		if (node_role(child) != FLIGHT_ROLE_HARDPOINT)
			continue;
		cJSON*  extension = parse_flight_extension(child);
		int32_t type      = 0;
		if (child->mesh || child->children_count || child->has_matrix || !finite_node_transform(child) ||
			!identity_rotation_scale(child) || !extension || json_role(extension) != FLIGHT_ROLE_HARDPOINT ||
			!json_int(cJSON_GetObjectItemCaseSensitive(extension, "type"), &type) || type < 0 || type > 39) {
			cJSON_Delete(extension);
			return false;
		}
		cJSON_Delete(extension);
		AeronFlightHardpoint* hardpoint = &component->hardpoints[component->hardpoint_count++];
		float                 matrix[16];
		float                 origin[3] = { 0.0f, 0.0f, 0.0f };
		float                 transformed[3];
		cgltf_node_transform_world(child, matrix);
		transform_point(matrix, origin, transformed);
		hardpoint->type = type;
		gltf_to_aeron(transformed, &hardpoint->position);
	}
	return true;
}

static bool append_engine_glows(const cgltf_node* node, uint32_t component_index,
								AeronFlightComponent* component, AeronFlightModel* model) {
	component->first_engine_glow = model->engine_glow_count;
	for (cgltf_size index = 0; index < node->children_count; ++index) {
		const cgltf_node* child = node->children[index];
		if (node_role(child) != FLIGHT_ROLE_ENGINE_GLOW)
			continue;
		if (child->mesh || child->children_count || child->has_matrix || !finite_node_transform(child) ||
			child->scale[0] <= 0.0f || child->scale[1] <= 0.0f || child->scale[2] == 0.0f)
			return false;
		cJSON*       extension = parse_flight_extension(child);
		float        core_rgba[4], outer_rgba[4];
		bool         enabled = true;
		const cJSON* enabled_value =
			extension ? cJSON_GetObjectItemCaseSensitive(extension, "enabled") : NULL;
		if (!extension || json_role(extension) != FLIGHT_ROLE_ENGINE_GLOW ||
			!json_vector(cJSON_GetObjectItemCaseSensitive(extension, "coreColor"), core_rgba, 4) ||
			!json_vector(cJSON_GetObjectItemCaseSensitive(extension, "outerColor"), outer_rgba, 4) ||
			(enabled_value && !cJSON_IsBool(enabled_value))) {
			cJSON_Delete(extension);
			return false;
		}
		for (int channel = 0; channel < 4; ++channel) {
			if (core_rgba[channel] < 0.0f || core_rgba[channel] > 1.0f || outer_rgba[channel] < 0.0f ||
				outer_rgba[channel] > 1.0f) {
				cJSON_Delete(extension);
				return false;
			}
		}
		if (enabled_value)
			enabled = cJSON_IsTrue(enabled_value);
		cJSON_Delete(extension);
		AeronFlightEngineGlow* grown = realloc(model->engine_glows, (size_t)(model->engine_glow_count + 1) *
																		sizeof *model->engine_glows);
		if (!grown)
			return false;
		model->engine_glows         = grown;
		AeronFlightEngineGlow* glow = &grown[model->engine_glow_count++];
		memset(glow, 0, sizeof *glow);
		float matrix[16];
		float origin[3] = { 0.0f, 0.0f, 0.0f };
		float transformed[3];
		cgltf_node_transform_world(child, matrix);
		transform_point(matrix, origin, transformed);
		gltf_to_aeron(transformed, &glow->position);
		const float      signs[3]      = { 1.0f, 1.0f, child->scale[2] < 0.0f ? -1.0f : 1.0f };
		AeronFlightVec3* axes[3]       = { &glow->right, &glow->up, &glow->look };
		float*           dimensions[3] = { &glow->dimensions.x, &glow->dimensions.y, &glow->dimensions.z };
		for (int axis = 0; axis < 3; ++axis) {
			const float column[3] = { matrix[axis * 4 + 0], matrix[axis * 4 + 1], matrix[axis * 4 + 2] };
			const float length = sqrtf(column[0] * column[0] + column[1] * column[1] + column[2] * column[2]);
			if (!(length > 0.0f))
				return false;
			const float direction[3] = {
				column[0] / (length * signs[axis]),
				column[1] / (length * signs[axis]),
				column[2] / (length * signs[axis]),
			};
			gltf_to_aeron(direction, axes[axis]);
			*dimensions[axis] = length * signs[axis];
		}
		memcpy(glow->core_rgba, core_rgba, sizeof core_rgba);
		memcpy(glow->outer_rgba, outer_rgba, sizeof outer_rgba);
		glow->enabled         = enabled;
		glow->component_index = component_index;
		++component->engine_glow_count;
	}
	return true;
}

static bool build_semantics(const cgltf_data* data, AeronFlightModel* model) {
	if (!data->scene || data->scene->nodes_count != 1 || data->animations_count || data->skins_count)
		return false;
	const cgltf_node* root = data->scene->nodes[0];
	if (node_role(root) != FLIGHT_ROLE_MODEL || root->mesh || root->skin || !uniform_positive_transform(root))
		return false;
	model->component_count = (uint32_t)root->children_count;
	if (!model->component_count)
		return false;
	model->components = calloc(model->component_count, sizeof *model->components);
	if (!model->components)
		return false;
	model->bridge_component = -1;
	bool have_model_bounds  = false;
	for (uint32_t index = 0; index < model->component_count; ++index) {
		const cgltf_node*     node      = root->children[index];
		AeronFlightComponent* component = &model->components[index];
		if (node_role(node) != FLIGHT_ROLE_COMPONENT)
			return false;
		for (cgltf_size child = 0; child < node->children_count; ++child) {
			const FlightNodeRole role = node_role(node->children[child]);
			if (role != FLIGHT_ROLE_HARDPOINT && role != FLIGHT_ROLE_ENGINE_GLOW)
				return false;
		}
		if (!build_component_semantics(node, component) || !build_hardpoints(node, component) ||
			!append_engine_glows(node, index, component, model))
			return false;
		if (component->mesh_type == 7 && model->bridge_component < 0)
			model->bridge_component = (int32_t)index;
		bounds_include(&model->bounds, &component->bounds.min, &have_model_bounds);
		bounds_include(&model->bounds, &component->bounds.max, &have_model_bounds);
	}
	model->max_extent    = model->bounds.max.x - model->bounds.min.x;
	const float extent_y = model->bounds.max.y - model->bounds.min.y;
	const float extent_z = model->bounds.max.z - model->bounds.min.z;
	if (extent_y > model->max_extent)
		model->max_extent = extent_y;
	if (extent_z > model->max_extent)
		model->max_extent = extent_z;
	for (cgltf_size index = 0; index < data->nodes_count; ++index) {
		const cgltf_node*    node = &data->nodes[index];
		const FlightNodeRole role = node_role(node);
		if (role == FLIGHT_ROLE_INVALID || (role == FLIGHT_ROLE_MODEL && node != root) ||
			(role == FLIGHT_ROLE_COMPONENT && node->parent != root) ||
			((role == FLIGHT_ROLE_HARDPOINT || role == FLIGHT_ROLE_ENGINE_GLOW) &&
			 (!node->parent || node_role(node->parent) != FLIGHT_ROLE_COMPONENT ||
			  node->parent->parent != root)))
			return false;
	}
	return true;
}

bool Aeron_FlightModelBuildData(const cgltf_data* data, const char* source_label, AeronFlightModel* out) {
	if (!data || !out)
		return false;
	memset(out, 0, sizeof *out);
	if (cgltf_validate((cgltf_data*)data) != cgltf_result_success || !build_semantics(data, out) ||
		!Aeron_GltfMeshBuildData(data, source_label, &out->render)) {
		SDL_Log("[flight_model] '%s' has an invalid %s contract",
				source_label ? source_label : "<memory glTF>", AERON_FLIGHT_MODEL_EXTENSION);
		Aeron_FlightModelFree(out);
		return false;
	}
	return true;
}

bool Aeron_FlightModelBuildMemory(const void* bytes, size_t size, const char* source_label,
								  AeronFlightModel* out) {
	if (!bytes || !size || !source_label || !source_label[0] || !out)
		return false;
	cgltf_options options = { 0 };
	cgltf_data*   data    = NULL;
	if (cgltf_parse(&options, bytes, size, &data) != cgltf_result_success)
		return false;
	for (cgltf_size index = 0; index < data->buffers_count; ++index) {
		if (!data->buffers[index].data) {
			cgltf_free(data);
			return false;
		}
	}
	const bool built = Aeron_FlightModelBuildData(data, source_label, out);
	cgltf_free(data);
	return built;
}

bool Aeron_FlightModelBuild(const char* glb_path, AeronFlightModel* out) {
	if (!glb_path || !out)
		return false;
	cgltf_options options = { 0 };
	cgltf_data*   data    = NULL;
	if (cgltf_parse_file(&options, glb_path, &data) != cgltf_result_success)
		return false;
	if (cgltf_load_buffers(&options, data, glb_path) != cgltf_result_success) {
		cgltf_free(data);
		return false;
	}
	const bool built = Aeron_FlightModelBuildData(data, glb_path, out);
	cgltf_free(data);
	return built;
}

void Aeron_FlightModelReleaseRenderData(AeronFlightModel* model) {
	if (model)
		Aeron_GltfMeshFree(&model->render);
}

void Aeron_FlightModelFree(AeronFlightModel* model) {
	if (!model)
		return;
	Aeron_GltfMeshFree(&model->render);
	for (uint32_t index = 0; index < model->component_count; ++index) {
		AeronFlightComponent* component = &model->components[index];
		free(component->topology.positions);
		free(component->topology.faces);
		free(component->hardpoints);
	}
	free(model->components);
	free(model->engine_glows);
	memset(model, 0, sizeof *model);
}
