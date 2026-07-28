/*
 * aeron_scene post stack — SSAO chain + motion-blur resolve. The pass
 * topology that sequences these lives in AeronScene_Render
 * (scene3d.c).
 */

#include "internal.h"

#include "aeron/aeron.h"

#include <math.h>
#include <string.h>

/* TileMax/NeighborMax tile size — bounds the maximum blur length. */
#define AERON_SCENE_MB_TILE_SIZE 32

/* ---- cbuffer mirrors (hard-coded by the scene_ssao/scene_mb shaders) ---- */

typedef struct SsaoUniforms {
	float tan_h_half;
	float tan_v_half;
	float near_z;
	float proj_y_offset;
	float radius_view;
	float bias_view;
	float intensity;
	float low_quality;
	float proj_x_offset;
	float min_screen_frac; /* screen-space radius clamp (see AeronScenePostDesc) */
	float max_screen_frac;
	float sample_jitter; /* per-pixel radius jitter [0,1] */
	float view_rot[3][4];
} SsaoUniforms; /* 96 B */

typedef struct SsaoBlurUniforms {
	float direction_uv[2];
	float near_z;
	float _pad0;
	float view_texel_scale[2];
	float _pad1[2];
} SsaoBlurUniforms; /* 32 B */

typedef struct SsaoDebugUniforms {
	float intensity;
	float power;
	float _pad[2];
} SsaoDebugUniforms;

typedef struct MbReconstructUniforms {
	float    shutter_scale;
	float    tap_count;
	float    max_radius;
	float    _pad0;
	uint32_t velocity_size[2];
	uint32_t direct_velocity;
	uint32_t direct_gather;
} MbReconstructUniforms;

typedef struct MbVelocityVizUniforms {
	float    gain;
	uint32_t direct_fsr_motion;
	uint32_t velocity_size[2];
} MbVelocityVizUniforms;

typedef struct MbTemporalVelocityUniforms {
	float source_texel[2];
	float native_resolution;
	float _pad;
} MbTemporalVelocityUniforms;

typedef struct MbTemporalTileMaxUniforms {
	float    source_texel[2];
	uint32_t output_size[2];
	uint32_t native_resolution;
	uint32_t _pad[3];
} MbTemporalTileMaxUniforms;

typedef struct MbTileMaxComputeUniforms {
	uint32_t output_size[2];
	uint32_t _pad[2];
} MbTileMaxComputeUniforms;

typedef struct MbFsrTileMaxUniforms {
	uint32_t render_size[2];
	uint32_t output_size[2];
} MbFsrTileMaxUniforms;

typedef struct MbTileUniforms {
	float src_texel[2];
	float base_scale[2];
	float step_dir[2];
	float _pad[2];
} MbTileUniforms;

/* ---- shared fullscreen pass ---- */

int AeronScenePost_Fullscreen(AeronCommandBuffer* cmd, AeronGraphicsPipeline* pipe, AeronRenderTarget* dst,
							  AeronTexture* const* textures, AeronSampler* const* samplers,
							  uint32_t num_binds, const void* uniform, uint32_t uniform_size,
							  const char* debug_label) {
	if (!cmd || !pipe || !dst) {
		return 0;
	}
	AeronRenderPass* rp = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target   = dst,
		.discard_color  = 1,
		.command_buffer = cmd,
		.debug_label    = debug_label,
	});
	if (!rp) {
		return 0;
	}
	Aeron_BindGraphicsPipeline(rp, pipe);
	for (uint32_t i = 0; i < num_binds; ++i) {
		Aeron_BindTextureSampler(rp, AERON_SHADER_STAGE_FRAGMENT, i, textures[i], samplers[i]);
	}
	if (uniform_size) {
		Aeron_BindUniformData(rp, AERON_SHADER_STAGE_FRAGMENT, 0, uniform, uniform_size);
	}
	Aeron_Draw(rp, 4, 0);
	Aeron_EndRenderPass(rp);
	return 1;
}

/* ---- fullscreen pipeline factory (no vertex input) ---- */

typedef struct PostTarget {
	AeronTextureFormat format;
	uint8_t            write_mask;
} PostTarget;

static AeronGraphicsPipeline* post_pipeline_samples(AeronShader* vs, AeronShader* ps,
													const PostTarget* targets, uint32_t num_targets,
													int has_depth, AeronSampleCount sample_count) {
	AeronColorTargetStateDesc cts[2] = { 0 };
	for (uint32_t i = 0; i < num_targets; ++i) {
		cts[i].format                        = targets[i].format;
		cts[i].blend.color_write_mask_enable = 1;
		cts[i].blend.color_write_mask        = targets[i].write_mask;
	}
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader      = vs,
		.fragment_shader    = ps,
		.primitive_type     = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode          = AERON_CULL_NONE,
		.depth_format       = has_depth ? AERON_TEXTURE_FORMAT_D32_FLOAT : AERON_TEXTURE_FORMAT_UNKNOWN,
		.color_target_count = num_targets,
		.color_targets      = cts,
		.sample_count       = sample_count,
	});
}

static AeronGraphicsPipeline* post_pipeline(AeronShader* vs, AeronShader* ps, const PostTarget* targets,
											uint32_t num_targets, int has_depth) {
	return post_pipeline_samples(vs, ps, targets, num_targets, has_depth, AERON_SAMPLE_COUNT_1);
}

static int ensure_fullscreen_vs(struct AeronScene3D* s) {
	if (!s->fullscreen_vs) {
		s->fullscreen_vs = AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert",
															AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	}
	return s->fullscreen_vs != NULL;
}

/* 4x4 rotation-noise tile: 16 deterministic-LCG 2D vectors in [-1, 1]
 * (Gram-Schmidt helpers for the SSAO tangent-basis spin; also the MB
 * reconstruct's dither source). */
static void ensure_noise_tex(struct AeronScene3D* s) {
	if (s->ssao_noise_tex) {
		return;
	}
	s->ssao_noise_tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = 4,
		.height     = 4,
		.format     = AERON_TEXTURE_FORMAT_R32G32_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "scene.ssao_noise",
	});
	if (!s->ssao_noise_tex) {
		Aeron_RequestFatalRendererError("SSAO noise texture creation");
		return;
	}
	float    noise[16][2];
	uint32_t state = 0x9E3779B9u; /* golden-ratio seed */
	for (int i = 0; i < 16; i++) {
		state       = state * 1103515245u + 12345u;
		uint32_t rx = (state >> 16) & 0x7FFFu;
		state       = state * 1103515245u + 12345u;
		uint32_t ry = (state >> 16) & 0x7FFFu;
		noise[i][0] = (float)rx / 16383.5f - 1.0f;
		noise[i][1] = (float)ry / 16383.5f - 1.0f;
	}
	if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc) {
			.texture  = s->ssao_noise_tex,
			.width    = 4,
			.height   = 4,
			.raw_data = noise,
			.raw_size = sizeof noise,
		})) {
		Aeron_DestroyTexture(s->ssao_noise_tex);
		s->ssao_noise_tex = NULL;
		Aeron_RequestFatalRendererError("SSAO noise texture upload");
	}
}

static int ensure_post_samplers(struct AeronScene3D* s) {
	if (!s->post_point_sampler) {
		/* NEAREST + CLAMP for depth/velocity: depth values must not be
		 * filtered (averaging two depths produces a surface that doesn't
		 * exist). */
		s->post_point_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_NEAREST,
			.mag_filter = AERON_FILTER_NEAREST,
			.mip_filter = AERON_FILTER_NEAREST,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		});
	}
	if (!s->post_linear_sampler) {
		s->post_linear_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_LINEAR,
			.mag_filter = AERON_FILTER_LINEAR,
			.mip_filter = AERON_FILTER_LINEAR,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		});
	}
	return s->post_point_sampler && s->post_linear_sampler;
}

int AeronScenePost_EnsureSsao(struct AeronScene3D* s) {
	if (s->post_ssao_tried) {
		return s->ssao_pipeline && s->ao_rt && s->ao_blur_rt;
	}
	s->post_ssao_tried = 1;
	if (!ensure_fullscreen_vs(s) || !ensure_post_samplers(s)) {
		return 0;
	}

	s->ssao_ps = AeronSceneInternal_CompileShader("scene_ssao.frag", AERON_SHADER_STAGE_FRAGMENT, 4, 2, 0);
	s->ssao_blur_ps =
		AeronSceneInternal_CompileShader("scene_ssao_blur.frag", AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
	s->ssao_blur_lq_ps =
		AeronSceneInternal_CompileShader("scene_ssao_blur_lq.frag", AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
	const PostTarget ao = { AERON_TEXTURE_FORMAT_R8G8_UNORM, 0xF };
	if (s->ssao_ps) {
		s->ssao_pipeline = post_pipeline(s->fullscreen_vs, s->ssao_ps, &ao, 1, 0);
	}
	if (s->ssao_blur_ps) {
		s->ssao_blur_pipeline = post_pipeline(s->fullscreen_vs, s->ssao_blur_ps, &ao, 1, 0);
	}
	if (s->ssao_blur_lq_ps) {
		s->ssao_blur_lq_pipeline = post_pipeline(s->fullscreen_vs, s->ssao_blur_lq_ps, &ao, 1, 0);
	}

	/* Half-res AO/directional-shadow visibility + ping-pong blur target. */
	s->ao_rt_w    = (s->render_w + 1) / 2;
	s->ao_rt_h    = (s->render_h + 1) / 2;
	s->ao_rt      = AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_R8G8_UNORM, s->ao_rt_w, s->ao_rt_h,
													 "scene.visibility");
	s->ao_blur_rt = AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_R8G8_UNORM, s->ao_rt_w, s->ao_rt_h,
													 "scene.visibility_blur");

	if (!s->ssao_pipeline || !s->ao_rt || !s->ao_blur_rt) {
		Aeron_LogError("aeron.scene", "SSAO initialization failed");
		return 0;
	}
	return 1;
}

#ifdef AERON_DEBUG_UI
static int ensure_ssao_debug_pipeline(struct AeronScene3D* s) {
	if (s->post_ssao_debug_tried) {
		return s->ssao_debug_pipeline != NULL;
	}
	s->post_ssao_debug_tried = 1;
	if (!ensure_fullscreen_vs(s) || !ensure_post_samplers(s)) {
		return 0;
	}
	s->ssao_debug_ps =
		AeronSceneInternal_CompileShader("scene_ssao_debug.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	if (s->ssao_debug_ps) {
		const PostTarget color = { s->color_format, 0xF };
		s->ssao_debug_pipeline = post_pipeline(s->fullscreen_vs, s->ssao_debug_ps, &color, 1, 0);
	}
	if (!s->ssao_debug_pipeline) {
		Aeron_LogError("aeron.scene", "SSAO debug visualization initialization failed");
	}
	return s->ssao_debug_pipeline != NULL;
}
#endif

int AeronScenePost_EnsureMb(struct AeronScene3D* s) {
	if (s->post_mb_tried) {
		const int temporal_ok = s->temporal_active_mode == AERON_TEMPORAL_OFF ||
								(s->mb_temporal_velocity_pipeline && s->mb_velocity_viz_output_pipeline);
		const int high_ok =
			s->post.mb_quality != 2 || (s->mb_tilemax_compute_pipeline && s->mb_neighbormax_pipeline &&
										s->mb_tile_rt && s->mb_neighbor_rt &&
										(s->temporal_active_mode == AERON_TEMPORAL_OFF ||
										 (s->mb_temporal_tilemax_pipeline && s->mb_fsr_tilemax_pipeline)));
		return s->mb_reconstruct_pipeline && s->velocity_rt && s->mb_rt && temporal_ok && high_ok;
	}
	s->post_mb_tried = 1;
	if (!ensure_fullscreen_vs(s) || !ensure_post_samplers(s)) {
		return 0;
	}

	s->mb_camera_fill_ps =
		AeronSceneInternal_CompileShader("scene_mb_camera_fill.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 1, 0);
	s->mb_velocity_viz_ps =
		AeronSceneInternal_CompileShader("scene_mb_velocity_viz.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	if (s->temporal_active_mode != AERON_TEMPORAL_OFF) {
		s->mb_temporal_velocity_ps = AeronSceneInternal_CompileShader("scene_mb_temporal_velocity.frag",
																	  AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
		s->mb_temporal_tilemax_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_mb_temporal_tilemax.comp",
			.sampler_count                   = 2,
			.readwrite_storage_texture_count = 2,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = 8,
			.thread_count_y                  = 8,
			.thread_count_z                  = 1,
		});
		s->mb_fsr_tilemax_pipeline      = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_mb_fsr_tilemax.comp",
			.readonly_storage_texture_count  = 1,
			.readwrite_storage_texture_count = 1,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = 8,
			.thread_count_y                  = 8,
			.thread_count_z                  = 1,
		});
	}
	s->mb_tilemax_compute_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name                            = "scene_mb_tilemax.comp",
		.sampler_count                   = 1,
		.readwrite_storage_texture_count = 1,
		.uniform_buffer_count            = 1,
		.thread_count_x                  = 8,
		.thread_count_y                  = 8,
		.thread_count_z                  = 1,
	});
	s->mb_reconstruct_ps =
		AeronSceneInternal_CompileShader("scene_mb_reconstruct.frag", AERON_SHADER_STAGE_FRAGMENT, 4, 1, 0);
	s->mb_neighbormax_ps =
		AeronSceneInternal_CompileShader("scene_mb_neighbormax.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);

	const PostTarget normal_masked = { AERON_TEXTURE_FORMAT_R16G16_SNORM, 0x0 };
	const PostTarget velocity_rw   = { AERON_TEXTURE_FORMAT_R16G16_FLOAT, 0x3 };
	const PostTarget color_rw      = { s->color_format, 0xF };
	const PostTarget tile_rw       = { AERON_TEXTURE_FORMAT_R16G16_FLOAT, 0x3 };
	if (s->mb_camera_fill_ps) {
		const PostTarget fill2[2]  = { normal_masked, velocity_rw };
		s->mb_camera_fill_pipeline = post_pipeline(s->fullscreen_vs, s->mb_camera_fill_ps, fill2, 2, 1);
	}
	if (s->mb_velocity_viz_ps) {
		s->mb_velocity_viz_pipeline =
			post_pipeline_samples(s->fullscreen_vs, s->mb_velocity_viz_ps, &color_rw, 1, 1, s->sample_count);
		s->mb_velocity_viz_output_pipeline =
			post_pipeline(s->fullscreen_vs, s->mb_velocity_viz_ps, &color_rw, 1, 0);
	}
	if (s->mb_temporal_velocity_ps) {
		s->mb_temporal_velocity_pipeline =
			post_pipeline(s->fullscreen_vs, s->mb_temporal_velocity_ps, &velocity_rw, 1, 0);
	}
	if (s->mb_reconstruct_ps) {
		s->mb_reconstruct_pipeline = post_pipeline(s->fullscreen_vs, s->mb_reconstruct_ps, &color_rw, 1, 0);
	}
	if (s->mb_neighbormax_ps) {
		s->mb_neighbormax_pipeline = post_pipeline(s->fullscreen_vs, s->mb_neighbormax_ps, &tile_rw, 1, 0);
	}

	ensure_noise_tex(s);
	if (!s->velocity_rt) {
		s->velocity_rt = AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_R16G16_FLOAT, s->render_w,
														  s->render_h, "scene.velocity");
	}
	if (s->temporal_active_mode != AERON_TEMPORAL_OFF && !s->post.mb_fsr_direct_motion) {
		s->mb_temporal_velocity_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = s->output_w,
			.height     = s->output_h,
			.format     = AERON_TEXTURE_FORMAT_R16G16_FLOAT,
			.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
			.debug_name = "scene.mb_temporal_velocity",
		});
	}
	s->mb_rt =
		AeronSceneInternal_CreateColorRt(s->color_format, s->output_w, s->output_h, "scene.motion_blur");
	s->mb_tile_w      = (s->output_w + AERON_SCENE_MB_TILE_SIZE - 1) / AERON_SCENE_MB_TILE_SIZE;
	s->mb_tile_h      = (s->output_h + AERON_SCENE_MB_TILE_SIZE - 1) / AERON_SCENE_MB_TILE_SIZE;
	s->mb_tile_rt     = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = s->mb_tile_w,
		.height     = s->mb_tile_h,
		.format     = AERON_TEXTURE_FORMAT_R16G16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.mb_tile",
	});
	s->mb_neighbor_rt = AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_R16G16_FLOAT, s->mb_tile_w,
														 s->mb_tile_h, "scene.mb_neighbor");

	const int temporal_ok = s->temporal_active_mode == AERON_TEMPORAL_OFF ||
							(s->mb_temporal_velocity_ps && s->mb_temporal_velocity_pipeline &&
							 s->mb_velocity_viz_output_pipeline);
	const int high_ok =
		s->post.mb_quality != 2 ||
		(s->mb_tilemax_compute_pipeline && s->mb_neighbormax_pipeline && s->mb_tile_rt && s->mb_neighbor_rt &&
		 (s->temporal_active_mode == AERON_TEMPORAL_OFF ||
		  (s->mb_temporal_tilemax_pipeline && s->mb_fsr_tilemax_pipeline)));
	if (!s->mb_reconstruct_pipeline || !s->velocity_rt || !s->mb_rt || !temporal_ok || !high_ok) {
		Aeron_LogError("aeron.scene", "motion-blur initialization failed");
		return 0;
	}
	return 1;
}

void AeronScenePost_Release(struct AeronScene3D* s) {
	if (s->ssao_pipeline)
		Aeron_DestroyGraphicsPipeline(s->ssao_pipeline);
	if (s->ssao_blur_pipeline)
		Aeron_DestroyGraphicsPipeline(s->ssao_blur_pipeline);
	if (s->ssao_blur_lq_pipeline)
		Aeron_DestroyGraphicsPipeline(s->ssao_blur_lq_pipeline);
	if (s->ssao_ps)
		Aeron_DestroyShader(s->ssao_ps);
	if (s->ssao_blur_ps)
		Aeron_DestroyShader(s->ssao_blur_ps);
	if (s->ssao_blur_lq_ps)
		Aeron_DestroyShader(s->ssao_blur_lq_ps);
	if (s->ao_rt)
		Aeron_DestroyRenderTarget(s->ao_rt);
	if (s->ao_blur_rt)
		Aeron_DestroyRenderTarget(s->ao_blur_rt);
	if (s->ssao_noise_tex)
		Aeron_DestroyTexture(s->ssao_noise_tex);
#ifdef AERON_DEBUG_UI
	if (s->ssao_debug_pipeline)
		Aeron_DestroyGraphicsPipeline(s->ssao_debug_pipeline);
	if (s->ssao_debug_ps)
		Aeron_DestroyShader(s->ssao_debug_ps);
#endif
	if (s->mb_camera_fill_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_camera_fill_pipeline);
	if (s->mb_velocity_viz_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_velocity_viz_pipeline);
	if (s->mb_velocity_viz_output_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_velocity_viz_output_pipeline);
	if (s->mb_temporal_velocity_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_temporal_velocity_pipeline);
	if (s->mb_temporal_tilemax_pipeline)
		Aeron_DestroyComputePipeline(s->mb_temporal_tilemax_pipeline);
	if (s->mb_fsr_tilemax_pipeline)
		Aeron_DestroyComputePipeline(s->mb_fsr_tilemax_pipeline);
	if (s->mb_tilemax_compute_pipeline)
		Aeron_DestroyComputePipeline(s->mb_tilemax_compute_pipeline);
	if (s->mb_reconstruct_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_reconstruct_pipeline);
	if (s->mb_neighbormax_pipeline)
		Aeron_DestroyGraphicsPipeline(s->mb_neighbormax_pipeline);
	if (s->mb_camera_fill_ps)
		Aeron_DestroyShader(s->mb_camera_fill_ps);
	if (s->mb_velocity_viz_ps)
		Aeron_DestroyShader(s->mb_velocity_viz_ps);
	if (s->mb_temporal_velocity_ps)
		Aeron_DestroyShader(s->mb_temporal_velocity_ps);
	if (s->mb_reconstruct_ps)
		Aeron_DestroyShader(s->mb_reconstruct_ps);
	if (s->mb_neighbormax_ps)
		Aeron_DestroyShader(s->mb_neighbormax_ps);
	if (s->velocity_rt)
		Aeron_DestroyRenderTarget(s->velocity_rt);
	if (s->mb_temporal_velocity_rt)
		Aeron_DestroyRenderTarget(s->mb_temporal_velocity_rt);
	if (s->mb_rt)
		Aeron_DestroyRenderTarget(s->mb_rt);
	if (s->mb_tile_rt)
		Aeron_DestroyRenderTarget(s->mb_tile_rt);
	if (s->mb_neighbor_rt)
		Aeron_DestroyRenderTarget(s->mb_neighbor_rt);
	if (s->fullscreen_vs)
		Aeron_DestroyShader(s->fullscreen_vs);
	if (s->post_point_sampler)
		Aeron_DestroySampler(s->post_point_sampler);
	if (s->post_linear_sampler)
		Aeron_DestroySampler(s->post_linear_sampler);
	s->post_ssao_tried       = 0;
	s->post_mb_tried         = 0;
	s->fullscreen_vs         = NULL;
	s->post_point_sampler    = NULL;
	s->post_linear_sampler   = NULL;
	s->ssao_ps               = NULL;
	s->ssao_blur_ps          = NULL;
	s->ssao_blur_lq_ps       = NULL;
	s->ssao_pipeline         = NULL;
	s->ssao_blur_pipeline    = NULL;
	s->ssao_blur_lq_pipeline = NULL;
	s->ao_rt                 = NULL;
	s->ao_blur_rt            = NULL;
	s->ao_rt_w               = 0;
	s->ao_rt_h               = 0;
	s->ssao_noise_tex        = NULL;
#ifdef AERON_DEBUG_UI
	s->post_ssao_debug_tried = 0;
	s->ssao_debug_ps         = NULL;
	s->ssao_debug_pipeline   = NULL;
#endif
	s->mb_camera_fill_ps               = NULL;
	s->mb_velocity_viz_ps              = NULL;
	s->mb_temporal_velocity_ps         = NULL;
	s->mb_reconstruct_ps               = NULL;
	s->mb_neighbormax_ps               = NULL;
	s->mb_camera_fill_pipeline         = NULL;
	s->mb_velocity_viz_pipeline        = NULL;
	s->mb_velocity_viz_output_pipeline = NULL;
	s->mb_temporal_velocity_pipeline   = NULL;
	s->mb_temporal_tilemax_pipeline    = NULL;
	s->mb_fsr_tilemax_pipeline         = NULL;
	s->mb_tilemax_compute_pipeline     = NULL;
	s->mb_reconstruct_pipeline         = NULL;
	s->mb_neighbormax_pipeline         = NULL;
	s->velocity_rt                     = NULL;
	s->mb_temporal_velocity_rt         = NULL;
	s->mb_rt                           = NULL;
	s->mb_tile_rt                      = NULL;
	s->mb_neighbor_rt                  = NULL;
	s->mb_tile_w                       = 0;
	s->mb_tile_h                       = 0;
	s->mb_velocity_valid               = 0;
	s->mb_temporal_motion_valid        = 0;
	s->mb_temporal_motion_direct       = 0;
	s->mb_temporal_tile_valid          = 0;
}

/* ===== SSAO compute + blur (from flight_gpu_passes) ================= */

int AeronScenePost_RunSsao(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
	const int low = (s->post.ssao_quality == 1);

	AeronTexture* depth_tex  = Aeron_DepthTargetGetTexture(s->depth_rt);
	AeronTexture* normal_tex = Aeron_RenderTargetGetTexture(s->normal_rt);

	{
		AeronTexture* texs[2] = { depth_tex, normal_tex };
		AeronSampler* smps[2] = { s->post_point_sampler, s->post_point_sampler };
		SsaoUniforms  su      = { 0 };
		su.tan_h_half         = tanf(s->camera.h_half_rad);
		su.tan_v_half         = tanf(s->camera.v_half_rad);
		su.near_z             = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f;
		su.proj_y_offset      = s->camera.proj_y_offset;
		su.proj_x_offset      = s->camera.proj_x_offset;
		if (s->camera.viewport.width > 0 && s->camera.viewport.height > 0) {
			su.proj_x_offset += 2.0f * s->temporal_jitter[0] / (float)s->camera.viewport.width;
			su.proj_y_offset -= 2.0f * s->temporal_jitter[1] / (float)s->camera.viewport.height;
		}
		su.radius_view = s->post.ssao_radius_view;
		su.bias_view   = s->post.ssao_bias_view;
		/* Forward FS scales by its own intensity; kernel scale = 1. */
		su.intensity       = 1.0f;
		su.low_quality     = low ? 1.0f : 0.0f;
		su.min_screen_frac = s->post.ssao_min_screen_frac;
		su.max_screen_frac = s->post.ssao_max_screen_frac;
		su.sample_jitter   = s->post.ssao_sample_jitter;
		float view_rot[9];
		AeronSceneInternal_QuatToMat3(s->camera.ori, view_rot);
		for (int row = 0; row < 3; row++) {
			su.view_rot[row][0] = view_rot[row * 3 + 0];
			su.view_rot[row][1] = view_rot[row * 3 + 1];
			su.view_rot[row][2] = view_rot[row * 3 + 2];
		}
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target   = s->ao_rt,
			.discard_color  = 1,
			.command_buffer = cmd,
			.debug_label    = "AO and directional shadow evaluate",
		});
		if (!pass) {
			return 0;
		}
		Aeron_BindGraphicsPipeline(pass, s->ssao_pipeline);
		for (uint32_t i = 0; i < 2; i++) {
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, i, texs[i], smps[i]);
		}
		AeronSceneDirectionalShadow_BindScreen(s, pass);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, &su, sizeof su);
		Aeron_Draw(pass, 4, 0);
		Aeron_EndRenderPass(pass);
	}

	const float ao_w                = (float)s->ao_rt_w;
	const float ao_h                = (float)s->ao_rt_h;
	const float near_z              = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f;
	const float view_texel_scale[2] = {
		2.0f * tanf(s->camera.h_half_rad) / ao_w,
		2.0f * tanf(s->camera.v_half_rad) / ao_h,
	};

	if (low) {
		/* Two centered five-tap passes: 10 physical samples reconstruct a
		 * depth-aware 5x5 footprint using the existing ping-pong targets. */
		for (int blur_pass = 0; blur_pass < 2; blur_pass++) {
			const int          horizontal = blur_pass == 0;
			AeronRenderTarget* src        = horizontal ? s->ao_rt : s->ao_blur_rt;
			AeronRenderTarget* dst        = horizontal ? s->ao_blur_rt : s->ao_rt;
			AeronTexture*      texs[2]    = { Aeron_RenderTargetGetTexture(src), depth_tex };
			AeronSampler*      smps[2]    = { s->post_point_sampler, s->post_point_sampler };
			SsaoBlurUniforms   bu         = { 0 };
			bu.direction_uv[0]            = horizontal ? 1.0f / ao_w : 0.0f;
			bu.direction_uv[1]            = horizontal ? 0.0f : 1.0f / ao_h;
			bu.near_z                     = near_z;
			bu.view_texel_scale[0]        = view_texel_scale[0];
			bu.view_texel_scale[1]        = view_texel_scale[1];
			if (!AeronScenePost_Fullscreen(cmd, s->ssao_blur_lq_pipeline, dst, texs, smps, 2, &bu,
										   sizeof bu,
										   horizontal ? "SSAO low blur horizontal"
													  : "SSAO low blur vertical")) {
				return 0;
			}
		}
		return 1;
	}

	/* HIGH: separable bilateral blur — H then V. */
	for (int blur_pass = 0; blur_pass < 2; blur_pass++) {
		const int          horizontal = (blur_pass == 0);
		AeronRenderTarget* src        = horizontal ? s->ao_rt : s->ao_blur_rt;
		AeronRenderTarget* dst        = horizontal ? s->ao_blur_rt : s->ao_rt;
		AeronTexture*      texs[2]    = { Aeron_RenderTargetGetTexture(src), depth_tex };
		AeronSampler*      smps[2]    = { s->post_point_sampler, s->post_point_sampler };
		SsaoBlurUniforms   bu         = { 0 };
		bu.direction_uv[0]            = horizontal ? 1.0f / ao_w : 0.0f;
		bu.direction_uv[1]            = horizontal ? 0.0f : 1.0f / ao_h;
		bu.near_z                     = near_z;
		bu.view_texel_scale[0]        = view_texel_scale[0];
		bu.view_texel_scale[1]        = view_texel_scale[1];
		if (!AeronScenePost_Fullscreen(cmd, s->ssao_blur_pipeline, dst, texs, smps, 2, &bu, sizeof bu,
									   horizontal ? "SSAO blur horizontal" : "SSAO blur vertical")) {
			return 0;
		}
	}
	return 1;
}

int AeronScenePost_DebugVisualizeSsao(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
#ifdef AERON_DEBUG_UI
	if (!s || !cmd || !s->post.ssao_debug_viz || !s->ao_rt || !s->scene_rt_out ||
		!ensure_ssao_debug_pipeline(s) || !AeronSceneTemporal_EnsureMutableOutput(s, cmd)) {
		return 0;
	}
	AeronTexture* visibility = Aeron_RenderTargetGetTexture(s->ao_rt);
	if (!visibility) {
		return 0;
	}
	const SsaoDebugUniforms uniforms = {
		.intensity = s->post.ssao_intensity,
		.power     = s->post.ssao_power,
	};
	return AeronScenePost_Fullscreen(cmd, s->ssao_debug_pipeline, s->scene_rt_out, &visibility,
									 &s->post_linear_sampler, 1, &uniforms, sizeof uniforms,
									 "SSAO visualization");
#else
	(void)s;
	(void)cmd;
	return 0;
#endif
}

/* ===== Motion-blur resolve (from flight_gpu_passes) ================= */

static int mb_tile_pass(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronGraphicsPipeline* pipe,
						AeronRenderTarget* dst, AeronRenderTarget* src, const MbTileUniforms* u,
						const char* label) {
	AeronTexture* tex = Aeron_RenderTargetGetTexture(src);
	AeronSampler* smp = s->post_point_sampler;
	return AeronScenePost_Fullscreen(cmd, pipe, dst, &tex, &smp, 1, u, sizeof *u, label);
}

static int mb_ensure_temporal_velocity_target(struct AeronScene3D* s) {
	if (s->mb_temporal_velocity_rt) {
		return 1;
	}
	s->mb_temporal_velocity_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = s->output_w,
		.height     = s->output_h,
		.format     = AERON_TEXTURE_FORMAT_R16G16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.mb_temporal_velocity",
	});
	return s->mb_temporal_velocity_rt != NULL;
}

static int mb_prepare_temporal_velocity_tilemax(struct AeronScene3D* s, AeronCommandBuffer* cmd,
												AeronTexture* depth, AeronTexture* motion) {
	if (!s->mb_temporal_tilemax_pipeline || !s->mb_temporal_velocity_rt || !s->mb_tile_rt) {
		return 0;
	}
	AeronComputeTextureBinding outputs[2] = {
		{ .texture = Aeron_RenderTargetGetTexture(s->mb_temporal_velocity_rt) },
		{ .texture = Aeron_RenderTargetGetTexture(s->mb_tile_rt) },
	};
	AeronComputePass* pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = outputs,
		.write_texture_count = 2,
		.debug_label         = "Motion blur reconstruct velocity + TileMax",
	});
	if (!pass) {
		return 0;
	}
	const MbTemporalTileMaxUniforms uniforms = {
		.source_texel      = { 1.0f / (float)s->render_w, 1.0f / (float)s->render_h },
		.output_size       = { (uint32_t)s->output_w, (uint32_t)s->output_h },
		.native_resolution = s->render_w == s->output_w && s->render_h == s->output_h ? 1u : 0u,
	};
	Aeron_BindComputePipeline(pass, s->mb_temporal_tilemax_pipeline);
	Aeron_BindComputeTextureSampler(pass, 0, depth, s->post_point_sampler);
	Aeron_BindComputeTextureSampler(pass, 1, motion, s->post_point_sampler);
	Aeron_BindComputeUniformData(pass, 0, &uniforms, sizeof uniforms);
	Aeron_DispatchCompute(pass, (uint32_t)s->mb_tile_w, (uint32_t)s->mb_tile_h, 1);
	Aeron_EndComputePass(pass);
	return 1;
}

static int mb_prepare_tilemax(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronTexture* velocity) {
	if (!s->mb_tilemax_compute_pipeline || !s->mb_tile_rt || !velocity) {
		return 0;
	}
	AeronComputeTextureBinding output = {
		.texture = Aeron_RenderTargetGetTexture(s->mb_tile_rt),
	};
	AeronComputePass* pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &output,
		.write_texture_count = 1,
		.debug_label         = "Motion blur TileMax",
	});
	if (!pass) {
		return 0;
	}
	const MbTileMaxComputeUniforms uniforms = {
		.output_size = { (uint32_t)s->output_w, (uint32_t)s->output_h },
	};
	Aeron_BindComputePipeline(pass, s->mb_tilemax_compute_pipeline);
	Aeron_BindComputeTextureSampler(pass, 0, velocity, s->post_point_sampler);
	Aeron_BindComputeUniformData(pass, 0, &uniforms, sizeof uniforms);
	Aeron_DispatchCompute(pass, (uint32_t)s->mb_tile_w, (uint32_t)s->mb_tile_h, 1);
	Aeron_EndComputePass(pass);
	return 1;
}

static int mb_prepare_fsr_tilemax(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronTexture* motion) {
	if (!s->mb_fsr_tilemax_pipeline || !s->mb_tile_rt || !motion) {
		return 0;
	}
	AeronComputeTextureBinding output = {
		.texture = Aeron_RenderTargetGetTexture(s->mb_tile_rt),
	};
	AeronComputePass* pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &output,
		.write_texture_count = 1,
		.debug_label         = "Motion blur direct FSR TileMax",
	});
	if (!pass) {
		return 0;
	}
	const MbFsrTileMaxUniforms uniforms = {
		.render_size = { (uint32_t)s->render_w, (uint32_t)s->render_h },
		.output_size = { (uint32_t)s->output_w, (uint32_t)s->output_h },
	};
	Aeron_BindComputePipeline(pass, s->mb_fsr_tilemax_pipeline);
	Aeron_BindComputeStorageTexture(pass, 0, motion);
	Aeron_BindComputeUniformData(pass, 0, &uniforms, sizeof uniforms);
	Aeron_DispatchCompute(pass, (uint32_t)s->mb_tile_w, (uint32_t)s->mb_tile_h, 1);
	Aeron_EndComputePass(pass);
	return 1;
}

int AeronScenePost_MbPrepareTemporalMotion(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd || !s->temporal_upscaler || s->render_w <= 0 || s->render_h <= 0) {
		return 0;
	}
	s->mb_temporal_tile_valid    = 0;
	s->mb_temporal_motion_direct = 0;
	if (s->post.mb_fsr_direct_motion) {
		AeronTexture* retained = AeronTemporalUpscaler_RetainedDilatedMotionVectors(s->temporal_upscaler);
		if (retained) {
			s->mb_temporal_motion_direct = 1;
			if (s->post.mb_quality != 2 || mb_prepare_fsr_tilemax(s, cmd, retained)) {
				s->mb_temporal_tile_valid = s->post.mb_quality == 2;
				return 1;
			}
			s->mb_temporal_motion_direct = 0;
		}
	}
	if (!mb_ensure_temporal_velocity_target(s)) {
		return 0;
	}
	AeronTexture* depth  = AeronTemporalUpscaler_DilatedDepth(s->temporal_upscaler);
	AeronTexture* motion = AeronTemporalUpscaler_DilatedMotionVectors(s->temporal_upscaler);
	if (!depth || !motion) {
		return 0;
	}
	if (s->post.mb_quality == 2) {
		if (!mb_prepare_temporal_velocity_tilemax(s, cmd, depth, motion)) {
			return 0;
		}
		s->mb_temporal_tile_valid = 1;
		return 1;
	}
	if (!s->mb_temporal_velocity_pipeline) {
		return 0;
	}
	AeronTexture*                    textures[2] = { depth, motion };
	AeronSampler*                    samplers[2] = { s->post_point_sampler, s->post_point_sampler };
	const MbTemporalVelocityUniforms uniforms    = {
		.source_texel      = { 1.0f / (float)s->render_w, 1.0f / (float)s->render_h },
		.native_resolution = s->render_w == s->output_w && s->render_h == s->output_h ? 1.0f : 0.0f,
	};
	return AeronScenePost_Fullscreen(cmd, s->mb_temporal_velocity_pipeline, s->mb_temporal_velocity_rt,
									 textures, samplers, 2, &uniforms, sizeof uniforms,
									 "Motion blur reconstruct temporal velocity");
}

int AeronScenePost_MbVisualizeTemporal(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronTexture* motion,
									   int direct_fsr_motion) {
	if (!s || !cmd || !s->scene_rt_out || !motion || !s->mb_velocity_viz_output_pipeline) {
		return 0;
	}
	AeronSampler*               sampler  = direct_fsr_motion ? s->post_point_sampler : s->post_linear_sampler;
	const MbVelocityVizUniforms uniforms = {
		.gain              = 20.0f,
		.direct_fsr_motion = direct_fsr_motion ? 1u : 0u,
		.velocity_size     = { direct_fsr_motion ? (uint32_t)s->render_w : (uint32_t)s->output_w,
							   direct_fsr_motion ? (uint32_t)s->render_h : (uint32_t)s->output_h },
	};
	return AeronScenePost_Fullscreen(cmd, s->mb_velocity_viz_output_pipeline, s->scene_rt_out, &motion,
									 &sampler, 1, &uniforms, sizeof uniforms,
									 "Motion blur temporal velocity visualization");
}

int AeronScenePost_MbResolve(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderTarget* color,
							 AeronTexture* velocity, int direct_fsr_motion) {
	if (!s || !cmd || !color || !velocity || s->output_w <= 0 || s->output_h <= 0) {
		return 0;
	}
	const int high = (s->post.mb_quality == 2);
	/* Blur-length clamp (UV, height-referenced): High may reach ~3 tiles
	 * (NeighborMax dilates ~1.5), Low stays at one tile. */
	const float max_radius =
		(float)(high ? 3 * AERON_SCENE_MB_TILE_SIZE : AERON_SCENE_MB_TILE_SIZE) / (float)s->output_h;

	AeronTexture* gather = velocity; /* Low: own velocity */
	if (high && s->mb_neighbormax_pipeline && s->mb_tile_rt && s->mb_neighbor_rt) {
		AeronTexture* legacy_temporal =
			s->mb_temporal_velocity_rt ? Aeron_RenderTargetGetTexture(s->mb_temporal_velocity_rt) : NULL;
		const int temporal_motion = direct_fsr_motion || velocity == legacy_temporal;
		int       tile_ready      = temporal_motion && s->mb_temporal_tile_valid;
		if (!tile_ready) {
			const int prepared = direct_fsr_motion ? mb_prepare_fsr_tilemax(s, cmd, velocity)
												   : mb_prepare_tilemax(s, cmd, velocity);
			if (!prepared) {
				return 0;
			}
			tile_ready = 1;
			if (temporal_motion) {
				s->mb_temporal_tile_valid = 1;
			}
		}
		if (tile_ready) {
			MbTileUniforms tn = {
				.src_texel = { 1.0f / (float)s->mb_tile_w, 1.0f / (float)s->mb_tile_h },
			};
			if (!mb_tile_pass(s, cmd, s->mb_neighbormax_pipeline, s->mb_neighbor_rt, s->mb_tile_rt, &tn,
							  "Motion blur NeighborMax")) {
				return 0;
			}
			gather = Aeron_RenderTargetGetTexture(s->mb_neighbor_rt); /* High: dominant motion */
		}
	}

	AeronTexture* texs[4]   = { Aeron_RenderTargetGetTexture(color), velocity, gather, s->ssao_noise_tex };
	AeronSampler* smps[4]   = { s->post_linear_sampler, s->post_linear_sampler, s->post_linear_sampler,
								s->post_point_sampler };
	MbReconstructUniforms u = {
		.shutter_scale   = s->post.mb_shutter,
		.tap_count       = high ? 16.0f : 8.0f,
		.max_radius      = max_radius,
		.velocity_size   = { (uint32_t)Aeron_TextureGetWidth(velocity),
							 (uint32_t)Aeron_TextureGetHeight(velocity) },
		.direct_velocity = direct_fsr_motion ? 1u : 0u,
		.direct_gather   = direct_fsr_motion && !high ? 1u : 0u,
	};
	return AeronScenePost_Fullscreen(cmd, s->mb_reconstruct_pipeline, s->mb_rt, texs, smps, 4, &u, sizeof u,
									 "Motion blur reconstruct");
}
