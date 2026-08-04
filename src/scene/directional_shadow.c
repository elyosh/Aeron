/* Stabilized cascaded depth shadows for the scene's key directional light. */

#include "internal.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct ShadowVsBlock {
	float view_proj[16];
	float model_to_world[16];
	uint32_t mesh_table_index;
	uint32_t _pad[3];
} ShadowVsBlock;
typedef char ShadowVsBlockSizeCheck[sizeof(ShadowVsBlock) == 144 ? 1 : -1];

typedef struct ShadowDebugUniforms {
	float output_size[2];
	float cascade_count;
	float selected_cascade;
} ShadowDebugUniforms;

typedef struct ShadowBound {
	float center[3];
	float radius;
} ShadowBound;

typedef struct ShadowBounds {
	float min[3];
	float max[3];
} ShadowBounds;

typedef struct ShadowSplitFrustum {
	float corners[8][3];
	float eye_to_world[9];
	float center[3];
	float radius;
} ShadowSplitFrustum;

#define SHADOW_FIT_EXTENT_BUCKETS 32.0f
#define SHADOW_FIT_SHRINK_FRAMES 30u

static float shadow_dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static double shadow_dot_origin(const double origin[3], const float axis[3]) {
	return origin[0] * (double)axis[0] + origin[1] * (double)axis[1] + origin[2] * (double)axis[2];
}

static void shadow_cross3(float out[3], const float a[3], const float b[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static int shadow_normalize3(float v[3]) {
	const float length = sqrtf(shadow_dot3(v, v));
	if (!(length > 1.0e-5f) || !isfinite(length)) {
		return 0;
	}
	v[0] /= length;
	v[1] /= length;
	v[2] /= length;
	return 1;
}

static void shadow_bounds_reset(ShadowBounds* bounds) {
	for (int axis = 0; axis < 3; axis++) {
		bounds->min[axis] = FLT_MAX;
		bounds->max[axis] = -FLT_MAX;
	}
}

static int shadow_bounds_valid(const ShadowBounds* bounds) {
	return bounds->min[0] <= bounds->max[0] && bounds->min[1] <= bounds->max[1] &&
		   bounds->min[2] <= bounds->max[2];
}

static void shadow_bounds_include_sphere(ShadowBounds* bounds, const ShadowBound* sphere,
										 const float x_axis[3], const float y_axis[3],
										 const float z_axis[3]) {
	const float projected[3] = {
		shadow_dot3(sphere->center, x_axis),
		shadow_dot3(sphere->center, y_axis),
		shadow_dot3(sphere->center, z_axis),
	};
	for (int axis = 0; axis < 3; axis++) {
		bounds->min[axis] = fminf(bounds->min[axis], projected[axis] - sphere->radius);
		bounds->max[axis] = fmaxf(bounds->max[axis], projected[axis] + sphere->radius);
	}
}

static const AeronSceneMeshInstance* shadow_caster(const struct AeronScene3D* scene, uint16_t index) {
	if (index < AERON_SCENE_MAX_INSTANCES) {
		return index < (uint16_t)scene->instance_count ? &scene->instances[index] : NULL;
	}
	index = (uint16_t)(index - AERON_SCENE_MAX_INSTANCES);
	return index < (uint16_t)scene->shadow_only_count ? &scene->shadow_only[index] : NULL;
}

static int shadow_instance_bound(const AeronSceneMeshInstance* instance, ShadowBound* out) {
	const AeronSceneMesh* mesh = instance ? instance->mesh : NULL;
	if (!mesh || !out) {
		return 0;
	}
	for (int row = 0; row < 3; row++) {
		out->center[row] = instance->transform[row * 4 + 3];
	}
	float scale = 0.0f;
	for (int row = 0; row < 3; row++) {
		const float row_scale = sqrtf(instance->transform[row * 4 + 0] * instance->transform[row * 4 + 0] +
									  instance->transform[row * 4 + 1] * instance->transform[row * 4 + 1] +
									  instance->transform[row * 4 + 2] * instance->transform[row * 4 + 2]);
		if (row_scale > scale) {
			scale = row_scale;
		}
	}
	const float local_extent[3] = {
		fmaxf(fabsf(mesh->bound_min[0]), fabsf(mesh->bound_max[0])),
		fmaxf(fabsf(mesh->bound_min[1]), fabsf(mesh->bound_max[1])),
		fmaxf(fabsf(mesh->bound_min[2]), fabsf(mesh->bound_max[2])),
	};
	const float local_radius         = sqrtf(shadow_dot3(local_extent, local_extent));
	float       articulation_padding = 0.0f;
	if (instance->mesh_table) {
		for (int mesh_index = 0; mesh_index < AERON_MAX_MESH_SLOTS; mesh_index++) {
			if (instance->mesh_table->visibility_packed[mesh_index >> 2][mesh_index & 3] < 0.5f) {
				continue;
			}
			const float tx          = instance->mesh_table->rows[mesh_index][0][3];
			const float ty          = instance->mesh_table->rows[mesh_index][1][3];
			const float tz          = instance->mesh_table->rows[mesh_index][2][3];
			const float translation = sqrtf(tx * tx + ty * ty + tz * tz);
			if (translation > articulation_padding) {
				articulation_padding = translation;
			}
		}
	}
	/* Bounds and articulation translations share the GPU vertex's raw local
	 * units. An origin-centered sphere remains valid under mesh rotations;
	 * pivot translations and the instance transform are applied exactly once. */
	out->radius = scale * (local_radius + articulation_padding);
	return isfinite(out->radius) && out->radius > 0.0f;
}

static void shadow_build_split_frustum(const struct AeronScene3D* scene, float near_z, float far_z,
									   ShadowSplitFrustum* out) {
	double       eye_corners[8][3];
	double       eye_center[3] = { 0.0, 0.0, 0.0 };
	const double tan_h         = tan((double)scene->camera.h_half_rad);
	const double tan_v         = tan((double)scene->camera.v_half_rad);
	for (int depth_index = 0; depth_index < 2; depth_index++) {
		const double depth = depth_index ? (double)far_z : (double)near_z;
		for (int corner = 0; corner < 4; corner++) {
			const double ndc_x = (corner & 1) ? 1.0 : -1.0;
			const double ndc_y = (corner & 2) ? 1.0 : -1.0;
			double*      eye   = eye_corners[depth_index * 4 + corner];
			eye[0]             = (ndc_x - (double)scene->camera.proj_x_offset) * depth * tan_h;
			eye[1]             = -(ndc_y - (double)scene->camera.proj_y_offset) * depth * tan_v;
			eye[2]             = depth;
			for (int axis = 0; axis < 3; axis++) {
				eye_center[axis] += eye[axis] * 0.125;
			}
		}
	}
	double radius_sq = 0.0;
	for (int corner = 0; corner < 8; corner++) {
		double distance_sq = 0.0;
		for (int axis = 0; axis < 3; axis++) {
			const double delta = eye_corners[corner][axis] - eye_center[axis];
			distance_sq += delta * delta;
		}
		radius_sq = fmax(radius_sq, distance_sq);
	}

	AeronSceneInternal_QuatToMat3(scene->camera.ori, out->eye_to_world);
	for (int axis = 0; axis < 3; axis++) {
		out->center[axis] = scene->camera.pos[axis] + out->eye_to_world[0 * 3 + axis] * (float)eye_center[0] +
							out->eye_to_world[1 * 3 + axis] * (float)eye_center[1] +
							out->eye_to_world[2 * 3 + axis] * (float)eye_center[2];
	}
	for (int corner = 0; corner < 8; corner++) {
		for (int axis = 0; axis < 3; axis++) {
			out->corners[corner][axis] = scene->camera.pos[axis] +
										 out->eye_to_world[0 * 3 + axis] * (float)eye_corners[corner][0] +
										 out->eye_to_world[1 * 3 + axis] * (float)eye_corners[corner][1] +
										 out->eye_to_world[2 * 3 + axis] * (float)eye_corners[corner][2];
		}
	}
	/* Radius is calculated entirely in eye space, so camera rotation cannot
	 * move it across a rounding boundary and resize the shadow texel grid. */
	out->radius = ceilf(fmaxf((float)sqrt(radius_sq), 1.0f));
}

static void shadow_project_split_bounds(const ShadowSplitFrustum* frustum, const float x_axis[3],
										const float y_axis[3], const float z_axis[3], ShadowBounds* out) {
	shadow_bounds_reset(out);
	for (int corner = 0; corner < 8; corner++) {
		const float projected[3] = {
			shadow_dot3(frustum->corners[corner], x_axis),
			shadow_dot3(frustum->corners[corner], y_axis),
			shadow_dot3(frustum->corners[corner], z_axis),
		};
		for (int axis = 0; axis < 3; axis++) {
			out->min[axis] = fminf(out->min[axis], projected[axis]);
			out->max[axis] = fmaxf(out->max[axis], projected[axis]);
		}
	}
}

static int shadow_bound_intersects_split(const struct AeronScene3D* scene, const ShadowSplitFrustum* frustum,
										 const ShadowBound* bound, float near_z, float far_z) {
	float delta[3] = {
		bound->center[0] - scene->camera.pos[0],
		bound->center[1] - scene->camera.pos[1],
		bound->center[2] - scene->camera.pos[2],
	};
	const float eye[3] = {
		shadow_dot3(delta, &frustum->eye_to_world[0]),
		shadow_dot3(delta, &frustum->eye_to_world[3]),
		shadow_dot3(delta, &frustum->eye_to_world[6]),
	};
	if (eye[2] + bound->radius < near_z || eye[2] - bound->radius > far_z) {
		return 0;
	}
	const float tan_h       = tanf(scene->camera.h_half_rad);
	const float tan_v       = tanf(scene->camera.v_half_rad);
	const float plane[4][3] = {
		{ 1.0f, 0.0f, (1.0f + scene->camera.proj_x_offset) * tan_h },
		{ -1.0f, 0.0f, (1.0f - scene->camera.proj_x_offset) * tan_h },
		{ 0.0f, 1.0f, (1.0f - scene->camera.proj_y_offset) * tan_v },
		{ 0.0f, -1.0f, (1.0f + scene->camera.proj_y_offset) * tan_v },
	};
	for (int side = 0; side < 4; side++) {
		const float normal_length = sqrtf(shadow_dot3(plane[side], plane[side]));
		if (shadow_dot3(eye, plane[side]) < -bound->radius * normal_length) {
			return 0;
		}
	}
	return 1;
}

static void shadow_build_matrix(float out[16], const float x_axis[3], const float y_axis[3],
								const float light_dir[3], float center_x, float center_y, float extent_x,
								float extent_y, float z_min, float z_max) {
	memset(out, 0, 16 * sizeof(float));
	const float inv_extent_x = 1.0f / extent_x;
	const float inv_extent_y = 1.0f / extent_y;
	const float inv_depth    = 1.0f / (z_max - z_min);
	for (int axis = 0; axis < 3; axis++) {
		out[0 * 4 + axis] = x_axis[axis] * inv_extent_x;
		out[1 * 4 + axis] = y_axis[axis] * inv_extent_y;
		out[2 * 4 + axis] = light_dir[axis] * inv_depth;
	}
	out[0 * 4 + 3] = -center_x * inv_extent_x;
	out[1 * 4 + 3] = -center_y * inv_extent_y;
	out[2 * 4 + 3] = -z_min * inv_depth;
	out[15]        = 1.0f;
}

static void shadow_stabilize_fit(struct AeronScene3D* scene, uint32_t cascade,
								 const AeronSceneDirectionalShadowDesc* desc, const float light_dir[3],
								 const float x_axis[3], const float y_axis[3], const ShadowBounds* raw,
								 float reference_extent, uint32_t usable_size, float center[2],
								 float extent[2], float world_per_texel[2]) {
	AeronSceneShadowFitHistory* history = &scene->shadow_fit_history[cascade];
	const float light_alignment = history->light_dir[0] * light_dir[0] +
								  history->light_dir[1] * light_dir[1] + history->light_dir[2] * light_dir[2];
	const float axis_alignment =
		history->x_axis[0] * x_axis[0] + history->x_axis[1] * x_axis[1] + history->x_axis[2] * x_axis[2];
	const float reference_delta = fabsf(history->reference_extent - reference_extent);
	const int   history_compatible =
		history->valid && history->fit_mode == desc->fit_mode &&
		history->tile_size == (desc->cascade_count == 1 ? desc->atlas_size : desc->atlas_size / 2) &&
		light_alignment > 0.9999f && axis_alignment > 0.9999f &&
		reference_delta <= fmaxf(reference_extent * 0.001f, 0.01f) && !scene->temporal.reset_history;
	if (!history_compatible) {
		memset(history, 0, sizeof *history);
		history->valid            = 1;
		history->fit_mode         = desc->fit_mode;
		history->tile_size        = desc->cascade_count == 1 ? desc->atlas_size : desc->atlas_size / 2;
		history->light_dir[0]     = light_dir[0];
		history->light_dir[1]     = light_dir[1];
		history->light_dir[2]     = light_dir[2];
		history->x_axis[0]        = x_axis[0];
		history->x_axis[1]        = x_axis[1];
		history->x_axis[2]        = x_axis[2];
		history->reference_extent = reference_extent;
	}

	const float bucket = fmaxf(reference_extent / SHADOW_FIT_EXTENT_BUCKETS, 1.0e-3f);
	/* The stable split radius makes bucket sizes invariant under camera
	 * rotation. Grow immediately, but delay shrinking to prevent pulsing as
	 * receivers enter and leave the camera frustum. */
	float target[2] = {
		ceilf(fmaxf(0.5f * (raw->max[0] - raw->min[0]), 1.0f) / bucket) * bucket,
		ceilf(fmaxf(0.5f * (raw->max[1] - raw->min[1]), 1.0f) / bucket) * bucket,
	};
	for (int axis = 0; axis < 2; axis++) {
		if (history->extent[axis] <= 0.0f || target[axis] > history->extent[axis]) {
			history->extent[axis]        = target[axis];
			history->shrink_frames[axis] = 0;
		} else if (target[axis] < history->extent[axis] - 0.5f * bucket) {
			history->shrink_frames[axis]++;
			if (history->shrink_frames[axis] >= SHADOW_FIT_SHRINK_FRAMES) {
				history->extent[axis]        = target[axis];
				history->shrink_frames[axis] = 0;
			}
		} else {
			history->shrink_frames[axis] = 0;
		}
		extent[axis] = history->extent[axis];
	}

	const double origin[2] = {
		shadow_dot_origin(desc->world_origin, x_axis),
		shadow_dot_origin(desc->world_origin, y_axis),
	};
	for (int attempt = 0; attempt < 2; attempt++) {
		for (int axis = 0; axis < 2; axis++) {
			world_per_texel[axis]        = (2.0f * extent[axis]) / (float)usable_size;
			const double raw_center      = 0.5 * ((double)raw->min[axis] + (double)raw->max[axis]);
			const double absolute_center = origin[axis] + raw_center;
			center[axis]         = (float)(floor(absolute_center / (double)world_per_texel[axis] + 0.5) *
											   (double)world_per_texel[axis] -
										   origin[axis]);
			const float required = fmaxf(center[axis] - raw->min[axis], raw->max[axis] - center[axis]);
			if (required > extent[axis]) {
				extent[axis]          = ceilf(required / bucket) * bucket;
				history->extent[axis] = extent[axis];
			}
		}
	}
}

static AeronGraphicsPipeline* shadow_pipeline(struct AeronScene3D* scene, AeronCullMode cull,
											  float constant_bias, float slope_bias) {
	AeronVertexAttributeDesc attributes[2] = {
		{ .location    = 0,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(AeronGltfVertex, pos) },
		{ .location    = 1,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT,
		  .offset      = (uint32_t)offsetof(AeronGltfVertex, mesh_index) },
	};
	const AeronVertexBufferLayoutDesc vertex_buffer = {
		.slot   = 0,
		.stride = (uint32_t)sizeof(AeronGltfVertex),
	};
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader = scene->shadow_vs,
		.fragment_shader = scene->shadow_fs,
		.primitive_type = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode = cull,
		.vertex_buffers = &vertex_buffer,
		.vertex_buffer_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT,
		/* The atlas uses reversed Z, so moving caster depth away from the
		 * light requires negative rasterizer bias factors. */
		.rasterizer = {
			.depth_bias = constant_bias > 0.0f || slope_bias > 0.0f,
			.depth_bias_constant_factor = -constant_bias,
			.depth_bias_slope_factor = -slope_bias,
		},
		.depth = { .depth_test = 1, .depth_write = 1, .compare = AERON_COMPARE_GREATER_EQUAL },
	});
}

static int shadow_ensure_common(struct AeronScene3D* scene) {
	if (!scene->shadow_sampler) {
		scene->shadow_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter     = AERON_FILTER_LINEAR,
			.mag_filter     = AERON_FILTER_LINEAR,
			.mip_filter     = AERON_FILTER_NEAREST,
			.address_u      = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v      = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w      = AERON_ADDRESS_CLAMP_TO_EDGE,
			.enable_compare = 1,
			.compare        = AERON_COMPARE_GREATER_EQUAL,
		});
	}
	if (!scene->shadow_depth_sampler) {
		scene->shadow_depth_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_NEAREST,
			.mag_filter = AERON_FILTER_NEAREST,
			.mip_filter = AERON_FILTER_NEAREST,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		});
	}
	if (!scene->shadow_fallback) {
		scene->shadow_fallback = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
			.width      = 1,
			.height     = 1,
			.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.sampled    = 1,
			.debug_name = "scene.shadow_fallback",
		});
	}
	return scene->shadow_sampler && scene->shadow_depth_sampler && scene->shadow_fallback;
}

static int shadow_ensure_pipelines(struct AeronScene3D* scene, float constant_bias, float slope_bias) {
	int complete = 1;
	for (int cull = 0; cull < 3; cull++) {
		complete = complete && scene->shadow_pipes[cull] != NULL;
	}
	if (complete && scene->shadow_pipeline_constant_bias == constant_bias &&
		scene->shadow_pipeline_slope_bias == slope_bias) {
		return 1;
	}

	AeronGraphicsPipeline* replacement[3] = { NULL, NULL, NULL };
	for (int cull = 0; cull < 3; cull++) {
		replacement[cull] = shadow_pipeline(scene, (AeronCullMode)cull, constant_bias, slope_bias);
		if (!replacement[cull]) {
			for (int created = 0; created < 3; created++) {
				Aeron_DestroyGraphicsPipeline(replacement[created]);
			}
			return 0;
		}
	}
	for (int cull = 0; cull < 3; cull++) {
		Aeron_DestroyGraphicsPipeline(scene->shadow_pipes[cull]);
		scene->shadow_pipes[cull] = replacement[cull];
	}
	scene->shadow_pipeline_constant_bias = constant_bias;
	scene->shadow_pipeline_slope_bias    = slope_bias;
	return 1;
}

static int shadow_ensure_debug_pipeline(struct AeronScene3D* scene) {
	if (scene->shadow_debug_tried) {
		return scene->shadow_debug_pipe != NULL;
	}
	scene->shadow_debug_tried = 1;
	scene->shadow_debug_vs =
		AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert", AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	scene->shadow_debug_fs = AeronSceneInternal_CompileShader("scene_directional_shadow_debug.frag",
															  AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	if (!scene->shadow_debug_vs || !scene->shadow_debug_fs) {
		return 0;
	}
	scene->shadow_debug_pipe = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader   = scene->shadow_debug_vs,
		.fragment_shader = scene->shadow_debug_fs,
		.primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode       = AERON_CULL_NONE,
		.color_format    = scene->color_format,
	});
	return scene->shadow_debug_pipe != NULL;
}

static int shadow_ensure_resources(struct AeronScene3D* scene, uint32_t atlas_size, float constant_bias,
								   float slope_bias) {
	if (!shadow_ensure_common(scene)) {
		return 0;
	}
	if (!scene->shadow_tried) {
		scene->shadow_tried = 1;
		scene->shadow_vs    = AeronSceneInternal_CompileShader("scene_directional_shadow.vert",
															   AERON_SHADER_STAGE_VERTEX, 0, 1, 1);
		scene->shadow_fs    = AeronSceneInternal_CompileShader("scene_directional_shadow.frag",
															   AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	}
	if (!scene->shadow_vs || !scene->shadow_fs ||
		!shadow_ensure_pipelines(scene, constant_bias, slope_bias)) {
		return 0;
	}
	if (!scene->shadow_atlas || scene->shadow_resource_atlas_size != atlas_size) {
		if (scene->shadow_atlas) {
			Aeron_DestroyDepthTarget(scene->shadow_atlas);
		}
		scene->shadow_atlas               = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
			.width      = (int)atlas_size,
			.height     = (int)atlas_size,
			.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.sampled    = 1,
			.debug_name = "scene.directional_shadow_atlas",
		});
		scene->shadow_resource_atlas_size = scene->shadow_atlas ? atlas_size : 0;
	}
	return scene->shadow_atlas != NULL;
}

static int shadow_ensure_receiver_local_atlas(struct AeronScene3D* scene) {
	if (!scene->receiver_local_shadow_atlas) {
		scene->receiver_local_shadow_atlas = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
			.width      = AERON_SCENE_RECEIVER_LOCAL_SHADOW_SIZE,
			.height     = AERON_SCENE_RECEIVER_LOCAL_SHADOW_SIZE,
			.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.sampled    = 1,
			.debug_name = "scene.receiver_local_shadow",
		});
	}
	return scene->receiver_local_shadow_atlas != NULL;
}

static uint32_t shadow_sanitize_atlas(uint32_t atlas_size) {
	if (atlas_size < 1024) {
		return 1024;
	}
	if (atlas_size > 8192) {
		return 8192;
	}
	uint32_t power = 1;
	while (power < atlas_size) {
		power <<= 1;
	}
	return power;
}

static float shadow_receiver_bias_filter_radius(const AeronSceneDirectionalShadowDesc* desc) {
	if (desc->filter_quality == 0) {
		return 1.0f;
	}
	return fmaxf(desc->contact_hardening ? desc->max_filter_radius : desc->filter_radius, 1.0f);
}

static void shadow_prepare_receiver_local(struct AeronScene3D*                   scene,
										  const AeronSceneDirectionalShadowDesc* desc,
										  const float light_dir[3], const float x_axis[3],
										  const float y_axis[3]) {
	memset(&scene->receiver_local_shadow_uniform, 0, sizeof scene->receiver_local_shadow_uniform);
	scene->receiver_local_shadow_caster_count         = 0;
	scene->shadow_stats.receiver_local_active         = 0;
	scene->shadow_stats.receiver_local_size           = 0;
	scene->shadow_stats.receiver_local_caster_count   = 0;
	scene->shadow_stats.receiver_local_triangle_count = 0;

	float receiver_x_min = FLT_MAX;
	float receiver_x_max = -FLT_MAX;
	float receiver_y_min = FLT_MAX;
	float receiver_y_max = -FLT_MAX;
	float receiver_z_min = FLT_MAX;
	float receiver_z_max = -FLT_MAX;
	for (int index = 0; index < scene->instance_count; index++) {
		const AeronSceneMeshInstance* instance = &scene->instances[index];
		if ((instance->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) == 0 ||
			(instance->shadow_flags & AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW) != 0) {
			continue;
		}
		ShadowBound bound;
		if (!shadow_instance_bound(instance, &bound)) {
			continue;
		}
		const float bx = shadow_dot3(bound.center, x_axis);
		const float by = shadow_dot3(bound.center, y_axis);
		const float bz = shadow_dot3(bound.center, light_dir);
		receiver_x_min = fminf(receiver_x_min, bx - bound.radius);
		receiver_x_max = fmaxf(receiver_x_max, bx + bound.radius);
		receiver_y_min = fminf(receiver_y_min, by - bound.radius);
		receiver_y_max = fmaxf(receiver_y_max, by + bound.radius);
		receiver_z_min = fminf(receiver_z_min, bz - bound.radius);
		receiver_z_max = fmaxf(receiver_z_max, bz + bound.radius);
	}
	if (receiver_x_min == FLT_MAX) {
		return;
	}

	const uint32_t atlas_size  = AERON_SCENE_RECEIVER_LOCAL_SHADOW_SIZE;
	const uint32_t usable_size = atlas_size - 2u * AERON_SCENE_SHADOW_GUARD_TEXELS;
	float          center_x    = 0.5f * (receiver_x_min + receiver_x_max);
	float          center_y    = 0.5f * (receiver_y_min + receiver_y_max);
	float extent = fmaxf(0.5f * (receiver_x_max - receiver_x_min), 0.5f * (receiver_y_max - receiver_y_min));
	extent       = fmaxf(extent, 1.0f);
	/* Keep the entire receiver inside the unclamped filter footprint. */
	const float initial_world_per_texel = (2.0f * extent) / (float)usable_size;
	const float filter_radius = shadow_receiver_bias_filter_radius(desc);
	extent += (desc->normal_bias * filter_radius + filter_radius + 2.0f) *
			  initial_world_per_texel;
	const float world_per_texel = (2.0f * extent) / (float)usable_size;

	const double origin_x          = shadow_dot_origin(desc->world_origin, x_axis);
	const double origin_y          = shadow_dot_origin(desc->world_origin, y_axis);
	const double absolute_center_x = origin_x + (double)center_x;
	const double absolute_center_y = origin_y + (double)center_y;
	center_x = (float)(floor(absolute_center_x / (double)world_per_texel + 0.5) * (double)world_per_texel -
					   origin_x);
	center_y = (float)(floor(absolute_center_y / (double)world_per_texel + 0.5) * (double)world_per_texel -
					   origin_y);

	float     caster_z_max    = receiver_z_max;
	int       excluded_caster = 0;
	const int combined_count  = scene->instance_count + scene->shadow_only_count;
	for (int candidate = 0; candidate < combined_count; candidate++) {
		const uint16_t encoded =
			candidate < scene->instance_count
				? (uint16_t)candidate
				: (uint16_t)(AERON_SCENE_MAX_INSTANCES + candidate - scene->instance_count);
		const AeronSceneMeshInstance* instance = shadow_caster(scene, encoded);
		ShadowBound                   bound;
		if (!instance || !instance->mesh || instance->mesh->opaque_index_count == 0 ||
			(instance->shadow_flags & AERON_SCENE_INSTANCE_NO_CAST_SHADOW) != 0 ||
			!shadow_instance_bound(instance, &bound)) {
			continue;
		}
		const float bx = shadow_dot3(bound.center, x_axis);
		const float by = shadow_dot3(bound.center, y_axis);
		const float bz = shadow_dot3(bound.center, light_dir);
		if (fabsf(bx - center_x) > extent + bound.radius || fabsf(by - center_y) > extent + bound.radius ||
			bz + bound.radius < receiver_z_min || bz - bound.radius > receiver_z_max + desc->max_distance) {
			continue;
		}
		if ((instance->shadow_flags & AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW) != 0) {
			excluded_caster = 1;
			continue;
		}
		if (scene->receiver_local_shadow_caster_count < AERON_SCENE_MAX_SHADOW_CASTERS) {
			scene->receiver_local_shadow_casters[scene->receiver_local_shadow_caster_count++] = encoded;
			caster_z_max = fmaxf(caster_z_max, bz + bound.radius);
		}
	}
	if (!excluded_caster || !shadow_ensure_receiver_local_atlas(scene)) {
		scene->receiver_local_shadow_caster_count = 0;
		return;
	}

	const float  depth_margin       = fmaxf(world_per_texel * 8.0f, 1.0f);
	const float  depth_quantum      = fmaxf(world_per_texel * 16.0f, 1.0f);
	const double origin_z           = shadow_dot_origin(desc->world_origin, light_dir);
	const double raw_absolute_z_min = origin_z + (double)receiver_z_min - (double)depth_margin;
	const double snapped_absolute_z_min =
		floor(raw_absolute_z_min / (double)depth_quantum) * (double)depth_quantum;
	const float z_min               = (float)(snapped_absolute_z_min - origin_z);
	const float required_depth_span = fmaxf(caster_z_max + depth_margin - z_min, depth_quantum);
	const float depth_span          = ceilf(required_depth_span / depth_quantum) * depth_quantum;
	const float z_max               = z_min + depth_span;

	AeronSceneDirectionalShadowUniform* uniform = &scene->receiver_local_shadow_uniform;
	shadow_build_matrix(uniform->view_proj[0], x_axis, y_axis, light_dir, center_x, center_y, extent, extent,
						z_min, z_max);
	const float atlas_scale         = (float)usable_size / (float)atlas_size;
	uniform->atlas_scale_bias[0][0] = atlas_scale;
	uniform->atlas_scale_bias[0][1] = atlas_scale;
	uniform->atlas_scale_bias[0][2] = (float)AERON_SCENE_SHADOW_GUARD_TEXELS / (float)atlas_size;
	uniform->atlas_scale_bias[0][3] = (float)AERON_SCENE_SHADOW_GUARD_TEXELS / (float)atlas_size;
	const float half_texel          = 0.5f / (float)atlas_size;
	uniform->atlas_clamp[0][0]      = uniform->atlas_scale_bias[0][2] + half_texel;
	uniform->atlas_clamp[0][1]      = uniform->atlas_scale_bias[0][3] + half_texel;
	uniform->atlas_clamp[0][2]      = uniform->atlas_scale_bias[0][2] + atlas_scale - half_texel;
	uniform->atlas_clamp[0][3]      = uniform->atlas_scale_bias[0][3] + atlas_scale - half_texel;
	uniform->split_data[0][0]       = 0.0f;
	uniform->split_data[0][1]       = desc->max_distance;
	uniform->split_data[0][2]       = desc->max_distance;
	uniform->split_data[0][3]       = world_per_texel / depth_span;
	uniform->texel_data[0][0]       = world_per_texel;
	uniform->texel_data[0][1]       = world_per_texel;
	uniform->texel_data[0][2]       = world_per_texel / depth_span;
	uniform->texel_data[0][3]       = world_per_texel / depth_span;
	uniform->params[0]              = 1.0f;
	uniform->params[1]              = 1.0f;
	uniform->params[2]              = (float)desc->filter_quality;
	memcpy(uniform->camera_pos, scene->shadow_uniform.camera_pos, sizeof uniform->camera_pos);
	memcpy(uniform->camera_forward, scene->shadow_uniform.camera_forward, sizeof uniform->camera_forward);
	memcpy(uniform->bias, scene->shadow_uniform.bias, sizeof uniform->bias);
	uniform->bias[3] = desc->max_distance;
	uniform->fade[0] = desc->max_distance;
	uniform->fade[1] = desc->max_distance;
	uniform->fade[2] = 1.0f / (float)atlas_size;
	uniform->fade[3] = desc->receiver_plane_bias;
	memcpy(uniform->pcss, scene->shadow_uniform.pcss, sizeof uniform->pcss);
	memcpy(uniform->pcss_temporal, scene->shadow_uniform.pcss_temporal, sizeof uniform->pcss_temporal);
	memcpy(uniform->light_dir, scene->shadow_uniform.light_dir, sizeof uniform->light_dir);

	scene->shadow_stats.receiver_local_active       = 1;
	scene->shadow_stats.receiver_local_size         = atlas_size;
	scene->shadow_stats.receiver_local_caster_count = scene->receiver_local_shadow_caster_count;
}

static int shadow_bounds_overlap_xy(const ShadowBounds* bounds, float x, float y, float radius) {
	return x + radius >= bounds->min[0] && x - radius <= bounds->max[0] && y + radius >= bounds->min[1] &&
		   y - radius <= bounds->max[1];
}

static void shadow_prepare_cascade(struct AeronScene3D* scene, const AeronSceneDirectionalShadowDesc* desc,
								   const float light_dir[3], const float x_axis[3], const float y_axis[3],
								   const float split_edges[AERON_SCENE_SHADOW_MAX_CASCADES + 1],
								   uint32_t cascade, uint32_t usable_size, const ShadowBound object_bounds[],
								   const uint16_t object_indices[], const uint8_t bound_valid[],
								   const uint8_t caster_valid[], int combined_count) {
	float fit_near = split_edges[cascade];
	if (cascade > 0) {
		fit_near -= (split_edges[cascade] - split_edges[cascade - 1]) * desc->transition_fraction;
	}
	const float        fit_far = split_edges[cascade + 1];
	ShadowSplitFrustum frustum;
	shadow_build_split_frustum(scene, fit_near, fit_far, &frustum);
	ShadowBounds split_bounds;
	shadow_project_split_bounds(&frustum, x_axis, y_axis, light_dir, &split_bounds);

	ShadowBounds fit_bounds = split_bounds;
	ShadowBounds receiver_bounds;
	shadow_bounds_reset(&receiver_bounds);
	uint32_t receiver_count = 0;
	if (desc->fit_mode == AERON_SCENE_SHADOW_FIT_SCENE_DEPENDENT) {
		/* Receiver bounds exclude off-screen objects. Caster bounds remain
		 * independent of camera visibility so shadows can enter the view. */
		for (int candidate = 0; candidate < scene->instance_count; candidate++) {
			const AeronSceneMeshInstance* instance = &scene->instances[candidate];
			if (!bound_valid[candidate] ||
				(instance->shadow_flags & AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW) != 0 ||
				!shadow_bound_intersects_split(scene, &frustum, &object_bounds[candidate], fit_near,
											   fit_far)) {
				continue;
			}
			shadow_bounds_include_sphere(&receiver_bounds, &object_bounds[candidate], x_axis, y_axis,
										 light_dir);
			receiver_count++;
		}
		if (shadow_bounds_valid(&receiver_bounds)) {
			ShadowBounds caster_bounds;
			shadow_bounds_reset(&caster_bounds);
			for (int candidate = 0; candidate < combined_count; candidate++) {
				if (!caster_valid[candidate]) {
					continue;
				}
				const ShadowBound* bound = &object_bounds[candidate];
				const float        bx    = shadow_dot3(bound->center, x_axis);
				const float        by    = shadow_dot3(bound->center, y_axis);
				const float        bz    = shadow_dot3(bound->center, light_dir);
				if (!shadow_bounds_overlap_xy(&split_bounds, bx, by, bound->radius) ||
					bz + bound->radius < receiver_bounds.min[2] ||
					bz - bound->radius > receiver_bounds.max[2] + desc->max_distance) {
					continue;
				}
				shadow_bounds_include_sphere(&caster_bounds, bound, x_axis, y_axis, light_dir);
			}
			for (int axis = 0; axis < 2; axis++) {
				fit_bounds.min[axis] = fmaxf(split_bounds.min[axis], receiver_bounds.min[axis]);
				fit_bounds.max[axis] = fminf(split_bounds.max[axis], receiver_bounds.max[axis]);
				if (shadow_bounds_valid(&caster_bounds)) {
					fit_bounds.min[axis] = fmaxf(fit_bounds.min[axis], caster_bounds.min[axis]);
					fit_bounds.max[axis] = fminf(fit_bounds.max[axis], caster_bounds.max[axis]);
				}
			}
		}
		if (!shadow_bounds_valid(&fit_bounds)) {
			fit_bounds     = split_bounds;
			receiver_count = 0;
		}
	}

	float center[2];
	float extent[2];
	float world_per_texel[2];
	if (desc->fit_mode == AERON_SCENE_SHADOW_FIT_STABLE) {
		extent[0]                      = frustum.radius;
		extent[1]                      = frustum.radius;
		world_per_texel[0]             = (2.0f * extent[0]) / (float)usable_size;
		world_per_texel[1]             = world_per_texel[0];
		const double origin_x          = shadow_dot_origin(desc->world_origin, x_axis);
		const double origin_y          = shadow_dot_origin(desc->world_origin, y_axis);
		const double absolute_center_x = origin_x + (double)shadow_dot3(frustum.center, x_axis);
		const double absolute_center_y = origin_y + (double)shadow_dot3(frustum.center, y_axis);
		center[0] =
			(float)(floor(absolute_center_x / (double)world_per_texel[0] + 0.5) * (double)world_per_texel[0] -
					origin_x);
		center[1] =
			(float)(floor(absolute_center_y / (double)world_per_texel[1] + 0.5) * (double)world_per_texel[1] -
					origin_y);
	} else {
		const float extent_bucket              = fmaxf(frustum.radius / SHADOW_FIT_EXTENT_BUCKETS, 1.0e-3f);
		const float initial_world_per_texel[2] = {
			2.0f * ceilf(fmaxf(0.5f * (fit_bounds.max[0] - fit_bounds.min[0]), 1.0f) / extent_bucket) *
				extent_bucket / (float)usable_size,
			2.0f * ceilf(fmaxf(0.5f * (fit_bounds.max[1] - fit_bounds.min[1]), 1.0f) / extent_bucket) *
				extent_bucket / (float)usable_size,
		};
		const float filter_radius = shadow_receiver_bias_filter_radius(desc);
		const float max_initial_world_per_texel =
			fmaxf(initial_world_per_texel[0], initial_world_per_texel[1]);
		const float normal_bias_world =
			desc->normal_bias * filter_radius * max_initial_world_per_texel;
		for (int axis = 0; axis < 2; axis++) {
			const float padding =
				normal_bias_world + (filter_radius + 2.0f) * initial_world_per_texel[axis];
			fit_bounds.min[axis] -= padding;
			fit_bounds.max[axis] += padding;
		}
		shadow_stabilize_fit(scene, cascade, desc, light_dir, x_axis, y_axis, &fit_bounds, frustum.radius,
							 usable_size, center, extent, world_per_texel);
	}

	float receiver_z_min = split_bounds.min[2];
	float receiver_z_max = split_bounds.max[2];
	if (desc->fit_mode == AERON_SCENE_SHADOW_FIT_STABLE) {
		const float receiver_z_center = shadow_dot3(frustum.center, light_dir);
		receiver_z_min                = receiver_z_center - frustum.radius;
		receiver_z_max                = receiver_z_center + frustum.radius;
	} else if (desc->fit_mode == AERON_SCENE_SHADOW_FIT_SCENE_DEPENDENT &&
			   shadow_bounds_valid(&receiver_bounds)) {
		receiver_z_min = receiver_bounds.min[2];
		receiver_z_max = receiver_bounds.max[2];
	}

	float caster_z_max = receiver_z_max;
	for (int candidate = 0; candidate < combined_count; candidate++) {
		if (!caster_valid[candidate]) {
			continue;
		}
		const ShadowBound* bound = &object_bounds[candidate];
		const float        bx    = shadow_dot3(bound->center, x_axis);
		const float        by    = shadow_dot3(bound->center, y_axis);
		const float        bz    = shadow_dot3(bound->center, light_dir);
		if (fabsf(bx - center[0]) > extent[0] + bound->radius ||
			fabsf(by - center[1]) > extent[1] + bound->radius || bz + bound->radius < receiver_z_min ||
			bz - bound->radius > receiver_z_max + desc->max_distance) {
			continue;
		}
		uint16_t* count = &scene->shadow_caster_count[cascade];
		if (*count < AERON_SCENE_MAX_SHADOW_CASTERS) {
			scene->shadow_casters[cascade][(*count)++] = object_indices[candidate];
			caster_z_max                               = fmaxf(caster_z_max, bz + bound->radius);
		}
	}

	const float  max_world_per_texel = fmaxf(world_per_texel[0], world_per_texel[1]);
	const float  depth_margin        = fmaxf(max_world_per_texel * 8.0f, 1.0f);
	const float  depth_quantum       = fmaxf(max_world_per_texel * 16.0f, 1.0f);
	const double origin_z            = shadow_dot_origin(desc->world_origin, light_dir);
	const double raw_absolute_z_min  = origin_z + (double)receiver_z_min - (double)depth_margin;
	const double snapped_absolute_z_min =
		floor(raw_absolute_z_min / (double)depth_quantum) * (double)depth_quantum;
	const float z_min = (float)(snapped_absolute_z_min - origin_z);
	float       required_depth_span;
	if (desc->fit_mode == AERON_SCENE_SHADOW_FIT_STABLE) {
		required_depth_span = 2.0f * frustum.radius + desc->max_distance + 2.0f * depth_margin;
	} else {
		required_depth_span = fmaxf(caster_z_max + depth_margin - z_min, depth_quantum);
	}
	const float depth_span = (ceilf(required_depth_span / depth_quantum) +
							  (desc->fit_mode == AERON_SCENE_SHADOW_FIT_STABLE ? 1.0f : 0.0f)) *
							 depth_quantum;
	const float z_max      = z_min + depth_span;
	shadow_build_matrix(scene->shadow_uniform.view_proj[cascade], x_axis, y_axis, light_dir, center[0],
						center[1], extent[0], extent[1], z_min, z_max);

	const uint32_t tile_size   = desc->cascade_count == 1 ? desc->atlas_size : desc->atlas_size / 2;
	const uint32_t tile_x      = cascade & 1u;
	const uint32_t tile_y      = cascade >> 1u;
	const float    atlas_scale = (float)usable_size / (float)desc->atlas_size;
	scene->shadow_uniform.atlas_scale_bias[cascade][0] = atlas_scale;
	scene->shadow_uniform.atlas_scale_bias[cascade][1] = atlas_scale;
	scene->shadow_uniform.atlas_scale_bias[cascade][2] =
		(float)(tile_x * tile_size + AERON_SCENE_SHADOW_GUARD_TEXELS) / (float)desc->atlas_size;
	scene->shadow_uniform.atlas_scale_bias[cascade][3] =
		(float)(tile_y * tile_size + AERON_SCENE_SHADOW_GUARD_TEXELS) / (float)desc->atlas_size;
	const float half_texel = 0.5f / (float)desc->atlas_size;
	scene->shadow_uniform.atlas_clamp[cascade][0] =
		scene->shadow_uniform.atlas_scale_bias[cascade][2] + half_texel;
	scene->shadow_uniform.atlas_clamp[cascade][1] =
		scene->shadow_uniform.atlas_scale_bias[cascade][3] + half_texel;
	scene->shadow_uniform.atlas_clamp[cascade][2] =
		scene->shadow_uniform.atlas_scale_bias[cascade][2] + atlas_scale - half_texel;
	scene->shadow_uniform.atlas_clamp[cascade][3] =
		scene->shadow_uniform.atlas_scale_bias[cascade][3] + atlas_scale - half_texel;
	scene->shadow_uniform.split_data[cascade][0] = split_edges[cascade];
	scene->shadow_uniform.split_data[cascade][1] = split_edges[cascade + 1];
	scene->shadow_uniform.split_data[cascade][2] =
		split_edges[cascade + 1] -
		(split_edges[cascade + 1] - split_edges[cascade]) * desc->transition_fraction;
	scene->shadow_uniform.split_data[cascade][3] = max_world_per_texel / depth_span;
	scene->shadow_uniform.texel_data[cascade][0] = world_per_texel[0];
	scene->shadow_uniform.texel_data[cascade][1] = world_per_texel[1];
	scene->shadow_uniform.texel_data[cascade][2] = world_per_texel[0] / depth_span;
	scene->shadow_uniform.texel_data[cascade][3] = world_per_texel[1] / depth_span;

	const float stable_world_per_texel                   = (2.0f * frustum.radius) / (float)usable_size;
	scene->shadow_stats.split_near[cascade]              = split_edges[cascade];
	scene->shadow_stats.split_far[cascade]               = split_edges[cascade + 1];
	scene->shadow_stats.world_units_per_texel[cascade]   = max_world_per_texel;
	scene->shadow_stats.world_units_per_texel_x[cascade] = world_per_texel[0];
	scene->shadow_stats.world_units_per_texel_y[cascade] = world_per_texel[1];
	scene->shadow_stats.texel_density_gain[cascade] =
		stable_world_per_texel / sqrtf(world_per_texel[0] * world_per_texel[1]);
	scene->shadow_stats.receiver_count[cascade] = receiver_count;
	scene->shadow_stats.caster_count[cascade]   = scene->shadow_caster_count[cascade];
}

int AeronSceneDirectionalShadow_Prepare(struct AeronScene3D* scene) {
	if (!scene) {
		return 0;
	}
	memset(&scene->shadow_uniform, 0, sizeof scene->shadow_uniform);
	memset(&scene->receiver_local_shadow_uniform, 0, sizeof scene->receiver_local_shadow_uniform);
	memset(&scene->shadow_stats, 0, sizeof scene->shadow_stats);
	memset(scene->shadow_caster_count, 0, sizeof scene->shadow_caster_count);
	scene->receiver_local_shadow_caster_count = 0;
	if (!shadow_ensure_common(scene)) {
		return 0;
	}

	AeronSceneDirectionalShadowDesc desc = scene->directional_shadow;
	float light_dir[3]                   = { desc.light_dir[0], desc.light_dir[1], desc.light_dir[2] };
	if (!desc.enabled || !shadow_normalize3(light_dir)) {
		memset(scene->shadow_fit_history, 0, sizeof scene->shadow_fit_history);
		return 1;
	}
	desc.cascade_count = desc.cascade_count < 1 ? 1 : desc.cascade_count;
	if (desc.cascade_count > AERON_SCENE_SHADOW_MAX_CASCADES) {
		desc.cascade_count = AERON_SCENE_SHADOW_MAX_CASCADES;
	}
	if (desc.fit_mode > AERON_SCENE_SHADOW_FIT_SCENE_DEPENDENT) {
		desc.fit_mode = AERON_SCENE_SHADOW_FIT_STABLE;
	}
	desc.atlas_size         = shadow_sanitize_atlas(desc.atlas_size);
	desc.max_distance       = fmaxf(desc.max_distance, scene->camera.near_z + 1.0f);
	desc.split_lambda       = fminf(fmaxf(desc.split_lambda, 0.0f), 1.0f);
	desc.explicit_splits    = desc.explicit_splits ? 1 : 0;
	const float split_gap   = 0.001f;
	desc.split_positions[0] = fminf(fmaxf(desc.split_positions[0], split_gap), 1.0f - 3.0f * split_gap);
	desc.split_positions[1] =
		fminf(fmaxf(desc.split_positions[1], desc.split_positions[0] + split_gap), 1.0f - 2.0f * split_gap);
	desc.split_positions[2] =
		fminf(fmaxf(desc.split_positions[2], desc.split_positions[1] + split_gap), 1.0f - split_gap);
	desc.filter_quality               = desc.filter_quality > 3 ? 3 : desc.filter_quality;
	desc.filter_radius                = fminf(fmaxf(desc.filter_radius, 0.5f), 3.0f);
	desc.contact_hardening            = desc.contact_hardening ? 1 : 0;
	desc.light_angular_radius_degrees = fminf(fmaxf(desc.light_angular_radius_degrees, 0.0f), 5.0f);
	desc.max_filter_radius            = fminf(fmaxf(desc.max_filter_radius, desc.filter_radius), 16.0f);
	desc.pcss_min_filter_radius       = fminf(fmaxf(desc.pcss_min_filter_radius, 0.5f), desc.filter_radius);
	desc.normal_bias_face_normal      = desc.normal_bias_face_normal ? 1 : 0;
	desc.receiver_plane_bias          = fminf(fmaxf(desc.receiver_plane_bias, 0.0f), 2.0f);
	desc.caster_constant_bias         = fminf(fmaxf(desc.caster_constant_bias, 0.0f), 16.0f);
	desc.caster_slope_bias            = fminf(fmaxf(desc.caster_slope_bias, 0.0f), 16.0f);
	for (int axis = 0; axis < 3; axis++) {
		if (!isfinite(desc.world_origin[axis])) {
			desc.world_origin[axis] = 0.0;
		}
	}
	desc.transition_fraction    = fminf(fmaxf(desc.transition_fraction, 0.0f), 0.5f);
	desc.distance_fade_fraction = fminf(fmaxf(desc.distance_fade_fraction, 0.0f), 0.5f);
	desc.debug_atlas            = desc.debug_atlas ? 1 : 0;
	desc.debug_atlas_cascade =
		desc.debug_atlas_cascade < -1
			? -1
			: (desc.debug_atlas_cascade >= (int)desc.cascade_count ? (int)desc.cascade_count - 1
																   : desc.debug_atlas_cascade);
	if (!shadow_ensure_resources(scene, desc.atlas_size, desc.caster_constant_bias, desc.caster_slope_bias)) {
		return 0;
	}

	float       x_axis[3];
	const float reference[3] = {
		0.0f,
		fabsf(light_dir[2]) < 0.9f ? 0.0f : 1.0f,
		fabsf(light_dir[2]) < 0.9f ? 1.0f : 0.0f,
	};
	shadow_cross3(x_axis, reference, light_dir);
	if (!shadow_normalize3(x_axis)) {
		return 0;
	}
	float y_axis[3];
	shadow_cross3(y_axis, light_dir, x_axis);
	shadow_normalize3(y_axis);

	const float camera_near = fmaxf(scene->camera.near_z, 0.001f);
	float       split_edges[AERON_SCENE_SHADOW_MAX_CASCADES + 1];
	split_edges[0] = camera_near;
	if (desc.explicit_splits) {
		for (uint32_t i = 1; i < desc.cascade_count; i++) {
			split_edges[i] = camera_near + (desc.max_distance - camera_near) * desc.split_positions[i - 1];
		}
		split_edges[desc.cascade_count] = desc.max_distance;
	} else {
		for (uint32_t i = 1; i <= desc.cascade_count; i++) {
			const float fraction      = (float)i / (float)desc.cascade_count;
			const float uniform_split = camera_near + (desc.max_distance - camera_near) * fraction;
			const float log_split     = camera_near * powf(desc.max_distance / camera_near, fraction);
			split_edges[i]            = uniform_split + (log_split - uniform_split) * desc.split_lambda;
		}
	}

	float camera_rotation[9];
	AeronSceneInternal_QuatToMat3(scene->camera.ori, camera_rotation);
	for (int axis = 0; axis < 3; axis++) {
		scene->shadow_uniform.camera_pos[axis]     = scene->camera.pos[axis];
		scene->shadow_uniform.camera_forward[axis] = camera_rotation[2 * 3 + axis];
	}
	scene->shadow_uniform.camera_pos[3]     = desc.normal_bias_face_normal ? 1.0f : 0.0f;
	scene->shadow_uniform.camera_forward[3] = desc.filter_radius;
	scene->shadow_uniform.params[0]         = 1.0f;
	scene->shadow_uniform.params[1]         = (float)desc.cascade_count;
	scene->shadow_uniform.params[2]         = (float)desc.filter_quality;
	scene->shadow_uniform.params[3]         = desc.debug_cascades ? 1.0f : 0.0f;
	scene->shadow_uniform.bias[0]           = fmaxf(desc.normal_bias, 0.0f);
	scene->shadow_uniform.bias[1]           = fmaxf(desc.depth_bias_texels, 0.0f);
	scene->shadow_uniform.bias[2]           = fmaxf(desc.slope_bias, 0.0f);
	scene->shadow_uniform.bias[3]           = desc.max_distance;
	scene->shadow_uniform.fade[0]           = desc.max_distance * (1.0f - desc.distance_fade_fraction);
	scene->shadow_uniform.fade[1]           = desc.max_distance;
	scene->shadow_uniform.fade[2]           = 1.0f / (float)desc.atlas_size;
	scene->shadow_uniform.fade[3]           = desc.receiver_plane_bias;
	scene->shadow_uniform.pcss[0] =
		desc.contact_hardening && desc.filter_quality > 0 && desc.light_angular_radius_degrees > 0.0f ? 1.0f
																									  : 0.0f;
	scene->shadow_uniform.pcss[1] =
		tanf(desc.light_angular_radius_degrees * (3.14159265358979323846f / 180.0f));
	scene->shadow_uniform.pcss[2] = desc.max_filter_radius;
	scene->shadow_uniform.pcss[3] = desc.pcss_min_filter_radius;
	/* Keep blocker estimation deterministic. FSR accumulates only the
	 * temporally rotated final PCSS coverage samples. */
	scene->shadow_uniform.pcss_temporal[0] = scene->temporal_active_mode != AERON_TEMPORAL_OFF ? 1.0f : 0.0f;
	scene->shadow_uniform.pcss_temporal[1] = (float)(scene->temporal_phase & 4095u);
	memcpy(scene->shadow_uniform.light_dir, light_dir, sizeof light_dir);

	const uint32_t tile_size      = desc.cascade_count == 1 ? desc.atlas_size : desc.atlas_size / 2;
	const uint32_t usable_size    = tile_size - 2u * AERON_SCENE_SHADOW_GUARD_TEXELS;
	const int      combined_count = scene->instance_count + scene->shadow_only_count;
	ShadowBound    object_bounds[AERON_SCENE_MAX_SHADOW_CASTERS];
	uint16_t       object_indices[AERON_SCENE_MAX_SHADOW_CASTERS];
	uint8_t        bound_valid[AERON_SCENE_MAX_SHADOW_CASTERS];
	uint8_t        caster_valid[AERON_SCENE_MAX_SHADOW_CASTERS];
	memset(bound_valid, 0, sizeof bound_valid);
	memset(caster_valid, 0, sizeof caster_valid);
	scene->shadow_stats.candidate_count = 0;
	for (int candidate = 0; candidate < combined_count; candidate++) {
		const uint16_t encoded =
			candidate < scene->instance_count
				? (uint16_t)candidate
				: (uint16_t)(AERON_SCENE_MAX_INSTANCES + candidate - scene->instance_count);
		const AeronSceneMeshInstance* instance = shadow_caster(scene, encoded);
		if (!instance || !shadow_instance_bound(instance, &object_bounds[candidate])) {
			continue;
		}
		object_indices[candidate] = encoded;
		bound_valid[candidate]    = 1;
		if ((instance->shadow_flags & AERON_SCENE_INSTANCE_NO_CAST_SHADOW) == 0 && instance->mesh &&
			instance->mesh->opaque_index_count != 0) {
			caster_valid[candidate] = 1;
			scene->shadow_stats.candidate_count++;
		}
	}

	for (uint32_t cascade = 0; cascade < desc.cascade_count; cascade++) {
		shadow_prepare_cascade(scene, &desc, light_dir, x_axis, y_axis, split_edges, cascade, usable_size,
							   object_bounds, object_indices, bound_valid, caster_valid, combined_count);
	}
	shadow_prepare_receiver_local(scene, &desc, light_dir, x_axis, y_axis);
	scene->shadow_stats.active              = 1;
	scene->shadow_stats.atlas_size          = desc.atlas_size;
	scene->shadow_stats.cascade_count       = desc.cascade_count;
	scene->shadow_stats.fit_mode            = desc.fit_mode;
	scene->shadow_stats.dropped_shadow_only = (uint32_t)scene->shadow_only_dropped;
	scene->directional_shadow               = desc;
	return 1;
}

static void shadow_draw_list(struct AeronScene3D* scene, AeronRenderPass* pass, const float view_proj[16],
							 const uint16_t* casters, uint16_t caster_count, uint32_t* triangle_count) {
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, scene->mesh_table_buffer);
	const AeronSceneMesh*  bound_mesh     = NULL;
	AeronGraphicsPipeline* bound_pipeline = NULL;
	for (uint16_t item = 0; item < caster_count; item++) {
		const AeronSceneMeshInstance* instance = shadow_caster(scene, casters[item]);
		const AeronSceneMesh*         mesh     = instance ? instance->mesh : NULL;
		if (!mesh || !mesh->vbo || !mesh->ibo || mesh->opaque_index_count == 0) {
			continue;
		}
		AeronCullMode cull =
			instance->cull_mode <= AERON_CULL_BACK ? (AeronCullMode)instance->cull_mode : AERON_CULL_NONE;
		AeronGraphicsPipeline* pipeline = scene->shadow_pipes[cull];
		if (!pipeline) {
			pipeline = scene->shadow_pipes[AERON_CULL_NONE];
		}
		if (pipeline != bound_pipeline) {
			bound_pipeline = pipeline;
			Aeron_BindGraphicsPipeline(pass, pipeline);
		}
		if (mesh != bound_mesh) {
			bound_mesh = mesh;
			Aeron_BindVertexBuffer(pass, 0, mesh->vbo, 0);
			Aeron_BindIndexBuffer(pass, mesh->ibo, AERON_INDEX_FORMAT_UINT16, 0);
		}
		ShadowVsBlock block;
		memset(&block, 0, sizeof block);
		memcpy(block.view_proj, view_proj, sizeof block.view_proj);
		memcpy(block.model_to_world, instance->transform, sizeof block.model_to_world);
		block.mesh_table_index = AeronSceneStorage_ShadowTableIndex(scene, casters[item]);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &block, sizeof block);
		Aeron_DrawIndexed(pass, mesh->opaque_index_count, 0, 0);
		*triangle_count += mesh->opaque_index_count / 3u;
	}
}

int AeronSceneDirectionalShadow_Render(struct AeronScene3D* scene, AeronCommandBuffer* command_buffer) {
	if (!scene || !command_buffer || !scene->storage_ready) {
		return 0;
	}
	if (!scene->shadow_stats.active) {
		return 1;
	}
	if (!scene->shadow_atlas) {
		return 0;
	}
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.depth_target = scene->shadow_atlas,
		.viewport = { 0, 0, (int)scene->shadow_resource_atlas_size, (int)scene->shadow_resource_atlas_size },
		.scissor  = { 0, 0, (int)scene->shadow_resource_atlas_size, (int)scene->shadow_resource_atlas_size },
		.clear_depth       = 1,
		.clear_depth_value = 0.0f,
		.command_buffer    = command_buffer,
		.debug_label       = "Directional shadows",
	});
	if (!pass) {
		return 0;
	}

	const uint32_t cascade_count = scene->directional_shadow.cascade_count;
	const uint32_t tile_size =
		cascade_count == 1 ? scene->shadow_resource_atlas_size : scene->shadow_resource_atlas_size / 2;
	const int usable_size = (int)tile_size - 2 * AERON_SCENE_SHADOW_GUARD_TEXELS;
	for (uint32_t cascade = 0; cascade < cascade_count; cascade++) {
		char label[32];
		snprintf(label, sizeof label, "Cascade %u", cascade);
		Aeron_GpuDebugPush(command_buffer, label);
		const AeronRectI tile = {
			(int)((cascade & 1u) * tile_size) + AERON_SCENE_SHADOW_GUARD_TEXELS,
			(int)((cascade >> 1u) * tile_size) + AERON_SCENE_SHADOW_GUARD_TEXELS,
			usable_size,
			usable_size,
		};
		Aeron_SetViewport(pass, &tile);
		Aeron_SetScissor(pass, &tile);
		shadow_draw_list(scene, pass, scene->shadow_uniform.view_proj[cascade],
						 scene->shadow_casters[cascade], scene->shadow_caster_count[cascade],
						 &scene->shadow_stats.triangle_count[cascade]);
		Aeron_GpuDebugPop(command_buffer);
	}
	Aeron_EndRenderPass(pass);

	if (!scene->shadow_stats.receiver_local_active || !scene->receiver_local_shadow_atlas) {
		return 1;
	}
	const int        receiver_size = AERON_SCENE_RECEIVER_LOCAL_SHADOW_SIZE;
	AeronRenderPass* receiver_pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.depth_target      = scene->receiver_local_shadow_atlas,
		.viewport          = { 0, 0, receiver_size, receiver_size },
		.scissor           = { 0, 0, receiver_size, receiver_size },
		.clear_depth       = 1,
		.clear_depth_value = 0.0f,
		.command_buffer    = command_buffer,
		.debug_label       = "Receiver-local directional shadow",
	});
	if (!receiver_pass) {
		return 0;
	}
	const AeronRectI receiver_viewport = {
		AERON_SCENE_SHADOW_GUARD_TEXELS,
		AERON_SCENE_SHADOW_GUARD_TEXELS,
		receiver_size - 2 * AERON_SCENE_SHADOW_GUARD_TEXELS,
		receiver_size - 2 * AERON_SCENE_SHADOW_GUARD_TEXELS,
	};
	Aeron_SetViewport(receiver_pass, &receiver_viewport);
	Aeron_SetScissor(receiver_pass, &receiver_viewport);
	shadow_draw_list(scene, receiver_pass, scene->receiver_local_shadow_uniform.view_proj[0],
					 scene->receiver_local_shadow_casters, scene->receiver_local_shadow_caster_count,
					 &scene->shadow_stats.receiver_local_triangle_count);
	Aeron_EndRenderPass(receiver_pass);
	return 1;
}

static void shadow_bind_map(struct AeronScene3D* scene, AeronRenderPass* render_pass,
							const AeronSceneDirectionalShadowUniform* active_uniform,
							AeronDepthTarget* active_atlas, int active, uint32_t comparison_slot,
							uint32_t depth_slot, uint32_t uniform_slot) {
	AeronSceneDirectionalShadowUniform        disabled;
	const AeronSceneDirectionalShadowUniform* uniform = active_uniform;
	if (!active) {
		memset(&disabled, 0, sizeof disabled);
		uniform = &disabled;
	}
	Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_FRAGMENT, uniform_slot, uniform,
						  sizeof *uniform);
	Aeron_BindTextureSampler(render_pass, AERON_SHADER_STAGE_FRAGMENT, comparison_slot,
							 Aeron_DepthTargetGetTexture(active ? active_atlas : scene->shadow_fallback),
							 scene->shadow_sampler);
	Aeron_BindTextureSampler(render_pass, AERON_SHADER_STAGE_FRAGMENT, depth_slot,
							 Aeron_DepthTargetGetTexture(active ? active_atlas : scene->shadow_fallback),
							 scene->shadow_depth_sampler);
}

void AeronSceneDirectionalShadow_Bind(struct AeronScene3D* scene, AeronRenderPass* render_pass) {
	if (!scene || !render_pass || !shadow_ensure_common(scene)) {
		return;
	}
	const int active = scene->shadow_stats.active && scene->shadow_atlas;
	shadow_bind_map(scene, render_pass, &scene->shadow_uniform, scene->shadow_atlas, active, 5, 6, 0);
}

void AeronSceneDirectionalShadow_BindForInstance(struct AeronScene3D* scene, AeronRenderPass* render_pass,
												 uint8_t shadow_flags) {
	if (!scene || !render_pass || !shadow_ensure_common(scene)) {
		return;
	}
	const int use_receiver_local = (shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0 &&
								   scene->shadow_stats.active && scene->shadow_stats.receiver_local_active &&
								   scene->receiver_local_shadow_atlas;
	if (use_receiver_local) {
		shadow_bind_map(scene, render_pass, &scene->receiver_local_shadow_uniform,
						scene->receiver_local_shadow_atlas, 1, 5, 6, 0);
	} else {
		const int active = scene->shadow_stats.active && scene->shadow_atlas;
		shadow_bind_map(scene, render_pass, &scene->shadow_uniform, scene->shadow_atlas, active, 5, 6, 0);
	}
}

void AeronSceneDirectionalShadow_BindScreen(struct AeronScene3D* scene, AeronRenderPass* render_pass) {
	if (!scene || !render_pass || !shadow_ensure_common(scene)) {
		return;
	}
	/* Debug views either need per-fragment cascade metadata from the inline
	 * path or replace the output with AO; do not evaluate an unused mask. */
	const int active = scene->shadow_stats.active && scene->shadow_atlas &&
					   !scene->directional_shadow.debug_cascades && !scene->post.ssao_debug_viz;
	shadow_bind_map(scene, render_pass, &scene->shadow_uniform, scene->shadow_atlas, active, 2, 3, 1);
}

void AeronSceneDirectionalShadow_BindDisabled(struct AeronScene3D* scene, AeronRenderPass* render_pass) {
	if (!scene || !render_pass || !shadow_ensure_common(scene)) {
		return;
	}
	AeronSceneDirectionalShadowUniform disabled;
	memset(&disabled, 0, sizeof disabled);
	Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_FRAGMENT, 0, &disabled, sizeof disabled);
	Aeron_BindTextureSampler(render_pass, AERON_SHADER_STAGE_FRAGMENT, 5,
							 Aeron_DepthTargetGetTexture(scene->shadow_fallback), scene->shadow_sampler);
	Aeron_BindTextureSampler(render_pass, AERON_SHADER_STAGE_FRAGMENT, 6,
							 Aeron_DepthTargetGetTexture(scene->shadow_fallback),
							 scene->shadow_depth_sampler);
}

int AeronSceneDirectionalShadow_DebugVisualize(struct AeronScene3D* scene,
											   AeronCommandBuffer*  command_buffer) {
	if (!scene || !command_buffer || !scene->directional_shadow.debug_atlas || !scene->shadow_stats.active ||
		!scene->shadow_atlas || !scene->scene_rt_out || !shadow_ensure_debug_pipeline(scene)) {
		return 0;
	}
	if (!AeronSceneTemporal_EnsureMutableOutput(scene, command_buffer)) {
		return 0;
	}
	AeronTexture* output = Aeron_RenderTargetGetTexture(scene->scene_rt_out);
	AeronTexture* atlas  = Aeron_DepthTargetGetTexture(scene->shadow_atlas);
	if (!output || !atlas) {
		return 0;
	}
	const ShadowDebugUniforms uniforms = {
		.output_size      = { (float)Aeron_TextureGetWidth(output), (float)Aeron_TextureGetHeight(output) },
		.cascade_count    = (float)scene->shadow_stats.cascade_count,
		.selected_cascade = (float)scene->directional_shadow.debug_atlas_cascade,
	};
	return AeronScenePost_Fullscreen(command_buffer, scene->shadow_debug_pipe, scene->scene_rt_out, &atlas,
									 &scene->shadow_depth_sampler, 1, &uniforms, sizeof uniforms,
									 "Directional shadow atlas visualization");
}

void AeronSceneDirectionalShadow_Release(struct AeronScene3D* scene) {
	if (!scene) {
		return;
	}
	for (int cull = 0; cull < 3; cull++) {
		if (scene->shadow_pipes[cull]) {
			Aeron_DestroyGraphicsPipeline(scene->shadow_pipes[cull]);
		}
	}
	if (scene->shadow_vs) {
		Aeron_DestroyShader(scene->shadow_vs);
	}
	if (scene->shadow_fs) {
		Aeron_DestroyShader(scene->shadow_fs);
	}
	if (scene->shadow_debug_pipe) {
		Aeron_DestroyGraphicsPipeline(scene->shadow_debug_pipe);
	}
	if (scene->shadow_debug_vs) {
		Aeron_DestroyShader(scene->shadow_debug_vs);
	}
	if (scene->shadow_debug_fs) {
		Aeron_DestroyShader(scene->shadow_debug_fs);
	}
	if (scene->shadow_sampler) {
		Aeron_DestroySampler(scene->shadow_sampler);
	}
	if (scene->shadow_depth_sampler) {
		Aeron_DestroySampler(scene->shadow_depth_sampler);
	}
	if (scene->shadow_atlas) {
		Aeron_DestroyDepthTarget(scene->shadow_atlas);
	}
	if (scene->receiver_local_shadow_atlas) {
		Aeron_DestroyDepthTarget(scene->receiver_local_shadow_atlas);
	}
	if (scene->shadow_fallback) {
		Aeron_DestroyDepthTarget(scene->shadow_fallback);
	}
}
