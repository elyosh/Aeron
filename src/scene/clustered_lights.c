/* Scene-owned clustered-forward point-light allocation. */

#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define AERON_CLUSTER_STATS_WORDS 4u
#define AERON_CLUSTER_FAR_SHRINK_FRAMES 120u

typedef struct ClusterFillUniform {
	uint32_t fill_value;
	uint32_t element_count;
	uint32_t _pad[2];
} ClusterFillUniform;

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

static void cluster_prepare_uniform(AeronScene3D* s) {
	AeronSceneClusterUniformGPU* u = &s->cluster_uniform;
	memset(u, 0, sizeof *u);
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
	u->viewport_x         = (uint32_t)s->camera.viewport.x;
	u->viewport_y         = (uint32_t)s->camera.viewport.y;
	u->viewport_width     = (uint32_t)s->camera.viewport.width;
	u->viewport_height    = (uint32_t)s->camera.viewport.height;
	u->grid_x             = (u->viewport_width + s->cluster_desc.tile_size - 1u) / s->cluster_desc.tile_size;
	u->grid_y             = (u->viewport_height + s->cluster_desc.tile_size - 1u) / s->cluster_desc.tile_size;
	u->grid_z             = s->cluster_desc.depth_slices;
	u->point_light_count  = s->point_light_count;
	u->point_min_distance = s->cluster_desc.min_distance;
	u->point_contribution_cap = s->cluster_desc.contribution_cap;
	u->tile_size              = s->cluster_desc.tile_size;
	u->flags                  = s->cluster_desc.enabled ? AERON_SCENE_CLUSTER_ENABLED : 0u;
	u->flags |= s->cluster_desc.debug_view ? AERON_SCENE_CLUSTER_DEBUG_VIEW : 0u;
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
			.readwrite_storage_buffer_count = 3,
			.uniform_buffer_count           = 1,
			.thread_count_x                 = AERON_SCENE_CLUSTER_THREADS,
			.thread_count_y                 = 1,
			.thread_count_z                 = 1,
		});
		s->cluster_clear_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                           = "compute_fill_buffer.comp",
			.readwrite_storage_buffer_count = 1,
			.uniform_buffer_count           = 1,
			.thread_count_x                 = 64,
			.thread_count_y                 = 1,
			.thread_count_z                 = 1,
		});
	}
	const uint32_t output_usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	return s->cluster_build_pipeline && s->cluster_clear_pipeline &&
		   cluster_ensure_buffer(&s->cluster_header_buffer, &s->cluster_header_buffer_cap,
								 (uint32_t)sizeof(AeronSceneClusterHeaderGPU), output_usage,
								 "scene.cluster_headers") &&
		   cluster_ensure_buffer(&s->cluster_index_buffer, &s->cluster_index_buffer_cap,
								 AERON_SCENE_CLUSTER_MAX_LIGHTS * (uint32_t)sizeof(uint32_t), output_usage,
								 "scene.cluster_indices") &&
		   cluster_ensure_buffer(&s->cluster_stats_buffer, &s->cluster_stats_buffer_cap,
								 AERON_CLUSTER_STATS_WORDS * (uint32_t)sizeof(uint32_t),
								 AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE, "scene.cluster_stats");
}

int AeronSceneClusteredLights_Build(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd || !AeronSceneClusteredLights_Ensure(s)) {
		return 0;
	}
	cluster_prepare_uniform(s);
	const uint64_t cluster_count =
		(uint64_t)s->cluster_uniform.grid_x * s->cluster_uniform.grid_y * s->cluster_uniform.grid_z;
	const uint64_t header_bytes = cluster_count * sizeof(AeronSceneClusterHeaderGPU);
	const uint64_t index_bytes  = cluster_count * AERON_SCENE_CLUSTER_MAX_LIGHTS * sizeof(uint32_t);
	if (cluster_count == 0 || cluster_count > UINT32_MAX || header_bytes > UINT32_MAX ||
		index_bytes > UINT32_MAX) {
		Aeron_LogError("aeron.scene", "cluster grid is too large (%llu clusters)",
					   (unsigned long long)cluster_count);
		return 0;
	}
	const uint32_t output_usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	if (!cluster_ensure_buffer(&s->cluster_header_buffer, &s->cluster_header_buffer_cap,
							   (uint32_t)header_bytes, output_usage, "scene.cluster_headers") ||
		!cluster_ensure_buffer(&s->cluster_index_buffer, &s->cluster_index_buffer_cap, (uint32_t)index_bytes,
							   output_usage, "scene.cluster_indices")) {
		return 0;
	}
	s->cluster_count = (uint32_t)cluster_count;
	s->cluster_ready = 1;
	if (!s->cluster_desc.enabled || s->point_light_count == 0) {
		return 1;
	}
	if (!s->cluster_light_buffer) {
		return 0;
	}

	AeronComputeBufferBinding clear_output = { s->cluster_stats_buffer, 1 };
	AeronComputePass*         clear        = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = &clear_output,
		.write_buffer_count = 1,
		.debug_label        = "Clustered lights stats clear",
	});
	if (!clear) {
		return 0;
	}
	const ClusterFillUniform clear_uniform = { 0, AERON_CLUSTER_STATS_WORDS, { 0, 0 } };
	Aeron_BindComputePipeline(clear, s->cluster_clear_pipeline);
	Aeron_BindComputeUniformData(clear, 0, &clear_uniform, sizeof clear_uniform);
	Aeron_DispatchCompute(clear, 1, 1, 1);
	Aeron_EndComputePass(clear);

	AeronComputeBufferBinding outputs[3] = {
		{ s->cluster_header_buffer, 1 },
		{ s->cluster_index_buffer, 1 },
		{ s->cluster_stats_buffer, 0 },
	};
	AeronComputePass* build = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = outputs,
		.write_buffer_count = 3,
		.debug_label        = "Clustered lights build",
	});
	if (!build) {
		return 0;
	}
	Aeron_BindComputePipeline(build, s->cluster_build_pipeline);
	Aeron_BindComputeStorageBuffer(build, 0, s->cluster_light_buffer);
	Aeron_BindComputeUniformData(build, 0, &s->cluster_uniform, sizeof s->cluster_uniform);
	Aeron_DispatchCompute(build, s->cluster_uniform.grid_x, s->cluster_uniform.grid_y,
						  s->cluster_uniform.grid_z);
	Aeron_EndComputePass(build);
	return 1;
}

void AeronSceneClusteredLights_Bind(AeronScene3D* s, AeronRenderPass* pass) {
	if (!s || !pass || !s->cluster_ready) {
		return;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 3, s->cluster_header_buffer);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 4, s->cluster_index_buffer);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 2, &s->cluster_uniform,
						  sizeof s->cluster_uniform);
}

void AeronSceneClusteredLights_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	Aeron_DestroyComputePipeline(s->cluster_build_pipeline);
	Aeron_DestroyComputePipeline(s->cluster_clear_pipeline);
	Aeron_DestroyBuffer(s->cluster_header_buffer);
	Aeron_DestroyBuffer(s->cluster_index_buffer);
	Aeron_DestroyBuffer(s->cluster_stats_buffer);
	s->cluster_build_pipeline = NULL;
	s->cluster_clear_pipeline = NULL;
	s->cluster_header_buffer  = NULL;
	s->cluster_index_buffer   = NULL;
	s->cluster_stats_buffer   = NULL;
	s->cluster_tried          = 0;
	s->cluster_ready          = 0;
}
