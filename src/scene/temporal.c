#include "internal.h"

#include <string.h>

static AeronGraphicsPipeline* temporal_sky_velocity_pipeline(AeronScene3D* s) {
	AeronColorTargetStateDesc targets[3];
	memset(targets, 0, sizeof targets);
	targets[0].format                        = AERON_TEXTURE_FORMAT_R16G16_SNORM;
	targets[0].blend.color_write_mask_enable = 1;
	targets[0].blend.color_write_mask        = 0;
	targets[1].format                        = AERON_TEXTURE_FORMAT_R16G16_FLOAT;
	targets[1].blend.color_write_mask_enable = 1;
	targets[1].blend.color_write_mask        = 0x3;
	targets[2].format                        = AERON_TEXTURE_FORMAT_R32_FLOAT;
	targets[2].blend.color_write_mask_enable = 1;
	targets[2].blend.color_write_mask        = 0;
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader      = s->fullscreen_vs,
		.fragment_shader    = s->temporal_sky_velocity_ps,
		.primitive_type     = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode          = AERON_CULL_NONE,
		.depth_format       = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.color_target_count = 3,
		.color_targets      = targets,
	});
}

static AeronGraphicsPipeline* temporal_copy_pipeline(AeronScene3D* s) {
	const AeronColorTargetStateDesc target = { .format = s->color_format };
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader      = s->fullscreen_vs,
		.fragment_shader    = s->temporal_copy_ps,
		.primitive_type     = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode          = AERON_CULL_NONE,
		.color_target_count = 1,
		.color_targets      = &target,
	});
}

static int temporal_ensure_output_target(AeronScene3D* s) {
	if (s->temporal_output_rt) {
		return 1;
	}
	if (s->temporal_output_allocation_failed) {
		return 0;
	}
	s->temporal_output_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = s->output_w,
		.height     = s->output_h,
		.format     = s->color_format,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.fsr_output",
	});
	if (!s->temporal_output_rt) {
		s->temporal_output_allocation_failed = 1;
		Aeron_LogError("aeron.scene", "failed to allocate the FSR external output target");
		return 0;
	}
	return 1;
}

int AeronSceneTemporal_Ensure(AeronScene3D* s) {
	if (!s || s->temporal_active_mode == AERON_TEMPORAL_OFF) {
		return 0;
	}
	if (s->temporal_tried) {
		return s->temporal_upscaler && s->temporal_depth_rt && s->velocity_rt &&
			   (AeronTemporalUpscaler_UsesDirectHistory(s->temporal_upscaler) || s->temporal_output_rt) &&
			   s->temporal_sky_velocity_pipeline && s->temporal_copy_pipeline &&
			   s->temporal_copy_sampler;
	}
	s->temporal_tried    = 1;
	s->temporal_upscaler = AeronTemporalUpscaler_Create(&(AeronTemporalUpscalerDesc) {
		.max_render_width      = (uint32_t)s->render_w,
		.max_render_height     = (uint32_t)s->render_h,
		.max_output_width      = (uint32_t)s->output_w,
		.max_output_height     = (uint32_t)s->output_h,
		.output_format         = s->color_format,
		.retain_motion_vectors = s->post.mb_fsr_direct_motion,
		.debug_checking        = 1,
	});
	s->temporal_depth_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = s->render_w,
		.height     = s->render_h,
		.format     = AERON_TEXTURE_FORMAT_R32_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ,
		.debug_name = "scene.fsr_depth",
	});
	if (s->temporal_upscaler && !AeronTemporalUpscaler_UsesDirectHistory(s->temporal_upscaler)) {
		temporal_ensure_output_target(s);
	}
	if (s->velocity_rt && (Aeron_TextureGetUsage(Aeron_RenderTargetGetTexture(s->velocity_rt)) &
						   AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) == 0) {
		Aeron_DestroyRenderTarget(s->velocity_rt);
		s->velocity_rt = NULL;
	}
	if (!s->velocity_rt) {
		s->velocity_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = s->render_w,
			.height     = s->render_h,
			.format     = AERON_TEXTURE_FORMAT_R16G16_FLOAT,
			.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ,
			.debug_name = "scene.velocity",
		});
	}
	if (!s->fullscreen_vs) {
		s->fullscreen_vs = AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert",
															AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	}
	s->temporal_sky_velocity_ps =
		AeronSceneInternal_CompileShader("scene_mb_camera_fill.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 1, 0);
	s->temporal_copy_ps = AeronSceneInternal_CompileShader("scene_temporal_copy.frag",
														   AERON_SHADER_STAGE_FRAGMENT, 1, 0, 0);
	if (s->fullscreen_vs && s->temporal_sky_velocity_ps) {
		s->temporal_sky_velocity_pipeline = temporal_sky_velocity_pipeline(s);
	}
	if (s->fullscreen_vs && s->temporal_copy_ps) {
		s->temporal_copy_pipeline = temporal_copy_pipeline(s);
	}
	s->temporal_copy_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	if (!s->temporal_upscaler || !s->temporal_depth_rt || !s->velocity_rt ||
		(!AeronTemporalUpscaler_UsesDirectHistory(s->temporal_upscaler) && !s->temporal_output_rt) ||
		!s->temporal_sky_velocity_pipeline || !s->temporal_copy_pipeline ||
		!s->temporal_copy_sampler) {
		Aeron_LogError("aeron.scene", "FSR 3.1.4 %s initialization failed",
					   AeronTemporal_ModeName(s->temporal_active_mode));
		return 0;
	}
	return 1;
}

void AeronSceneTemporal_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	if (s->scene_rt_out_borrowed || s->scene_rt_out == s->temporal_output_rt) {
		s->scene_rt_out          = s->color_rt;
		s->scene_rt_out_borrowed = 0;
	}
	Aeron_DestroyGraphicsPipeline(s->temporal_sky_velocity_pipeline);
	Aeron_DestroyGraphicsPipeline(s->temporal_copy_pipeline);
	Aeron_DestroyShader(s->temporal_sky_velocity_ps);
	Aeron_DestroyShader(s->temporal_copy_ps);
	Aeron_DestroySampler(s->temporal_copy_sampler);
	Aeron_DestroyRenderTarget(s->temporal_depth_rt);
	Aeron_DestroyRenderTarget(s->temporal_output_rt);
	AeronTemporalUpscaler_Destroy(s->temporal_upscaler);
	s->temporal_sky_velocity_pipeline    = NULL;
	s->temporal_copy_pipeline            = NULL;
	s->temporal_sky_velocity_ps          = NULL;
	s->temporal_copy_ps                  = NULL;
	s->temporal_copy_sampler             = NULL;
	s->temporal_depth_rt                 = NULL;
	s->temporal_output_rt                = NULL;
	s->temporal_output_allocation_failed = 0;
	s->temporal_upscaler                 = NULL;
	s->temporal_tried                    = 0;
}

int AeronSceneTemporal_Dispatch(AeronScene3D* s, AeronCommandBuffer* cmd,
								int update_retained_motion_vectors) {
	if (!s || !cmd || !s->temporal_active) {
		return 0;
	}
	AeronTexture* source        = Aeron_RenderTargetGetTexture(s->scene_rt_out);
	AeronTexture* depth         = Aeron_RenderTargetGetTexture(s->temporal_depth_rt);
	AeronTexture* velocity      = Aeron_RenderTargetGetTexture(s->velocity_rt);
	const int     direct_output = AeronTemporalUpscaler_UsesDirectHistory(s->temporal_upscaler) &&
								  s->temporal.sharpness <= 0.0f && !s->temporal.debug_view;
	if (!direct_output && !temporal_ensure_output_target(s)) {
		return 0;
	}
	AeronTexture*                   output   = Aeron_RenderTargetGetTexture(s->temporal_output_rt);
	const AeronTemporalDispatchDesc dispatch = {
		.command_buffer        = cmd,
		.color                 = source,
		.depth                 = depth,
		.motion_vectors        = velocity,
		.output                = output,
		.render_width          = (uint32_t)s->render_w,
		.render_height         = (uint32_t)s->render_h,
		.output_width          = (uint32_t)s->output_w,
		.output_height         = (uint32_t)s->output_h,
		.jitter_x              = s->temporal_jitter[0],
		.jitter_y              = s->temporal_jitter[1],
		.motion_vector_scale_x = -(float)s->camera.viewport.width,
		.motion_vector_scale_y = -(float)s->camera.viewport.height,
		.frame_time_delta_ms =
			s->temporal.frame_time_delta_ms > 0.0f ? s->temporal.frame_time_delta_ms : 16.6667f,
		.pre_exposure                   = 1.0f,
		.camera_near                    = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f,
		.camera_vertical_fov_radians    = s->camera.v_half_rad * 2.0f,
		.view_space_to_meters           = s->view_space_to_meters,
		.sharpness                      = s->temporal.sharpness,
		.enable_sharpening              = s->temporal.sharpness > 0.0f,
		.reset_history                  = s->temporal.reset_history,
		.debug_view                     = s->temporal.debug_view,
		.update_retained_motion_vectors = update_retained_motion_vectors,
	};
	if (!AeronTemporalUpscaler_Dispatch(s->temporal_upscaler, &dispatch)) {
		Aeron_LogError("aeron.scene", "FSR dispatch failed: %s",
					   AeronTemporalUpscaler_LastError(s->temporal_upscaler));
		return 0;
	}
	AeronRenderTarget* borrowed = AeronTemporalUpscaler_OutputTarget(s->temporal_upscaler);
	if (borrowed) {
		s->scene_rt_out          = borrowed;
		s->scene_rt_out_borrowed = 1;
	} else {
		if (!s->temporal_output_rt) {
			return 0;
		}
		s->scene_rt_out          = s->temporal_output_rt;
		s->scene_rt_out_borrowed = 0;
	}
	return 1;
}

int AeronSceneTemporal_EnsureMutableOutput(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd || !s->scene_rt_out) {
		return 0;
	}
	if (!s->scene_rt_out_borrowed) {
		return 1;
	}
	if (!temporal_ensure_output_target(s) || !s->temporal_copy_pipeline ||
		!s->temporal_copy_sampler) {
		return 0;
	}
	AeronTexture* source = Aeron_RenderTargetGetTexture(s->scene_rt_out);
	if (!AeronScenePost_Fullscreen(cmd, s->temporal_copy_pipeline, s->temporal_output_rt, &source,
								   &s->temporal_copy_sampler, 1, NULL, 0,
								   "Materialize FSR internal history")) {
		return 0;
	}
	s->scene_rt_out          = s->temporal_output_rt;
	s->scene_rt_out_borrowed = 0;
	return 1;
}
