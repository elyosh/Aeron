/* Scene-owned clustered-forward point-light allocation. */

#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define AERON_CLUSTER_FAR_SHRINK_FRAMES 120u

static int cluster_ensure_buffer(AeronBuffer** buffer, uint32_t* capacity, uint32_t required, uint32_t usage,
								 const char* name) {
	if (*buffer && *capacity >= required) {
		return 1;
	}
	uint32_t new_capacity = *capacity ? *capacity : 256u;
	while (new_capacity < required) {
		if (new_capacity > UINT32_MAX / 2u) {
			new_capacity = required;
			break;
		}
		new_capacity *= 2u;
	}
	AeronBuffer* replacement = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size         = new_capacity,
		.usage        = usage,
		.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY,
		.debug_name   = name,
	});
	if (!replacement) {
		return 0;
	}
	Aeron_DestroyBuffer(*buffer);
	*buffer   = replacement;
	*capacity = new_capacity;
	return 1;
}

static float cluster_quantized_far(float near_z, float desired) {
	float ratio    = fmaxf(desired / near_z, 2.0f);
	float exponent = ceilf(log2f(ratio));
	if (exponent > 30.0f) {
		exponent = 30.0f;
	}
	return ldexpf(near_z, (int)exponent);
}

static uint32_t cluster_effective_tile_size(uint32_t viewport_height) {
	const uint64_t denominator =
		(uint64_t)AERON_SCENE_CLUSTER_REFERENCE_HEIGHT * AERON_SCENE_CLUSTER_TILE_ALIGNMENT;
	const uint64_t units =
		((uint64_t)AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE * viewport_height + denominator / 2u) / denominator;
	uint32_t tile_size = (uint32_t)(units * AERON_SCENE_CLUSTER_TILE_ALIGNMENT);
	if (tile_size < AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE)
		tile_size = AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE;
	if (tile_size > AERON_SCENE_CLUSTER_MAX_TILE_SIZE)
		tile_size = AERON_SCENE_CLUSTER_MAX_TILE_SIZE;
	return tile_size;
}

static void cluster_prepare_uniform(AeronScene3D* s) {
	AeronSceneClusterUniformGPU* u = &s->cluster_uniform;
	memset(u, 0, sizeof *u);
	s->cluster_active = s->point_light_count > 0;
	memcpy(u->camera_position, s->camera.pos, sizeof u->camera_position);
	u->near_z = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f;
	float view_rotation[9];
	AeronSceneInternal_QuatToMat3(s->camera.ori, view_rotation);
	memcpy(u->camera_forward, &view_rotation[6], sizeof u->camera_forward);

	float desired_far = u->near_z * 2.0f;
	for (uint32_t i = 0; i < s->point_light_count; ++i) {
		const AeronSceneClusterLightGPU* light = &s->cluster_light_staging[i];
		const float                      end = light->view_position_range[2] + light->view_position_range[3];
		if (isfinite(end) && end > desired_far) {
			desired_far = end;
		}
	}
	const float quantized_far = cluster_quantized_far(u->near_z, desired_far);
	const int   near_changed =
		!(s->cluster_near_z > 0.0f) || fabsf(s->cluster_near_z - u->near_z) > u->near_z * 0.001f;
	if (near_changed || !(s->cluster_far_z > u->near_z) || quantized_far > s->cluster_far_z) {
		s->cluster_far_z             = quantized_far;
		s->cluster_far_shrink_frames = 0;
	} else if (quantized_far <= s->cluster_far_z * 0.5f) {
		if (++s->cluster_far_shrink_frames >= AERON_CLUSTER_FAR_SHRINK_FRAMES) {
			s->cluster_far_z             = quantized_far;
			s->cluster_far_shrink_frames = 0;
		}
	} else {
		s->cluster_far_shrink_frames = 0;
	}
	s->cluster_near_z = u->near_z;
	u->slice_scale    = (float)(s->cluster_desc.depth_slices - 1u) / log2f(s->cluster_far_z / u->near_z);
	u->tan_h_half     = tanf(s->camera.h_half_rad);
	u->tan_v_half     = tanf(s->camera.v_half_rad);
	u->proj_x_offset  = s->camera.proj_x_offset;
	u->proj_y_offset  = s->camera.proj_y_offset;
	if (s->camera.viewport.width > 0 && s->camera.viewport.height > 0) {
		u->proj_x_offset += 2.0f * s->temporal_jitter[0] / (float)s->camera.viewport.width;
		u->proj_y_offset -= 2.0f * s->temporal_jitter[1] / (float)s->camera.viewport.height;
	}
	u->viewport_x        = (uint32_t)s->camera.viewport.x;
	u->viewport_y        = (uint32_t)s->camera.viewport.y;
	u->viewport_width    = (uint32_t)s->camera.viewport.width;
	u->viewport_height   = (uint32_t)s->camera.viewport.height;
	u->tile_size         = cluster_effective_tile_size(u->viewport_height);
	u->grid_x            = (u->viewport_width + u->tile_size - 1u) / u->tile_size;
	u->grid_y            = (u->viewport_height + u->tile_size - 1u) / u->tile_size;
	u->grid_z            = s->cluster_desc.depth_slices;
	u->point_light_count = s->point_light_count;
	u->tube_start        = s->point_tube_start;
	u->mask_words        = (s->point_light_count + 31u) / 32u;
	u->flags             = s->cluster_desc.debug_view ? AERON_SCENE_CLUSTER_DEBUG_VIEW : 0u;
}

static uint64_t cluster_data_words(const AeronSceneClusterUniformGPU* u) {
	return (uint64_t)AERON_SCENE_CLUSTER_BIN_WORDS * u->grid_z +
		   (uint64_t)u->grid_x * u->grid_y * u->mask_words;
}

int AeronSceneClusteredLights_Ensure(AeronScene3D* s) {
	if (!s) {
		return 0;
	}
	if (!s->cluster_tried) {
		s->cluster_tried          = 1;
		s->cluster_build_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                           = "scene_clustered_lights.comp",
			.readonly_storage_buffer_count  = 1,
			.readwrite_storage_buffer_count = 1,
			.uniform_buffer_count           = 1,
			.thread_count_x                 = AERON_SCENE_CLUSTER_THREADS,
			.thread_count_y                 = 1,
			.thread_count_z                 = 1,
		});
	}
	const uint32_t output_usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	return s->cluster_build_pipeline &&
		   cluster_ensure_buffer(&s->cluster_data_buffer, &s->cluster_data_buffer_cap,
								 AERON_SCENE_CLUSTER_BIN_WORDS * (uint32_t)sizeof(uint32_t), output_usage,
								 "scene.cluster_data");
}

int AeronSceneClusteredLights_Build(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd || !AeronSceneClusteredLights_Ensure(s)) {
		return 0;
	}
	cluster_prepare_uniform(s);
	const AeronSceneClusterUniformGPU* u          = &s->cluster_uniform;
	const uint64_t                     tile_count = (uint64_t)u->grid_x * u->grid_y;
	s->cluster_count                              = (uint32_t)(tile_count * u->grid_z);
	if (!s->cluster_active) {
		s->cluster_ready = 1;
		return 1;
	}
	const uint64_t data_bytes = cluster_data_words(u) * sizeof(uint32_t);
	if (tile_count == 0 || data_bytes > UINT32_MAX) {
		Aeron_LogError("aeron.scene", "cluster grid is too large (%llu tiles)",
					   (unsigned long long)tile_count);
		return 0;
	}
	const uint32_t output_usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	if (!cluster_ensure_buffer(&s->cluster_data_buffer, &s->cluster_data_buffer_cap, (uint32_t)data_bytes,
							   output_usage, "scene.cluster_data")) {
		return 0;
	}
	s->cluster_ready = 1;
	if (!s->cluster_light_buffer) {
		return 0;
	}

	/* One thread per tile, then one per depth bin. */
	const uint32_t            thread_count = (uint32_t)tile_count + u->grid_z;
	AeronComputeBufferBinding output       = { s->cluster_data_buffer, 1 };
	AeronComputePass*         build        = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = &output,
		.write_buffer_count = 1,
		.debug_label        = "Clustered lights build",
	});
	if (!build) {
		return 0;
	}
	Aeron_BindComputePipeline(build, s->cluster_build_pipeline);
	Aeron_BindComputeStorageBuffer(build, 0, s->cluster_light_buffer);
	Aeron_BindComputeUniformData(build, 0, u, sizeof *u);
	Aeron_DispatchCompute(
		build, (thread_count + AERON_SCENE_CLUSTER_THREADS - 1u) / AERON_SCENE_CLUSTER_THREADS, 1, 1);
	Aeron_EndComputePass(build);
	return 1;
}

void AeronSceneClusteredLights_Bind(AeronScene3D* s, AeronRenderPass* pass) {
	if (!s || !pass || !s->cluster_ready) {
		return;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 3, s->cluster_data_buffer);
	/* Slot 4 is read by no colour shader; SDL wants every declared slot
	 * bound, so it holds the same buffer. */
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 4, s->cluster_data_buffer);
	/* The fragment b2 block is the scene lighting block: the cluster
	 * parameters followed by the image-based lighting environment. */
	AeronSceneLightingUniformGPU lighting;
	memset(&lighting, 0, sizeof lighting);
	/* Kept: probe draws push it again with their set's fields. */
	lighting.cluster = s->cluster_uniform;
	if (s->pbr_environment_set) {
		const AeronScenePbrEnvironment* e = &s->pbr_environment;
		lighting.environment_params[0]    = e->diffuse_strength;
		lighting.environment_params[1]    = e->specular ? e->specular_strength : 0.0f;
		lighting.environment_params[2]    = e->specular ? (float)e->specular_mip_count : 0.0f;
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 3; ++col)
				lighting.environment_basis[row][col] = e->world_to_local[row * 3 + col];
	}
	memcpy(lighting.debug_material, s->pbr_debug_material, sizeof lighting.debug_material);
	lighting.emission[0]    = AeronScene_EmissiveScale();
	lighting.render_size[0] = (float)s->render_w;
	lighting.render_size[1] = (float)s->render_h;
	lighting.render_size[2] = 1.0f / (float)s->render_w;
	lighting.render_size[3] = 1.0f / (float)s->render_h;
	s->lighting_uniform     = lighting;
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 2, &lighting, sizeof lighting);
}

void AeronSceneClusteredLights_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	Aeron_DestroyComputePipeline(s->cluster_build_pipeline);
	Aeron_DestroyBuffer(s->cluster_data_buffer);
	s->cluster_build_pipeline = NULL;
	s->cluster_data_buffer    = NULL;
	s->cluster_active         = 0;
	s->cluster_tried          = 0;
	s->cluster_ready          = 0;
}
