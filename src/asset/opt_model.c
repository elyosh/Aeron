#include "aeron/asset/opt_model.h"

#include "gltf_cook.h"
#include "opt2gltf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_METERS_PER_UNIT (1600.0f / 65536.0f)
#define AERON_OPT_Q15_SCALE 32768.0f

typedef struct OptModelCookContext {
	OptGltfDocument *document;
	AeronFlightModel *model;
	const char *label;
	bool render_build_failed;
} OptModelCookContext;

static bool opt_model_error(AeronOptModelError *error, int code,
							const char *message) {
	if (error) {
		error->code = code;
		snprintf(error->message, sizeof error->message, "%s",
				 message ? message : "OPT conversion failed");
	}
	return false;
}

static bool opt_model_image_provider(void *context, const cgltf_image *image,
									 AeronGltfCookImageView *out_view) {
	OptModelCookContext *cook = context;
	OptGltfImageView image_view;
	if (!OptGltf_ImageView(cook->document, image, &image_view))
		return false;
	out_view->rgba = image_view.rgba;
	out_view->width = (int)image_view.width;
	out_view->height = (int)image_view.height;
	return true;
}

static bool opt_model_consumer(void *context, const cgltf_data *cooked_data) {
	OptModelCookContext *cook = context;
	if (!cooked_data->scene || cooked_data->scene->nodes_count != 1 ||
		cooked_data->scene->nodes[0]->children_count != cook->model->component_count ||
		!Aeron_GltfMeshBuildData(cooked_data, cook->label, &cook->model->render)) {
		cook->render_build_failed = true;
		return false;
	}
	return true;
}

static bool opt_model_arguments_valid(const char* label, const AeronOptModelBuildOptions* options,
									  AeronFlightModel* out) {
	if (!label || !label[0] || !options || !out || !isfinite(options->smooth_angle_degrees) ||
		options->smooth_angle_degrees < 0.0f || options->smooth_angle_degrees > 180.0f ||
		!isfinite(options->emissive_strength) || options->emissive_strength < 0.0f ||
		(options->alpha_override_count && !options->alpha_overrides))
		return false;
	for (size_t index = 0; index < options->alpha_override_count; ++index) {
		const AeronOptAlphaOverride* override = &options->alpha_overrides[index];
		if (!override->texture_name || !override->texture_name[0] ||
			override->alpha_mode < AERON_GLTF_ALPHA_OPAQUE || override->alpha_mode > AERON_GLTF_ALPHA_BLEND ||
			!isfinite(override->alpha_cutoff) || override->alpha_cutoff < 0.0f ||
			override->alpha_cutoff > 1.0f)
			return false;
	}
	return true;
}

static AeronFlightVec3 opt_position(const opt_vec3_t* source) {
	return (AeronFlightVec3) {
		source->x * OPT_METERS_PER_UNIT,
		source->y * OPT_METERS_PER_UNIT,
		source->z * OPT_METERS_PER_UNIT,
	};
}

static AeronFlightVec3 opt_unit_vector(const opt_vec3_t* source) {
	return (AeronFlightVec3) {
		source->x,
		source->y,
		source->z,
	};
}

static AeronFlightVec3 opt_q15_direction(const opt_vec3_t* source) {
	return (AeronFlightVec3) {
		source->x / AERON_OPT_Q15_SCALE,
		source->y / AERON_OPT_Q15_SCALE,
		source->z / AERON_OPT_Q15_SCALE,
	};
}

static void include_point(AeronFlightBounds* bounds, const AeronFlightVec3* point, bool* initialized) {
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

static uint32_t mesh_face_count(const opt_mesh_t* mesh) {
	if (!mesh || mesh->lod_count <= 0)
		return 0;
	uint32_t         count = 0;
	const opt_lod_t* lod   = &mesh->lods[0];
	for (int32_t group = 0; group < lod->group_count; ++group) {
		for (int32_t face = 0; face < lod->groups[group].face_count; ++face)
			count += lod->groups[group].faces[face].verts[3] >= 0 ? 2u : 1u;
	}
	return count;
}

static bool build_opt_topology(const opt_mesh_t* source, AeronFlightComponent* component) {
	const uint32_t position_count = source->vertex_count > 0 ? (uint32_t)source->vertex_count : 0;
	const uint32_t face_count     = mesh_face_count(source);
	if (!position_count || !face_count)
		return false;
	component->topology.positions = calloc(position_count, sizeof *component->topology.positions);
	component->topology.faces     = calloc(face_count, sizeof *component->topology.faces);
	if (!component->topology.positions || !component->topology.faces)
		return false;
	component->topology.position_count = position_count;
	component->topology.face_count     = face_count;
	bool have_bounds                   = false;
	for (uint32_t index = 0; index < position_count; ++index) {
		component->topology.positions[index] = opt_position(&source->vertices[index]);
		include_point(&component->bounds, &component->topology.positions[index], &have_bounds);
	}
	uint32_t         write = 0;
	const opt_lod_t* lod   = &source->lods[0];
	for (int32_t group = 0; group < lod->group_count; ++group) {
		const opt_face_group_t* face_group = &lod->groups[group];
		for (int32_t face_index = 0; face_index < face_group->face_count; ++face_index) {
			const opt_face_t* source_face   = &face_group->faces[face_index];
			const uint32_t    triangles     = source_face->verts[3] >= 0 ? 2u : 1u;
			const int         corners[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
			for (uint32_t triangle = 0; triangle < triangles; ++triangle) {
				AeronFlightFace* face = &component->topology.faces[write++];
				for (uint32_t corner = 0; corner < 3; ++corner) {
					const int32_t vertex = source_face->verts[corners[triangle][corner]];
					if (vertex < 0 || (uint32_t)vertex >= position_count)
						return false;
					face->indices[corner] = (uint32_t)vertex;
				}
				face->normal = opt_unit_vector(&source_face->face_normal);
			}
		}
	}
	component->span = (AeronFlightVec3) {
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

static bool build_opt_component(const opt_mesh_t* source, AeronFlightComponent* component) {
	component->mesh_type       = source->descriptor.mesh_type;
	component->explosion_flags = (uint32_t)source->descriptor.explosion_type;
	component->target_id       = source->descriptor.target_id;
	component->target          = opt_position(&source->descriptor.target);
	if (source->has_rotation_scale) {
		component->has_rotation            = true;
		component->rotation.pivot          = opt_position(&source->rotation_scale.pivot);
		component->rotation.rotation_axis  = opt_q15_direction(&source->rotation_scale.rotation_axis);
		component->rotation.direction_axis = opt_q15_direction(&source->rotation_scale.direction_axis);
		component->rotation.up_axis        = opt_q15_direction(&source->rotation_scale.up_axis);
	}
	if (source->hardpoint_count > 0) {
		component->hardpoint_count = (uint32_t)source->hardpoint_count;
		component->hardpoints      = calloc(component->hardpoint_count, sizeof *component->hardpoints);
		if (!component->hardpoints)
			return false;
		for (uint32_t index = 0; index < component->hardpoint_count; ++index) {
			component->hardpoints[index].type     = source->hardpoints[index].type;
			component->hardpoints[index].position = opt_position(&source->hardpoints[index].pos);
		}
	}
	return build_opt_topology(source, component);
}

static void unpack_color(uint32_t packed, float rgba[4]) {
	rgba[0] = (float)((packed >> 16) & 0xff) / 255.0f;
	rgba[1] = (float)((packed >> 8) & 0xff) / 255.0f;
	rgba[2] = (float)(packed & 0xff) / 255.0f;
	rgba[3] = (float)((packed >> 24) & 0xff) / 255.0f;
}

static void append_opt_glows(const opt_mesh_t* source, uint32_t component_index,
							 AeronFlightComponent* component, AeronFlightModel* model) {
	component->first_engine_glow = model->engine_glow_count;
	component->engine_glow_count = source->engine_glow_count > 0 ? (uint32_t)source->engine_glow_count : 0;
	for (uint32_t index = 0; index < component->engine_glow_count; ++index) {
		const opt_engine_glow_t* source_glow = &source->engine_glows[index];
		AeronFlightEngineGlow*   glow        = &model->engine_glows[model->engine_glow_count++];
		glow->position                       = opt_position(&source_glow->position);
		glow->look                           = opt_unit_vector(&source_glow->look_axis);
		glow->up                             = opt_unit_vector(&source_glow->up_axis);
		glow->right                          = opt_unit_vector(&source_glow->right_axis);
		glow->dimensions                     = (AeronFlightVec3) {
			source_glow->dimensions.x * OPT_METERS_PER_UNIT,
			source_glow->dimensions.y * OPT_METERS_PER_UNIT,
			source_glow->dimensions.z * OPT_METERS_PER_UNIT,
		};
		unpack_color(source_glow->core_color, glow->core_rgba);
		unpack_color(source_glow->outer_color, glow->outer_rgba);
		glow->component_index = component_index;
		glow->enabled         = source_glow->is_disabled == 0;
	}
}

static bool build_opt_semantics(const opt_file_t* source, AeronFlightModel* model) {
	if (source->mesh_count <= 0)
		return false;
	uint32_t glow_count = 0;
	for (int32_t index = 0; index < source->mesh_count; ++index)
		if (source->meshes[index].engine_glow_count > 0)
			glow_count += (uint32_t)source->meshes[index].engine_glow_count;
	AeronFlightComponent*  components   = calloc((uint32_t)source->mesh_count, sizeof *components);
	AeronFlightEngineGlow* engine_glows = glow_count ? calloc(glow_count, sizeof *engine_glows) : NULL;
	if (!components || (glow_count && !engine_glows)) {
		free(components);
		free(engine_glows);
		return false;
	}
	model->component_count  = (uint32_t)source->mesh_count;
	model->components       = components;
	model->engine_glows     = engine_glows;
	model->bridge_component = -1;
	bool have_bounds        = false;
	for (uint32_t index = 0; index < model->component_count; ++index) {
		AeronFlightComponent* component = &model->components[index];
		if (!build_opt_component(&source->meshes[index], component))
			return false;
		append_opt_glows(&source->meshes[index], index, component, model);
		if (component->mesh_type == 7 && model->bridge_component < 0)
			model->bridge_component = (int32_t)index;
		include_point(&model->bounds, &component->bounds.min, &have_bounds);
		include_point(&model->bounds, &component->bounds.max, &have_bounds);
	}
	model->max_extent    = model->bounds.max.x - model->bounds.min.x;
	const float extent_y = model->bounds.max.y - model->bounds.min.y;
	const float extent_z = model->bounds.max.z - model->bounds.min.z;
	if (extent_y > model->max_extent)
		model->max_extent = extent_y;
	if (extent_z > model->max_extent)
		model->max_extent = extent_z;
	return true;
}

bool Aeron_OptModelBuildMemory(const void* bytes, size_t size, const char* label,
							   const AeronOptModelBuildOptions* options, AeronFlightModel* out,
							   AeronOptModelError* error) {
	if (out)
		memset(out, 0, sizeof *out);
	if (error)
		memset(error, 0, sizeof *error);
	if (!bytes || !size || !opt_model_arguments_valid(label, options, out))
		return opt_model_error(error, 1, "invalid OPT build arguments");
	opt_error_t parser_error = { { 0 } };
	opt_file_t* source       = opt_load_memory(bytes, size, &parser_error);
	if (!source)
		return opt_model_error(error, 2, parser_error.msg);

	OptGltfAlphaOverride* alpha_overrides = NULL;
	if (options->alpha_override_count) {
		alpha_overrides = calloc(options->alpha_override_count, sizeof *alpha_overrides);
		if (!alpha_overrides) {
			opt_free(source);
			return opt_model_error(error, 1, "out of memory for OPT alpha overrides");
		}
		for (size_t index = 0; index < options->alpha_override_count; ++index) {
			alpha_overrides[index].texture_name = options->alpha_overrides[index].texture_name;
			alpha_overrides[index].alpha_mode = (OptGltfAlphaMode)options->alpha_overrides[index].alpha_mode;
			alpha_overrides[index].alpha_cutoff = options->alpha_overrides[index].alpha_cutoff;
		}
	}
	const OptGltfBuildOptions build_options = {
		.smooth_angle_degrees = options->smooth_angle_degrees,
		.repair_normals       = true,
		.emissive             = options->emissive,
		.alpha_overrides      = alpha_overrides,
		.alpha_override_count = options->alpha_override_count,
	};
	OptGltfDocument* document  = NULL;
	const bool       converted = OptGltf_BuildMemory(source, label, &build_options, &document, &parser_error);
	free(alpha_overrides);
	if (!converted) {
		opt_free(source);
		return opt_model_error(error, 3, parser_error.msg);
	}
	if (!build_opt_semantics(source, out)) {
		OptGltf_Free(document);
		opt_free(source);
		Aeron_FlightModelFree(out);
		return opt_model_error(error, 4, "could not build OPT native semantics");
	}
	AeronGltfCookOptions cook_options;
	aeron_gltf_cook_default_options(&cook_options);
	cook_options.encoding           = AERON_GLTF_COOK_ENCODING_RGBA8;
	cook_options.zstd_supercompress = false;
	cook_options.verbose            = false;
	OptModelCookContext context     = { .document = document, .model = out, .label = label };
	const bool          cooked = aeron_gltf_cook_data(OptGltf_Data(document), label, opt_model_image_provider,
													  &context, opt_model_consumer, &context, &cook_options);
	OptGltf_Free(document);
	if (!cooked) {
		opt_free(source);
		Aeron_FlightModelFree(out);
		return opt_model_error(error, 4,
							   context.render_build_failed
								   ? "converted OPT has an invalid AERON_flight_model contract"
								   : "in-memory OPT texture cooking failed");
	}
	opt_free(source);
	if (options->emissive) {
		for (uint32_t index = 0; index < out->render.material_count; ++index) {
			AeronGltfMaterial* material = &out->render.materials[index];
			const float*       rect     = material->uv_xform[AERON_GLTF_CHANNEL_EMISSIVE];
			if (rect[2] > 0.0f && rect[3] > 0.0f)
				material->emissive_strength *= options->emissive_strength;
		}
	}
	return true;
}
