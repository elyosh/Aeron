/* Scene-owned clustered-forward point-light allocation. */

#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define AERON_CLUSTER_FAR_SHRINK_FRAMES 120u
#define AERON_CLUSTER_GLOBAL_SCREEN_COVERAGE 0.25f
#define AERON_CLUSTER_GLOBAL_COUNT_SHIFT 8u
#define AERON_CLUSTER_LOCAL_COUNT_SHIFT 16u

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
		((uint64_t)AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE * viewport_height + denominator / 2u) /
		denominator;
	uint32_t tile_size = (uint32_t)(units * AERON_SCENE_CLUSTER_TILE_ALIGNMENT);
	if (tile_size < AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE)
		tile_size = AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE;
	if (tile_size > AERON_SCENE_CLUSTER_MAX_TILE_SIZE)
		tile_size = AERON_SCENE_CLUSTER_MAX_TILE_SIZE;
	return tile_size;
}

static int cluster_large_screen_light(const AeronScene3D* s, const AeronSceneClusterLightGPU* light) {
	const float* p = light->view_position_range;
	const float  r = p[3];
	if (!(r > 0.0f) || !isfinite(r) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
		return 0;
	}
	const float near_z = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f;
	if (p[2] + r < near_z) {
		return 0;
	}
	const float tan_h = tanf(s->camera.h_half_rad);
	const float tan_v = tanf(s->camera.v_half_rad);
	if (!(tan_h > 0.0f) || !(tan_v > 0.0f)) {
		return 0;
	}
	const float left   = (-1.0f - s->camera.proj_x_offset) * tan_h;
	const float right  = (1.0f - s->camera.proj_x_offset) * tan_h;
	const float top    = (s->camera.proj_y_offset - 1.0f) * tan_v;
	const float bottom = (s->camera.proj_y_offset + 1.0f) * tan_v;
	if (p[0] - left * p[2] < -r * hypotf(1.0f, left) ||
		right * p[2] - p[0] < -r * hypotf(1.0f, right) ||
		p[1] - top * p[2] < -r * hypotf(1.0f, top) ||
		bottom * p[2] - p[1] < -r * hypotf(1.0f, bottom)) {
		return 0;
	}

	const float nearest_z = p[2] - r;
	if (nearest_z <= near_z) {
		return 1;
	}
	const float ndc_left   = (p[0] - r) / (nearest_z * tan_h) + s->camera.proj_x_offset;
	const float ndc_right  = (p[0] + r) / (nearest_z * tan_h) + s->camera.proj_x_offset;
	const float ndc_top    = s->camera.proj_y_offset - (p[1] + r) / (nearest_z * tan_v);
	const float ndc_bottom = s->camera.proj_y_offset - (p[1] - r) / (nearest_z * tan_v);
	const float width      = fmaxf(fminf(ndc_right, 1.0f) - fmaxf(ndc_left, -1.0f), 0.0f);
	const float height     = fmaxf(fminf(ndc_bottom, 1.0f) - fmaxf(ndc_top, -1.0f), 0.0f);
	return width * height * 0.25f >= AERON_CLUSTER_GLOBAL_SCREEN_COVERAGE;
}

void AeronSceneClusteredLights_Classify(AeronScene3D* s) {
	if (!s) {
		return;
	}
	s->cluster_global_count = 0;
	s->cluster_light_count = 0;
	s->cluster_active =
		s->cluster_desc.enabled &&
		(s->cluster_desc.debug_view ||
		 s->point_light_count > AERON_SCENE_CLUSTER_BRUTE_FORCE_MAX_LIGHTS);
	memset(s->cluster_global_indices, 0, sizeof s->cluster_global_indices);
	for (uint32_t i = 0; i < s->point_light_count; ++i) {
		if (s->cluster_active &&
			s->cluster_global_count < AERON_SCENE_CLUSTER_MAX_GLOBAL_LIGHTS &&
			cluster_large_screen_light(s, &s->cluster_light_staging[i])) {
			s->cluster_global_indices[s->cluster_global_count++] = i;
		} else {
			AeronSceneClusterLightGPU light = s->cluster_light_staging[i];
			light.point_light_index = i;
			s->cluster_light_staging[s->cluster_light_count++] = light;
		}
	}
	if (s->point_light_count > 0 && s->cluster_light_count == 0) {
		memset(&s->cluster_light_staging[0], 0, sizeof s->cluster_light_staging[0]);
	}
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
	for (uint32_t i = 0; i < s->cluster_light_count; ++i) {
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
	u->tile_size          = cluster_effective_tile_size(u->viewport_height);
	u->grid_x             = (u->viewport_width + u->tile_size - 1u) / u->tile_size;
	u->grid_y             = (u->viewport_height + u->tile_size - 1u) / u->tile_size;
	u->grid_z             = s->cluster_desc.depth_slices;
	u->point_light_count  = s->point_light_count;
	u->point_min_distance = s->cluster_desc.min_distance;
	u->point_contribution_cap = s->cluster_desc.contribution_cap;
	u->flags                  = s->cluster_active ? AERON_SCENE_CLUSTER_ENABLED : 0u;
	u->flags |= s->cluster_desc.debug_view ? AERON_SCENE_CLUSTER_DEBUG_VIEW : 0u;
	u->flags |= s->cluster_global_count << AERON_CLUSTER_GLOBAL_COUNT_SHIFT;
	u->flags |= s->cluster_light_count << AERON_CLUSTER_LOCAL_COUNT_SHIFT;
	memcpy(u->global_light_indices, s->cluster_global_indices, sizeof u->global_light_indices);
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
			.readwrite_storage_buffer_count = 2,
			.uniform_buffer_count           = 1,
			.thread_count_x                 = AERON_SCENE_CLUSTER_THREADS,
			.thread_count_y                 = 1,
			.thread_count_z                 = 1,
		});
	}
	const uint32_t output_usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	return s->cluster_build_pipeline &&
		   cluster_ensure_buffer(&s->cluster_header_buffer, &s->cluster_header_buffer_cap,
								 (uint32_t)sizeof(AeronSceneClusterHeaderGPU), output_usage,
								 "scene.cluster_headers") &&
		   cluster_ensure_buffer(&s->cluster_index_buffer, &s->cluster_index_buffer_cap,
								 AERON_SCENE_CLUSTER_MAX_LIGHTS * (uint32_t)sizeof(uint32_t), output_usage,
								 "scene.cluster_indices");
}

int AeronSceneClusteredLights_Build(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd || !AeronSceneClusteredLights_Ensure(s)) {
		return 0;
	}
	cluster_prepare_uniform(s);
	const uint64_t cluster_count =
		(uint64_t)s->cluster_uniform.grid_x * s->cluster_uniform.grid_y * s->cluster_uniform.grid_z;
	if (cluster_count == 0 || cluster_count > UINT32_MAX) {
		Aeron_LogError("aeron.scene", "cluster grid is too large (%llu clusters)",
					   (unsigned long long)cluster_count);
		return 0;
	}
	s->cluster_count = (uint32_t)cluster_count;
	if (!s->cluster_active) {
		s->cluster_ready = 1;
		return 1;
	}
	const uint64_t header_bytes = cluster_count * sizeof(AeronSceneClusterHeaderGPU);
	const uint64_t index_bytes  = cluster_count * AERON_SCENE_CLUSTER_MAX_LIGHTS * sizeof(uint32_t);
	if (header_bytes > UINT32_MAX || index_bytes > UINT32_MAX) {
		Aeron_LogError("aeron.scene", "cluster buffers are too large (%llu clusters)",
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
	s->cluster_ready = 1;
	if (!s->cluster_light_buffer) {
		return 0;
	}

	AeronComputeBufferBinding outputs[2] = {
		{ s->cluster_header_buffer, 1 },
		{ s->cluster_index_buffer, 1 },
	};
	AeronComputePass* build = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = outputs,
		.write_buffer_count = 2,
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
	Aeron_DestroyBuffer(s->cluster_header_buffer);
	Aeron_DestroyBuffer(s->cluster_index_buffer);
	s->cluster_build_pipeline = NULL;
	s->cluster_header_buffer  = NULL;
	s->cluster_index_buffer   = NULL;
	s->cluster_global_count  = 0;
	s->cluster_light_count   = 0;
	s->cluster_active        = 0;
	s->cluster_tried          = 0;
	s->cluster_ready          = 0;
}
