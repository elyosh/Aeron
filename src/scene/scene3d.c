/*
 * AeronScene3D — see aeron/scene/scene3d.h.
 *
 * Camera math uses a reversed-Z perspective projection and quaternion
 * view transform.
 */

#include "internal.h"

#include "aeron/scene/environment.h"

#include <SDL3/SDL.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char AeronScenePointLightGPUSizeCheck[sizeof(AeronScenePointLightGPU) == 64 ? 1 : -1];
typedef char AeronSceneClusterLightGPUSizeCheck[sizeof(AeronSceneClusterLightGPU) == 48 ? 1 : -1];
typedef char AeronSceneClusterUniformGPUSizeCheck[sizeof(AeronSceneClusterUniformGPU) == 96 ? 1 : -1];

/* struct AeronScene3D lives in internal.h (shared with
 * the material-class translation units). */

/* ---- camera math (flight_gpu_math transplant) ---- */

static void scene_quat_to_mat3(const float q[4], float m[9]) {
	const float w = q[0], x = q[1], y = q[2], z = q[3];
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;
	m[0] = 1.0f - 2.0f * (yy + zz);
	m[1] = 2.0f * (xy - wz);
	m[2] = 2.0f * (xz + wy);
	m[3] = 2.0f * (xy + wz);
	m[4] = 1.0f - 2.0f * (xx + zz);
	m[5] = 2.0f * (yz - wx);
	m[6] = 2.0f * (xz - wy);
	m[7] = 2.0f * (yz + wx);
	m[8] = 1.0f - 2.0f * (xx + yy);
}

static void scene_mat4_perspective_reverse_z_xy(float m[16], float h_half, float v_half, float near_z,
												float proj_x_offset, float proj_y_offset) {
	memset(m, 0, sizeof(float) * 16);
	m[0] = 1.0f / tanf(h_half);
	m[2] = proj_x_offset;
	/* Negated for the engine eye-y-down convention. */
	m[5]  = -1.0f / tanf(v_half);
	m[6]  = proj_y_offset;
	m[11] = near_z;
	m[14] = 1.0f;
}

static void scene_jittered_projection_offsets(const AeronScene3D* s, float* offset_x, float* offset_y) {
	*offset_x = s->camera.proj_x_offset;
	*offset_y = s->camera.proj_y_offset;
	if (s->camera.viewport.width > 0 && s->camera.viewport.height > 0) {
		*offset_x += 2.0f * s->temporal_jitter[0] / (float)s->camera.viewport.width;
		*offset_y -= 2.0f * s->temporal_jitter[1] / (float)s->camera.viewport.height;
	}
}

/* Row-major view: rotate by the world->eye quat, translate by -R*pos. */
static void scene_mat4_view(float m[16], const float ori[4], const float pos[3]) {
	float r[9];
	scene_quat_to_mat3(ori, r);
	memset(m, 0, sizeof(float) * 16);
	for (int row = 0; row < 3; ++row) {
		m[row * 4 + 0] = r[row * 3 + 0];
		m[row * 4 + 1] = r[row * 3 + 1];
		m[row * 4 + 2] = r[row * 3 + 2];
		m[row * 4 + 3] = -(r[row * 3 + 0] * pos[0] + r[row * 3 + 1] * pos[1] + r[row * 3 + 2] * pos[2]);
	}
	m[15] = 1.0f;
}

static void scene_mat4_mul(float out[16], const float a[16], const float b[16]) {
	float t[16];
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 4; ++col)
			t[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
							   a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
	memcpy(out, t, sizeof t);
}

/* ---- lifecycle ---- */

static int scene_temporal_mode_valid(AeronTemporalMode mode) {
	return mode >= AERON_TEMPORAL_OFF && mode <= AERON_TEMPORAL_PERFORMANCE;
}

static int scene_calculate_render_dims(AeronScene3D* s, AeronTemporalMode mode, int* width, int* height) {
	uint32_t render_width;
	uint32_t render_height;
	if (!scene_temporal_mode_valid(mode) ||
		!AeronTemporal_GetRenderResolution(mode, (uint32_t)s->output_w, (uint32_t)s->output_h, &render_width,
										   &render_height) ||
		render_width == 0 || render_height == 0 || render_width > (uint32_t)s->output_w ||
		render_height > (uint32_t)s->output_h) {
		return 0;
	}
	*width  = (int)render_width;
	*height = (int)render_height;
	return 1;
}

static float scene_mode_mip_lod_bias(AeronScene3D* s, AeronTemporalMode mode) {
	int render_w;
	int render_h;
	if (!scene_calculate_render_dims(s, mode, &render_w, &render_h)) {
		return 0.0f;
	}
	return AeronTemporal_GetMipLodBias(mode, (uint32_t)render_w, (uint32_t)s->output_w);
}

static AeronSamplerDesc scene_world_sampler_desc(float mip_lod_bias) {
	return (AeronSamplerDesc) {
		.min_filter   = AERON_FILTER_LINEAR,
		.mag_filter   = AERON_FILTER_LINEAR,
		.mip_filter   = AERON_FILTER_LINEAR,
		.address_u    = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v    = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w    = AERON_ADDRESS_CLAMP_TO_EDGE,
		.mip_lod_bias = mip_lod_bias,
		.max_lod      = 1000.0f,
	};
}

static void scene_release_sampler_set(AeronSampler* samplers[AERON_TEMPORAL_PERFORMANCE + 1]) {
	for (int mode = AERON_TEMPORAL_OFF; mode <= AERON_TEMPORAL_PERFORMANCE; ++mode) {
		Aeron_DestroySampler(samplers[mode]);
		samplers[mode] = NULL;
	}
}

/* Model atlas samplers: `base` (the game's mesh sampler, or the world
 * sampler) with each mode's mip bias and REPEAT addressing. A dedicated
 * atlas layer is a whole texture that tiles; a packed sub-rect never
 * samples past its own REPEAT-filled footprint. */
static int scene_create_atlas_samplers(AeronScene3D* s, const AeronSamplerDesc* base,
									   AeronSampler* out[AERON_TEMPORAL_PERFORMANCE + 1]) {
	for (int mode = AERON_TEMPORAL_OFF; mode <= AERON_TEMPORAL_PERFORMANCE; ++mode) {
		AeronSamplerDesc desc = *base;
		desc.mip_lod_bias += scene_mode_mip_lod_bias(s, (AeronTemporalMode)mode);
		desc.address_u = desc.address_v = desc.address_w = AERON_ADDRESS_REPEAT;
		out[mode]                                        = Aeron_CreateSampler(&desc);
		if (!out[mode]) {
			scene_release_sampler_set(out);
			return 0;
		}
	}
	return 1;
}

static int scene_create_world_samplers(AeronScene3D* s) {
	for (int mode = AERON_TEMPORAL_OFF; mode <= AERON_TEMPORAL_PERFORMANCE; ++mode) {
		const AeronSamplerDesc desc = scene_world_sampler_desc(scene_mode_mip_lod_bias(s, mode));
		s->pbr_samplers[mode]       = Aeron_CreateSampler(&desc);
		if (!s->pbr_samplers[mode]) {
			scene_release_sampler_set(s->pbr_samplers);
			return 0;
		}
	}
	const AeronSamplerDesc atlas_base = scene_world_sampler_desc(0.0f);
	if (!scene_create_atlas_samplers(s, &atlas_base, s->mesh_samplers)) {
		scene_release_sampler_set(s->pbr_samplers);
		return 0;
	}
	return 1;
}

static void scene_select_world_samplers(AeronScene3D* s) {
	const int mode =
		scene_temporal_mode_valid(s->temporal_active_mode) ? s->temporal_active_mode : AERON_TEMPORAL_OFF;
	s->pbr_sampler  = s->pbr_samplers[mode];
	s->mesh_sampler = s->mesh_samplers[mode];
}

static void scene_release_targets(AeronScene3D* s) {
	Aeron_DestroyRenderTarget(s->screen_layer_msaa_rt);
	Aeron_DestroyRenderTarget(s->screen_layer_rt);
	s->screen_layer_msaa_rt = NULL;
	s->screen_layer_rt      = NULL;
	s->screen_layer_active  = 0;
	Aeron_DestroyDepthTarget(s->msaa_depth_rt);
	Aeron_DestroyRenderTarget(s->msaa_color_rt);
	Aeron_DestroyRenderTarget(s->normal_rt);
	Aeron_DestroyDepthTarget(s->depth_rt);
	Aeron_DestroyRenderTarget(s->color_rt);
	s->msaa_depth_rt = NULL;
	s->msaa_color_rt = NULL;
	s->normal_rt     = NULL;
	s->depth_rt      = NULL;
	s->color_rt      = NULL;
}

static int scene_create_targets(AeronScene3D* s) {
	s->color_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = s->render_w,
		.height     = s->render_h,
		.format     = s->color_format,
		.usage      = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ,
		.debug_name = "scene.color",
	});
	s->depth_rt = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
		.width      = s->render_w,
		.height     = s->render_h,
		.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.sampled    = 1,
		.debug_name = "scene.depth",
	});
	if (s->with_normal_rt) {
		s->normal_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = s->render_w,
			.height     = s->render_h,
			.format     = AERON_TEXTURE_FORMAT_R16G16_SNORM,
			.debug_name = "scene.normal",
		});
	}
	if (s->sample_count != AERON_SAMPLE_COUNT_1) {
		s->msaa_color_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width        = s->render_w,
			.height       = s->render_h,
			.format       = s->color_format,
			.sample_count = s->sample_count,
			.debug_name   = "scene.msaa_color",
		});
		s->msaa_depth_rt = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
			.width        = s->render_w,
			.height       = s->render_h,
			.format       = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.sample_count = s->sample_count,
			.debug_name   = "scene.msaa_depth",
		});
	}
	if (!s->color_rt || !s->depth_rt || (s->with_normal_rt && !s->normal_rt) ||
		(s->sample_count != AERON_SAMPLE_COUNT_1 && (!s->msaa_color_rt || !s->msaa_depth_rt))) {
		scene_release_targets(s);
		return 0;
	}
	return 1;
}

static AeronSampleCount scene_supported_sample_count(AeronTextureFormat color_format,
													 AeronSampleCount   requested) {
	static const AeronSampleCount counts[] = {
		AERON_SAMPLE_COUNT_8,
		AERON_SAMPLE_COUNT_4,
		AERON_SAMPLE_COUNT_2,
		AERON_SAMPLE_COUNT_1,
	};
	if (requested != AERON_SAMPLE_COUNT_2 && requested != AERON_SAMPLE_COUNT_4 &&
		requested != AERON_SAMPLE_COUNT_8) {
		requested = AERON_SAMPLE_COUNT_1;
	}
	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
		if (counts[i] > requested) {
			continue;
		}
		if (Aeron_TextureFormatSupportsSampleCount(color_format, counts[i]) &&
			Aeron_TextureFormatSupportsSampleCount(AERON_TEXTURE_FORMAT_D32_FLOAT, counts[i])) {
			return counts[i];
		}
	}
	return AERON_SAMPLE_COUNT_1;
}

/* Whether an instance draws its screens into the screen layer this frame.
 * The layer's targets are made the first time; without them, the screens
 * stay in the scene. */
static int scene_screen_layer_wanted(AeronScene3D* s) {
	int wanted = 0;
	for (int i = 0; i < s->instance_count && !wanted; ++i) {
		const AeronSceneMesh* m = s->instances[i].mesh;
		wanted                  = s->instances[i].screens_to_layer && m && m->blend_screen_index_count != 0;
	}
	if (!wanted) {
		return 0;
	}
	if (!s->screen_layer_rt) {
		s->screen_layer_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = s->render_w,
			.height     = s->render_h,
			.format     = s->color_format,
			.debug_name = "scene.screen_layer",
		});
	}
	if (s->sample_count != AERON_SAMPLE_COUNT_1 && !s->screen_layer_msaa_rt) {
		s->screen_layer_msaa_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width        = s->render_w,
			.height       = s->render_h,
			.format       = s->color_format,
			.sample_count = s->sample_count,
			.debug_name   = "scene.screen_layer_msaa",
		});
	}
	if (!s->screen_layer_rt || (s->sample_count != AERON_SAMPLE_COUNT_1 && !s->screen_layer_msaa_rt)) {
		if (!s->screen_layer_warned) {
			Aeron_LogWarn("aeron.scene", "the screen layer can't be created; the screens stay in the scene");
			s->screen_layer_warned = 1;
		}
		return 0;
	}
	return 1;
}

/* Draws the screens of the `screens_to_layer` instances into the screen
 * layer, against the depth of the colour pass. `pipe_kind` is the kind the
 * colour pass drew its transparent ranges with. */
static int scene_draw_screen_layer(AeronScene3D* s, AeronCommandBuffer* cmd, int pipe_kind) {
	const int        msaa = s->sample_count != AERON_SAMPLE_COUNT_1;
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target         = msaa ? s->screen_layer_msaa_rt : s->screen_layer_rt,
		.color_resolve_target = msaa ? s->screen_layer_rt : NULL,
		.depth_target         = msaa ? s->msaa_depth_rt : s->depth_rt,
		.viewport             = s->camera.viewport,
		.clear_color          = 1,
		.clear_color_rgba     = { 0.0f, 0.0f, 0.0f, 0.0f },
		/* The depth is loaded, so it is stored too: a discarded depth target
		 * is cycled, which SDL doesn't allow with a load. */
		.command_buffer = cmd,
		.debug_label    = "Scene screen layer",
	});
	if (!pass) {
		return 0;
	}
	AeronSceneInternal_BindFrameUniforms(s, pass);
	const int ok = AeronScenePbr_DrawTransparentInstances(s, pass, pipe_kind, NULL, /*layer=*/1);
	Aeron_EndRenderPass(pass);
	return ok;
}

static int scene_reconfigure(AeronScene3D* s, AeronTemporalMode mode) {
	int render_w;
	int render_h;
	if (!scene_calculate_render_dims(s, mode, &render_w, &render_h)) {
		return 0;
	}
	AeronSceneTemporal_Release(s);
	AeronScenePost_Release(s);
	scene_release_targets(s);
	s->render_w             = render_w;
	s->render_h             = render_h;
	s->temporal_active_mode = mode;
	scene_select_world_samplers(s);
	s->temporal_phase         = 0;
	s->temporal.reset_history = 1;
	return scene_create_targets(s);
}

static int scene_prepare_mode(AeronScene3D* s) {
	AeronTemporalMode requested =
		scene_temporal_mode_valid(s->temporal.mode) ? s->temporal.mode : AERON_TEMPORAL_OFF;
	if (requested != s->temporal_active_mode && !scene_reconfigure(s, requested)) {
		Aeron_LogError("aeron.scene", "failed to configure temporal mode %s",
					   AeronTemporal_ModeName(requested));
		return 0;
	}
	if (requested != AERON_TEMPORAL_OFF && !AeronSceneTemporal_Ensure(s)) {
		Aeron_LogError("aeron.scene", "temporal upscaling initialization failed");
		return 0;
	}
	if (requested != AERON_TEMPORAL_OFF &&
		(!s->normal_rt || !AeronScenePbr_Ensure(s) ||
		 !AeronScenePbr_Pipeline(s, AERON_PBR_PIPE_PREPASS_TEMPORAL, AERON_CULL_NONE))) {
		Aeron_LogError("aeron.scene", "temporal velocity rendering is unavailable");
		return 0;
	}
	return s->color_rt && s->depth_rt;
}

static int scene_scale_edge(int edge, int render_extent, int output_extent, int round_up) {
	const int64_t scaled = (int64_t)edge * (int64_t)render_extent;
	return (int)((scaled + (round_up ? output_extent - 1 : 0)) / output_extent);
}

static AeronRectI scene_render_viewport(const AeronScene3D* s, AeronRectI output) {
	const int left   = scene_scale_edge(output.x, s->render_w, s->output_w, 0);
	const int top    = scene_scale_edge(output.y, s->render_h, s->output_h, 0);
	const int right  = scene_scale_edge(output.x + output.width, s->render_w, s->output_w, 1);
	const int bottom = scene_scale_edge(output.y + output.height, s->render_h, s->output_h, 1);
	return (AeronRectI) { left, top, right - left, bottom - top };
}

AeronScene3D* AeronScene_Create(const AeronScene3DDesc* desc) {
	AeronScene3D* s = (AeronScene3D*)calloc(1, sizeof *s);
	if (!s) {
		return NULL;
	}
	s->output_w     = desc && desc->rt_width > 0 ? desc->rt_width : 3840;
	s->output_h     = desc && desc->rt_height > 0 ? desc->rt_height : 2160;
	s->color_format = desc && desc->color_format ? desc->color_format : AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT;
	AeronSampleCount requested_samples =
		desc && desc->sample_count ? desc->sample_count : AERON_SAMPLE_COUNT_1;
	s->sample_count = scene_supported_sample_count(s->color_format, requested_samples);
	if (s->sample_count != requested_samples) {
		Aeron_LogWarn("aeron.scene", "requested %dx MSAA unsupported for scene color/depth; using %dx",
					  (int)requested_samples, (int)s->sample_count);
	}
	/* MSAA keeps a single-sample prepass for post effects and lens occlusion. */
	s->with_normal_rt       = (desc && desc->with_normal_rt) || s->sample_count != AERON_SAMPLE_COUNT_1;
	s->view_space_to_meters = desc && desc->view_space_to_meters > 0.0f ? desc->view_space_to_meters : 1.0f;
	s->backface_fill        = 1;
	s->temporal.mode =
		desc && scene_temporal_mode_valid(desc->temporal_mode) ? desc->temporal_mode : AERON_TEMPORAL_OFF;
	if (s->sample_count != AERON_SAMPLE_COUNT_1 && s->temporal.mode != AERON_TEMPORAL_OFF) {
		Aeron_LogWarn("aeron.scene", "temporal upscaling disabled while MSAA is active");
		s->temporal.mode = AERON_TEMPORAL_OFF;
	}
	s->temporal.sharpness = desc ? desc->temporal_sharpness : 0.0f;
	if (s->temporal.sharpness < 0.0f)
		s->temporal.sharpness = 0.0f;
	if (s->temporal.sharpness > 1.0f)
		s->temporal.sharpness = 1.0f;
	s->temporal.reset_history = 1;
	s->temporal_active_mode   = s->temporal.mode;
	s->exposure               = 1.0f;
	s->image_radiance         = 1.0f;
	if (!scene_calculate_render_dims(s, s->temporal_active_mode, &s->render_w, &s->render_h)) {
		free(s);
		return NULL;
	}
	s->clear_rgba[0]             = 0.01f; /* deep-space baseline */
	s->clear_rgba[1]             = 0.015f;
	s->clear_rgba[2]             = 0.03f;
	s->clear_rgba[3]             = 1.0f;
	s->cluster_desc.depth_slices = AERON_SCENE_CLUSTER_DEFAULT_DEPTH_SLICES;

	if (!scene_create_targets(s)) {
		AeronScene_Destroy(s);
		return NULL;
	}
	/* World samplers carry AMD's per-mode mip bias. The atlas shaders handle
	 * UV wrapping through their sub-rect transform and frac operation; their
	 * samplers repeat so dedicated atlas layers tile. */
	if (!scene_create_world_samplers(s)) {
		AeronScene_Destroy(s);
		return NULL;
	}
	scene_select_world_samplers(s);
	return s;
}

static void scene_free_instances(AeronScene3D* s);

void AeronScene_Destroy(AeronScene3D* s) {
	if (!s) {
		return;
	}
	AeronSceneBb3d_Release(s);
	AeronSceneLensFlare_Release(s);
	scene_free_instances(s);
	AeronSceneMeshOverlay_Release(s);
	AeronSceneDirectionalShadow_Release(s);
	AeronSceneClusteredLights_Release(s);
	AeronSceneProbeOcclusion_Release(s);
	AeronScenePlanarReflection_Release(s);
	AeronSceneStorage_Release(s);
	AeronScenePbr_Release(s);
	AeronSceneTemporal_Release(s);
	AeronSceneExposure_Release(s);
	AeronScenePost_Release(s);
	if (s->sky_sun_pipe) {
		Aeron_DestroyGraphicsPipeline(s->sky_sun_pipe);
	}
	if (s->sky_sun_vs) {
		Aeron_DestroyShader(s->sky_sun_vs);
	}
	if (s->sky_sun_fs) {
		Aeron_DestroyShader(s->sky_sun_fs);
	}
	if (s->sky_overlay_pipe) {
		Aeron_DestroyGraphicsPipeline(s->sky_overlay_pipe);
	}
	if (s->sky_overlay_fs) {
		Aeron_DestroyShader(s->sky_overlay_fs);
	}
	if (s->sky_pipe) {
		Aeron_DestroyGraphicsPipeline(s->sky_pipe);
	}
	if (s->sky_vs) {
		Aeron_DestroyShader(s->sky_vs);
	}
	if (s->sky_fs) {
		Aeron_DestroyShader(s->sky_fs);
	}
	scene_release_sampler_set(s->mesh_samplers);
	scene_release_sampler_set(s->pbr_samplers);
	scene_release_targets(s);
	free(s);
}

int AeronScene_PrepareMesh(AeronScene3D* s, const AeronSceneMesh* mesh, AeronCullMode cull) {
	if (!s || !mesh || (unsigned)cull > AERON_CULL_BACK || !AeronScenePbr_Ensure(s))
		return 0;
	return AeronScenePbr_PrepareMesh(s, mesh, cull);
}

int AeronScene_PrepareResources(AeronScene3D* s, AeronCullMode mesh_cull) {
	if (!s || (unsigned)mesh_cull > AERON_CULL_BACK || !AeronScenePbr_Ensure(s) ||
		!AeronSceneBb3d_Ensure(s) || !AeronSceneClusteredLights_Ensure(s))
		return 0;
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; ++kind)
		if (!AeronScenePbr_Pipeline(s, kind, mesh_cull))
			return 0;
	if (s->post.ssao_quality > 0 && s->post.ssao_intensity > 0 && !AeronScenePost_EnsureSsao(s))
		return 0;
	if (s->post.mb_quality > 0 && !AeronScenePost_EnsureMb(s))
		return 0;
	if (s->temporal_active_mode != AERON_TEMPORAL_OFF && !AeronSceneTemporal_Ensure(s))
		return 0;
	return 1;
}

/* ---- per-frame ---- */

int AeronScene_Begin(AeronScene3D* s, const AeronSceneCamera* camera) {
	if (!s || !camera || !scene_prepare_mode(s)) {
		return 0;
	}
	/* Snapshot the previous frame's orientation before overwriting the
	 * camera; the motion-blur camera fill uses it to skip itself when the
	 * camera barely rotated (its output would be sub-pixel). */
	memcpy(s->mb_prev_ori, s->camera.ori, sizeof s->mb_prev_ori);
	s->output_camera = *camera;
	if (s->output_camera.viewport.width <= 0 || s->output_camera.viewport.height <= 0) {
		s->output_camera.viewport = (AeronRectI) { 0, 0, s->output_w, s->output_h };
	}
	s->camera          = s->output_camera;
	s->camera.viewport = scene_render_viewport(s, s->output_camera.viewport);
	float proj[16];
	float view[16];
	scene_mat4_perspective_reverse_z_xy(proj, s->camera.h_half_rad, s->camera.v_half_rad,
										s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f,
										s->camera.proj_x_offset, s->camera.proj_y_offset);
	scene_mat4_view(view, s->camera.ori, s->camera.pos);
	scene_mat4_mul(s->unjittered_view_proj, proj, view);
	s->temporal_jitter[0] = 0.0f;
	s->temporal_jitter[1] = 0.0f;
	if (s->temporal_active_mode != AERON_TEMPORAL_OFF &&
		AeronTemporal_GetJitter(s->temporal_phase, (uint32_t)s->camera.viewport.width,
								(uint32_t)s->output_camera.viewport.width, &s->temporal_jitter[0],
								&s->temporal_jitter[1])) {
		proj[2] += 2.0f * s->temporal_jitter[0] / (float)s->camera.viewport.width;
		proj[6] -= 2.0f * s->temporal_jitter[1] / (float)s->camera.viewport.height;
	}
	scene_mat4_mul(s->jittered_view_proj, proj, view);
	s->instance_count      = 0;
	s->instances_dropped   = 0;
	s->light_count         = 0;
	s->lights_dropped      = 0;
	s->cluster_active      = 0;
	s->cluster_ready       = 0;
	s->frame_uniform_count = 0;
	memset(&s->pbr_environment, 0, sizeof s->pbr_environment);
	s->pbr_environment_set       = 0;
	s->probe_set_count           = 0;
	s->probe_key_shadow          = 0;
	s->probe_occlusion_enabled   = 0;
	s->planar_reflection_enabled = 0;
	s->pbr_debug_views           = 0;
	memset(s->pbr_debug_material, 0, sizeof s->pbr_debug_material);
	AeronScene_SetScreenSources(s, NULL, NULL, NULL);
	memset(&s->directional_shadow, 0, sizeof s->directional_shadow);
	memset(&s->shadow_uniform, 0, sizeof s->shadow_uniform);
	memset(&s->shadow_stats, 0, sizeof s->shadow_stats);
	s->shadow_only_count    = 0;
	s->shadow_only_dropped  = 0;
	s->sky_cube             = NULL;
	s->sky_overlay          = NULL;
	s->sky_sun_count        = 0;
	s->bb_count             = 0;
	s->bb_dropped           = 0;
	s->bb_frame_verts       = 0;
	s->bb_frame_has_vel     = 0;
	s->flare_set            = 0;
	s->flare_frame          = 0;
	s->overlay_count        = 0;
	s->overlays_dropped     = 0;
	s->overlay_vertex_count = 0;
	s->overlay_frame_ready  = 0;
	s->storage_ready        = 0;
	s->after_meshes_fn      = NULL;
	s->after_meshes_user    = NULL;
	/* Default motion context: prev = current (zero camera velocity). */
	memcpy(s->mb_prev_view_proj, s->unjittered_view_proj, sizeof s->mb_prev_view_proj);
	s->mb_velocity_regen     = 0;
	s->scene_rt_out          = s->color_rt;
	s->scene_rt_out_borrowed = 0;
	return 1;
}

static int scene_reserve_instances(AeronScene3D* s, int instance_capacity, int shadow_only_capacity);

/* The next capacity of a list that holds `capacity` items, up to `limit`. */
static int scene_grown_capacity(int capacity, int limit) {
	const int grown = capacity ? capacity * 2 : 1024;
	return grown < limit ? grown : limit;
}

/* The groups an instance hides, limited to its mesh's groups. */
static uint32_t scene_hidden_tag_groups(const AeronSceneMeshInstance* instance) {
	const uint32_t count = instance->mesh->tag_groups.count;
	return count >= 32u ? instance->hidden_tag_groups : instance->hidden_tag_groups & ((1u << count) - 1u);
}

void AeronScene_AddMeshInstance(AeronScene3D* s, const AeronSceneMeshInstance* instance) {
	if (!s || !instance || !instance->mesh) {
		return;
	}
	if (s->instance_count >= s->instance_capacity && s->instance_capacity < AERON_SCENE_MAX_INSTANCES) {
		scene_reserve_instances(s, scene_grown_capacity(s->instance_capacity, AERON_SCENE_MAX_INSTANCES),
								s->shadow_only_capacity);
	}
	if (s->instance_count >= s->instance_capacity) {
		if (!s->instances_dropped) {
			Aeron_LogWarn("aeron.scene", "instance cap (%d) hit; dropping", AERON_SCENE_MAX_INSTANCES);
		}
		s->instances_dropped++;
		return;
	}
	s->instances[s->instance_count]                     = *instance;
	s->instances[s->instance_count++].hidden_tag_groups = scene_hidden_tag_groups(instance);
}

/* A light with a finite, non-zero half axis. */
static int scene_light_is_tube(const AeronSceneLight* light) {
	const float length_sq = light->half_axis[0] * light->half_axis[0] +
							light->half_axis[1] * light->half_axis[1] +
							light->half_axis[2] * light->half_axis[2];
	return length_sq > 0.0f && isfinite(length_sq);
}

/* A light's place in the point light order: spheres, then spot lights,
 * then tubes, each by view depth, so the lights reaching a depth bin have
 * nearby indices. */
enum { SCENE_LIGHT_SPHERE, SCENE_LIGHT_SPOT, SCENE_LIGHT_TUBE };

typedef struct SceneLightOrder {
	uint32_t kind;
	float    depth;
	uint32_t light;
} SceneLightOrder;

static int scene_light_order_compare(const void* a, const void* b) {
	const SceneLightOrder* x = (const SceneLightOrder*)a;
	const SceneLightOrder* y = (const SceneLightOrder*)b;
	if (x->kind != y->kind) {
		return x->kind < y->kind ? -1 : 1;
	}
	if (x->depth != y->depth) {
		return x->depth < y->depth ? -1 : 1;
	}
	return x->light < y->light ? -1 : (x->light > y->light ? 1 : 0);
}

/* The unit direction of a one-sided light into `out`, returning one, or
 * zero for a light emitting all around (`out` untouched). A tube's
 * direction loses its part along the axis. */
static int scene_light_direction(const AeronSceneLight* light, int is_tube, float out[3]) {
	float d[3] = { light->direction[0], light->direction[1], light->direction[2] };
	if (is_tube) {
		const float axis_sq = light->half_axis[0] * light->half_axis[0] +
							  light->half_axis[1] * light->half_axis[1] +
							  light->half_axis[2] * light->half_axis[2];
		const float along =
			(d[0] * light->half_axis[0] + d[1] * light->half_axis[1] + d[2] * light->half_axis[2]) / axis_sq;
		for (int k = 0; k < 3; ++k) {
			d[k] -= along * light->half_axis[k];
		}
	}
	const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	const float input =
		sqrtf(light->direction[0] * light->direction[0] + light->direction[1] * light->direction[1] +
			  light->direction[2] * light->direction[2]);
	if (!(length > 1e-3f * input) || !isfinite(length)) {
		return 0;
	}
	for (int k = 0; k < 3; ++k) {
		out[k] = d[k] / length;
	}
	return 1;
}

/* The cosines of a spot light's inner and outer angles, returning one, or
 * zero for a light that isn't a spot. The outer angle is at most pi/2 and
 * the inner one less than it. */
static int scene_light_spot_cosines(const AeronSceneLight* light, float* cos_inner, float* cos_outer) {
	const float outer = fminf(light->spot_outer_angle, 1.5707963267948966f);
	if (!(outer > 0.0f) || scene_light_is_tube(light)) {
		return 0;
	}
	const float inner = light->spot_inner_angle > 0.0f ? fminf(light->spot_inner_angle, outer) : 0.0f;
	*cos_inner        = cosf(inner);
	*cos_outer        = cosf(outer);
	return 1;
}

void AeronScene_LightBounds(const AeronSceneLight* light, float center[3], float* radius) {
	if (!light || !center || !radius) {
		return;
	}
	const float half_length = scene_light_is_tube(light) ? sqrtf(light->half_axis[0] * light->half_axis[0] +
																 light->half_axis[1] * light->half_axis[1] +
																 light->half_axis[2] * light->half_axis[2])
														 : 0.0f;
	memcpy(center, light->pos, 3 * sizeof *center);
	*radius = light->radius + half_length;
	float cos_inner, cos_outer, direction[3];
	if (!scene_light_spot_cosines(light, &cos_inner, &cos_outer) ||
		!scene_light_direction(light, 0, direction)) {
		return;
	}
	/* A narrow cone fits in the sphere through its apex and its rim; a wide
	 * one in the sphere around its rim. */
	const float range     = light->radius;
	const float sin_outer = sqrtf(fmaxf(1.0f - cos_outer * cos_outer, 0.0f));
	const float offset    = cos_outer >= sin_outer ? range / (2.0f * cos_outer) : range * cos_outer;
	*radius               = cos_outer >= sin_outer ? offset : range * sin_outer;
	for (int k = 0; k < 3; ++k) {
		center[k] += direction[k] * offset;
	}
}

int AeronScene_AddLight(AeronScene3D* s, const AeronSceneLight* light) {
	if (!s || !light) {
		return 0;
	}
	if (s->light_count >= AERON_SCENE_MAX_LIGHTS) {
		if (!s->lights_dropped) {
			Aeron_LogWarn("aeron.scene", "point-light cap (%d) hit; dropping", AERON_SCENE_MAX_LIGHTS);
		}
		s->lights_dropped++;
		return 0;
	}
	s->lights[s->light_count++] = *light;
	return 1;
}

/* A panel at least this many times longer than wide is drawn as a one-sided
 * tube along its length; a squarer one as a one-sided sphere of its area. */
#define SCENE_MESH_LIGHT_PANEL_TUBE_ASPECT 3.0f

/* `v` through the component's articulation rows, then the instance
 * transform, both without translation. */
static void scene_mesh_light_vector(const float (*rows)[4], const float transform[16],
									const AeronFlightVec3* v, float out[3]) {
	float local[3] = { v->x, v->y, v->z };
	if (rows) {
		float articulated[3];
		for (int r = 0; r < 3; ++r) {
			articulated[r] = rows[r][0] * local[0] + rows[r][1] * local[1] + rows[r][2] * local[2];
		}
		memcpy(local, articulated, sizeof local);
	}
	for (int r = 0; r < 3; ++r) {
		out[r] = transform[r * 4 + 0] * local[0] + transform[r * 4 + 1] * local[1] +
				 transform[r * 4 + 2] * local[2];
	}
}

static float scene_vec3_length(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* `light` through the articulation rows `rows` (optional), then the
 * instance transform. */
static int scene_flight_light(const AeronFlightLight* light, const float (*rows)[4],
							  const float transform[16], float units_per_meter, float intensity_scale,
							  float exposure, AeronSceneLight* out) {
	float p[3] = { light->position.x, light->position.y, light->position.z };
	if (rows) {
		float articulated[3];
		for (int r = 0; r < 3; ++r) {
			articulated[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
		}
		memcpy(p, articulated, sizeof p);
	}
	memset(out, 0, sizeof *out);
	for (int r = 0; r < 3; ++r) {
		out->pos[r] = transform[r * 4 + 0] * p[0] + transform[r * 4 + 1] * p[1] +
					  transform[r * 4 + 2] * p[2] + transform[r * 4 + 3];
	}
	/* The shape, in world units. */
	float source_radius =
		light->source_radius > 0.0f ? light->source_radius : AERON_FLIGHT_LIGHT_DEFAULT_RADIUS;
	source_radius *= units_per_meter;
	if (light->shape == AERON_FLIGHT_LIGHT_TUBE) {
		scene_mesh_light_vector(rows, transform, &light->half_x, out->half_axis);
	} else if (light->shape == AERON_FLIGHT_LIGHT_PANEL) {
		float half_x[3], half_y[3];
		scene_mesh_light_vector(rows, transform, &light->half_x, half_x);
		scene_mesh_light_vector(rows, transform, &light->half_y, half_y);
		const float length_x = scene_vec3_length(half_x);
		const float length_y = scene_vec3_length(half_y);
		const float longer   = length_x > length_y ? length_x : length_y;
		const float shorter  = length_x > length_y ? length_y : length_x;
		if (longer >= SCENE_MESH_LIGHT_PANEL_TUBE_ASPECT * shorter) {
			memcpy(out->half_axis, length_x > length_y ? half_x : half_y, sizeof out->half_axis);
			source_radius = shorter;
		} else {
			source_radius = 2.0f * sqrtf(length_x * length_y / 3.14159265f);
		}
	}
	if (light->direction.x != 0.0f || light->direction.y != 0.0f || light->direction.z != 0.0f) {
		scene_mesh_light_vector(rows, transform, &light->direction, out->direction);
	}
	out->spot_inner_angle = light->spot_inner_angle;
	out->spot_outer_angle = light->spot_outer_angle;
	/* I / d_m^2 = I * k^2 / d_world^2 with k world units per metre. */
	const float intensity = light->intensity * intensity_scale;
	float       peak      = 0.0f;
	for (int c = 0; c < 3; ++c) {
		out->color[c] = light->color[c] * intensity * units_per_meter * units_per_meter;
		if (light->color[c] * intensity > peak) {
			peak = light->color[c] * intensity;
		}
	}
	if (!(peak > 0.0f) || !isfinite(peak)) {
		return 0;
	}
	const float range_m =
		light->range > 0.0f ? light->range : sqrtf(peak * exposure / AERON_SCENE_LIGHT_CUTOFF);
	out->radius         = range_m * units_per_meter;
	out->source_radius  = source_radius;
	return 1;
}

int AeronScene_FlightLight(const AeronFlightLight* light, const float transform[16], float units_per_meter,
						   float intensity_scale, float exposure, AeronSceneLight* out) {
	if (!light || !transform || !(units_per_meter > 0.0f) || !(exposure > 0.0f) || !out) {
		return 0;
	}
	return scene_flight_light(light, NULL, transform, units_per_meter, intensity_scale, exposure, out);
}

int AeronScene_MeshLight(const AeronSceneMesh* mesh, uint32_t index, const float transform[16],
						 float units_per_meter, const AeronSceneMeshTable* table, uint32_t variant,
						 uint32_t hidden_tag_groups, float intensity_scale, float exposure,
						 AeronSceneLight* out) {
	if (!mesh || index >= mesh->light_count || !transform || !(units_per_meter > 0.0f) ||
		!(exposure > 0.0f) || !out || (mesh->lights[index].tag_groups & hidden_tag_groups) != 0) {
		return 0;
	}
	if (mesh->light_variant_masks) {
		const uint32_t  last = mesh->light_variant_index_count - 1u;
		const uint32_t  i    = variant < last ? variant : last;
		const uint32_t* mask =
			&mesh->light_variant_masks[(size_t)index * AERON_SCENE_MESH_LIGHT_VARIANT_WORDS];
		if (!(mask[i / 32] & (1u << (i % 32)))) {
			return 0;
		}
	}
	const AeronFlightLight* light      = &mesh->lights[index];
	const uint32_t          components = mesh->component_count ? mesh->component_count : 1u;
	const float (*rows)[4]             = NULL;
	if (table && light->component_index < components) {
		const uint32_t c = light->component_index;
		/* A hidden component (destroyed, or hidden by a profile) is dark. */
		if (table->visibility_packed[c >> 2][c & 3] == 0.0f) {
			return 0;
		}
		rows = table->rows[c];
	}
	return scene_flight_light(light, rows, transform, units_per_meter, intensity_scale, exposure, out);
}

int AeronScene_SetClusteredLights(AeronScene3D* s, const AeronSceneClusteredLightDesc* desc) {
	if (!s || !desc || desc->depth_slices < 4u || desc->depth_slices > 64u) {
		return 0;
	}
	s->cluster_desc            = *desc;
	s->cluster_desc.debug_view = desc->debug_view != 0;
	return 1;
}

void AeronScene_GetClusteredLightStats(const AeronScene3D* s, AeronSceneClusteredLightStats* out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof *out);
	if (!s) {
		return;
	}
	out->submitted_light_count  = s->light_count > 0 ? (uint32_t)s->light_count : 0u;
	out->dropped_light_count    = s->lights_dropped > 0 ? (uint32_t)s->lights_dropped : 0u;
	out->grid_x                 = s->cluster_uniform.grid_x;
	out->grid_y                 = s->cluster_uniform.grid_y;
	out->grid_z                 = s->cluster_uniform.grid_z;
	out->effective_tile_size    = s->cluster_uniform.tile_size;
	out->cluster_count          = s->cluster_count;
	out->clustered_active       = (uint32_t)(s->cluster_active != 0);
	out->allocated_buffer_bytes = (uint64_t)s->cluster_light_buffer_cap + s->cluster_data_buffer_cap;
}

void AeronScene_SetDirectionalShadow(AeronScene3D* s, const AeronSceneDirectionalShadowDesc* shadow) {
	if (!s) {
		return;
	}
	if (shadow) {
		s->directional_shadow = *shadow;
	} else {
		memset(&s->directional_shadow, 0, sizeof s->directional_shadow);
	}
}

void AeronScene_AddShadowCaster(AeronScene3D* s, const AeronSceneMeshInstance* instance) {
	if (!s || !instance || !instance->mesh) {
		return;
	}
	if (instance->shadow_flags & AERON_SCENE_INSTANCE_NO_CAST_SHADOW) {
		return;
	}
	if (s->shadow_only_count >= s->shadow_only_capacity &&
		s->shadow_only_capacity < AERON_SCENE_MAX_SHADOW_ONLY) {
		scene_reserve_instances(s, s->instance_capacity,
								scene_grown_capacity(s->shadow_only_capacity, AERON_SCENE_MAX_SHADOW_ONLY));
	}
	if (s->shadow_only_count >= s->shadow_only_capacity) {
		if (!s->shadow_only_dropped) {
			Aeron_LogWarn("aeron.scene", "shadow-only caster cap (%d) hit; dropping",
						  AERON_SCENE_MAX_SHADOW_ONLY);
		}
		s->shadow_only_dropped++;
		return;
	}
	s->shadow_only[s->shadow_only_count]                     = *instance;
	s->shadow_only[s->shadow_only_count++].hidden_tag_groups = scene_hidden_tag_groups(instance);
}

void AeronScene_GetDirectionalShadowStats(const AeronScene3D* s, AeronSceneDirectionalShadowStats* out) {
	if (s && out) {
		*out = s->shadow_stats;
	}
}

void AeronScene_SetPost(AeronScene3D* s, const AeronScenePostDesc* post) {
	if (!s) {
		return;
	}
	if (post) {
		s->post = *post;
	} else {
		memset(&s->post, 0, sizeof s->post);
	}
}

void AeronScene_SetTemporal(AeronScene3D* s, const AeronSceneTemporalDesc* temporal) {
	if (!s) {
		return;
	}
	if (temporal) {
		const AeronTemporalMode previous_mode = s->temporal.mode;
		s->temporal                           = *temporal;
		if (!scene_temporal_mode_valid(s->temporal.mode)) {
			s->temporal.mode = AERON_TEMPORAL_OFF;
		}
		if (s->sample_count != AERON_SAMPLE_COUNT_1) {
			s->temporal.mode = AERON_TEMPORAL_OFF;
		}
		if (s->temporal.sharpness < 0.0f) {
			s->temporal.sharpness = 0.0f;
		} else if (s->temporal.sharpness > 1.0f) {
			s->temporal.sharpness = 1.0f;
		}
		if (s->temporal.mode != previous_mode) {
			s->temporal.reset_history = 1;
			s->temporal_phase         = 0;
		}
		if (s->temporal.reset_history) {
			s->mb_temporal_motion_valid = 0;
			s->mb_tile_valid            = 0;
			s->mb_neighbor_valid        = 0;
		}
	} else {
		memset(&s->temporal, 0, sizeof s->temporal);
		s->temporal_phase           = 0;
		s->mb_temporal_motion_valid = 0;
		s->mb_tile_valid            = 0;
		s->mb_neighbor_valid        = 0;
	}
}

int AeronScene_GetTemporalProfileInfo(const AeronScene3D* s, AeronTemporalProfileInfo* info) {
	if (!info) {
		return 0;
	}
	memset(info, 0, sizeof *info);
	return s && s->temporal_upscaler ? AeronTemporalUpscaler_GetProfileInfo(s->temporal_upscaler, info) : 0;
}

void AeronScene_SetMotionContext(AeronScene3D* s, const float prev_view_proj[16], const float prev_ori[4],
								 int velocity_regen) {
	if (!s) {
		return;
	}
	if (prev_ori) {
		memcpy(s->mb_prev_ori, prev_ori, sizeof s->mb_prev_ori);
	}
	if (prev_view_proj) {
		memcpy(s->mb_prev_view_proj, prev_view_proj, sizeof s->mb_prev_view_proj);
	} else {
		memcpy(s->mb_prev_view_proj, s->unjittered_view_proj, sizeof s->mb_prev_view_proj);
		s->mb_velocity_valid        = 0;
		s->mb_temporal_motion_valid = 0;
		s->mb_tile_valid            = 0;
		s->mb_neighbor_valid        = 0;
		AeronTemporalUpscaler_InvalidateRetainedMotionVectors(s->temporal_upscaler);
	}
	s->mb_velocity_regen = velocity_regen ? 1 : 0;
	if (s->mb_velocity_regen) {
		s->mb_velocity_valid = 1;
	}
}

AeronRenderTarget* AeronScene_SceneRt(AeronScene3D* s) {
	return s ? (s->scene_rt_out ? s->scene_rt_out : s->color_rt) : NULL;
}

AeronTexture* AeronScene_ScreenLayer(AeronScene3D* s) {
	return s && s->screen_layer_active ? Aeron_RenderTargetGetTexture(s->screen_layer_rt) : NULL;
}

void AeronScene_SetClearColor(AeronScene3D* s, const float rgba[4]) {
	if (s && rgba) {
		memcpy(s->clear_rgba, rgba, sizeof s->clear_rgba);
	}
}

void AeronScene_SetSkyEnvironment(AeronScene3D* s, const AeronPreparedEnvironment* environment) {
	if (!s) {
		return;
	}
	s->sky_cube    = environment ? environment->radiance : NULL;
	s->sky_overlay = s->sky_cube ? environment->overlay : NULL;
	if (s->sky_cube) {
		s->sky_exposure = environment->settings.intensity;
		memcpy(s->sky_world_to_cube, environment->world_to_cube, sizeof s->sky_world_to_cube);
		memcpy(s->sky_overlay_world_to_cube, environment->overlay_world_to_cube,
			   sizeof s->sky_overlay_world_to_cube);
	}
}

/* ---- sky cube (AeronScene_SetSkyEnvironment) ----
 *
 * Fullscreen-triangle cube-map sky, first draw of the color pass.
 * No vertex buffer (the VS synthesizes the triangle from SV_VertexID);
 * reversed-Z far plane (z = 0), depth GE test, write OFF — valid in
 * both pass shapes: against the cleared 0.0 depth of the monolithic
 * pass, and against the prepass-laid depth of the forward pass (mesh
 * pixels carry z > 0 and reject the sky). */

static AeronGraphicsPipeline* scene_sky_pipeline(AeronScene3D* s, AeronShader* fs,
												 AeronBlendStateDesc blend) {
	AeronColorTargetStateDesc cts[1] = { 0 };
	cts[0].format                    = s->color_format;
	cts[0].blend                     = blend;
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader      = s->sky_vs,
		.fragment_shader    = fs,
		.primitive_type     = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode          = AERON_CULL_NONE,
		.depth_format       = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth              = { .depth_test = 1, .depth_write = 0, .compare = AERON_COMPARE_GREATER_EQUAL },
		.color_target_count = 1,
		.color_targets      = cts,
		.sample_count       = s->sample_count,
	});
}

static int scene_sky_ensure(AeronScene3D* s) {
	if (s->sky_tried) {
		return s->sky_vs && s->sky_fs;
	}
	s->sky_tried = 1;
	s->sky_vs = AeronSceneInternal_CompileShader("scene_sky_cube.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 0);
	s->sky_fs = AeronSceneInternal_CompileShader("scene_sky_cube.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	return s->sky_vs && s->sky_fs;
}

/* Cube-sky VS uniforms: eye→cube rotation (pre-multiplied basis × camera
 * world→eye transpose) + unprojection. */
static void scene_sky_bind_vs(AeronScene3D* s, AeronRenderPass* pass, const float world_to_cube[9]) {
	float r3[9];
	AeronSceneInternal_QuatToMat3(s->camera.ori, r3); /* world->eye */

	struct {
		float rot[3][4];
		float unproj[4];
	} vsu;

	memset(&vsu, 0, sizeof vsu);
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			vsu.rot[r][c] = world_to_cube[r * 3 + 0] * r3[c * 3 + 0] +
							world_to_cube[r * 3 + 1] * r3[c * 3 + 1] +
							world_to_cube[r * 3 + 2] * r3[c * 3 + 2];
		}
	}
	vsu.unproj[0] = tanf(s->camera.h_half_rad);
	vsu.unproj[1] = tanf(s->camera.v_half_rad);
	scene_jittered_projection_offsets(s, &vsu.unproj[2], &vsu.unproj[3]);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vsu, sizeof vsu);
}

/* ---- sky suns (AeronScene_AddSkySun) ----
 *
 * Disc suns use their own additive pipeline, drawn right after the sky
 * cube with the same far-plane depth rule; sprite suns become additive
 * SKY billboards. */

#define SKY_SUN_PI 3.14159265358979323846
/* Limb-darkening coefficient of the linear law I(mu) = 1 - u (1 - mu). */
#define SKY_SUN_LIMB_DARKENING 0.6f
/* Largest radiance written, kept inside the half-float scene target. */
#define SKY_SUN_MAX_RADIANCE 60000.0f

static void sky_sun_basis(const AeronScene3D* s, const float dir[3], float t0[3], float t1[3]) {
	float m[9];
	AeronSceneInternal_QuatToMat3(s->camera.ori, m); /* world->eye rows */
	const float* ref = &m[3];
	if (fabsf(ref[0] * dir[0] + ref[1] * dir[1] + ref[2] * dir[2]) > 0.99f)
		ref = &m[0];
	t0[0]             = ref[1] * dir[2] - ref[2] * dir[1];
	t0[1]             = ref[2] * dir[0] - ref[0] * dir[2];
	t0[2]             = ref[0] * dir[1] - ref[1] * dir[0];
	const float t0len = sqrtf(t0[0] * t0[0] + t0[1] * t0[1] + t0[2] * t0[2]);
	for (int i = 0; i < 3; ++i)
		t0[i] /= t0len;
	t1[0] = dir[1] * t0[2] - dir[2] * t0[1];
	t1[1] = dir[2] * t0[0] - dir[0] * t0[2];
	t1[2] = dir[0] * t0[1] - dir[1] * t0[0];
}

static float sky_sun_max3(const float v[3]) { return fmaxf(v[0], fmaxf(v[1], v[2])); }

void AeronScene_AddSkySun(AeronScene3D* s, const AeronEnvironmentLight* light) {
	if (!s || !light || light->visual == AERON_ENV_SUN_NONE)
		return;
	float       dir[3] = { light->direction_world[0], light->direction_world[1], light->direction_world[2] };
	const float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(length > 1e-6f))
		return;
	for (int i = 0; i < 3; ++i)
		dir[i] /= length;
	float t0[3], t1[3];
	sky_sun_basis(s, dir, t0, t1);
	const float deg = (float)(SKY_SUN_PI / 180.0);
	/* One output pixel, as a tangent at the view centre. */
	const float pixel = s->camera.viewport.height > 0
							? 2.0f * tanf(s->camera.v_half_rad) / (float)s->camera.viewport.height
							: 1e-3f;

	if (light->visual == AERON_ENV_SUN_SPRITE) {
		if (!light->sprite || !(light->angular_size_degrees > 0.0f))
			return;
		const float             half  = tanf(0.5f * light->angular_size_degrees * deg);
		const float             scale = light->sprite_strength;
		AeronSceneBillboardDesc desc;
		memset(&desc, 0, sizeof desc);
		desc.texture                   = light->sprite;
		desc.blend                     = AERON_SCENE_BILLBOARD_BLEND_ADDITIVE;
		desc.stage                     = AERON_SCENE_BILLBOARD_STAGE_SKY;
		static const float signs[4][2] = {
			{ -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f }
		};
		static const float uvs[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		for (int c = 0; c < 4; ++c) {
			for (int i = 0; i < 3; ++i)
				desc.corners[c][i] =
					s->camera.pos[i] + dir[i] + (t0[i] * signs[c][0] + t1[i] * signs[c][1]) * half;
			desc.uv[c][0]     = uvs[c][0];
			desc.uv[c][1]     = uvs[c][1];
			desc.colors[c][0] = desc.colors[c][1] = desc.colors[c][2] = scale;
			desc.colors[c][3]                                         = 1.0f;
		}
		AeronScene_AddBillboard(s, &desc);
		return;
	}

	if (s->sky_sun_count >= AERON_SCENE_MAX_SKY_SUNS || !(light->angular_radius_degrees > 0.0f))
		return;
	/* Mean disc radiance = irradiance / solid angle, so the visible disc
	 * carries the light it casts; the centre is brighter by the limb law. */
	const float angle  = light->angular_radius_degrees * deg;
	const float omega  = (float)(2.0 * SKY_SUN_PI) * (1.0f - cosf(angle));
	const float factor = light->radiance_scale;
	float       irradiance[3], center[3];
	for (int i = 0; i < 3; ++i) {
		irradiance[i] = light->color_linear[i] * factor;
		center[i]     = irradiance[i] / omega / (1.0f - SKY_SUN_LIMB_DARKENING / 3.0f);
	}
	if (!(sky_sun_max3(irradiance) > 0.0f) || !isfinite(sky_sun_max3(center)))
		return;
	/* Keep the flux when the disc is below a pixel or brighter than the
	 * target can hold: widen it and lower its radiance by the area ratio. */
	float radius = tanf(angle);
	if (radius < 0.75f * pixel) {
		const float ratio = radius / (0.75f * pixel);
		for (int i = 0; i < 3; ++i)
			center[i] *= ratio * ratio;
		radius = 0.75f * pixel;
	}
	/* Display cap: the disc keeps its size and is drawn no brighter than
	 * the cap after the exposure; only its appearance changes, not the
	 * light it casts. */
	const float display_max = light->display_max_radiance / s->exposure;
	if (display_max > 0.0f && sky_sun_max3(center) > display_max) {
		const float ratio = display_max / sky_sun_max3(center);
		for (int i = 0; i < 3; ++i)
			center[i] *= ratio;
	}
	const float peak = sky_sun_max3(center);
	if (peak > SKY_SUN_MAX_RADIANCE) {
		const float ratio = peak / SKY_SUN_MAX_RADIANCE;
		for (int i = 0; i < 3; ++i)
			center[i] /= ratio;
		radius *= sqrtf(ratio);
	}

	AeronSceneSkySun* sun = &s->sky_suns[s->sky_sun_count++];
	memset(sun, 0, sizeof *sun);
	memcpy(sun->direction, dir, sizeof sun->direction);
	memcpy(sun->tangent0, t0, sizeof sun->tangent0);
	memcpy(sun->tangent1, t1, sizeof sun->tangent1);
	memcpy(sun->disc, center, sizeof center);
	sun->disc[3]            = SKY_SUN_LIMB_DARKENING;
	sun->radius             = radius;
	sun->glow_radius        = 0.0f;
	const float glow_radius = tanf(light->glow_radius_degrees * deg);
	if (light->glow_strength > 0.0f && glow_radius > radius) {
		/* The glow (1 - t)^2 from R to G integrates to glow_strength times
		 * the disc's irradiance: its area integral is 2 pi w (R/3 + w/12). */
		const float w        = glow_radius - radius;
		const float integral = (float)(2.0 * SKY_SUN_PI) * w * (radius / 3.0f + w / 12.0f);
		for (int i = 0; i < 3; ++i)
			sun->glow[i] = fminf(light->glow_strength * irradiance[i] / integral, SKY_SUN_MAX_RADIANCE);
		sun->glow_radius = glow_radius;
	}
	sun->half_extent = fmaxf(sun->radius, sun->glow_radius) * 1.05f + 2.0f * pixel;
}

static int scene_draw_sky_suns(AeronScene3D* s, AeronRenderPass* pass) {
	if (s->sky_sun_count == 0)
		return 1;
	if (!s->sky_sun_tried) {
		s->sky_sun_tried = 1;
		s->sky_sun_vs =
			AeronSceneInternal_CompileShader("scene_sky_sun.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 0);
		s->sky_sun_fs =
			AeronSceneInternal_CompileShader("scene_sky_sun.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 1, 0);
		if (s->sky_sun_vs && s->sky_sun_fs) {
			AeronColorTargetStateDesc cts[1] = { 0 };
			cts[0].format                    = s->color_format;
			cts[0].blend                     = (AeronBlendStateDesc) { .enabled   = 1,
																	   .src_color = AERON_BLEND_ONE,
																	   .dst_color = AERON_BLEND_ONE,
																	   .color_op  = AERON_BLEND_OP_ADD,
																	   .src_alpha = AERON_BLEND_ZERO,
																	   .dst_alpha = AERON_BLEND_ONE,
																	   .alpha_op  = AERON_BLEND_OP_ADD };
			s->sky_sun_pipe                  = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
				.vertex_shader   = s->sky_sun_vs,
				.fragment_shader = s->sky_sun_fs,
				.primitive_type  = AERON_PRIMITIVE_TRIANGLES,
				.cull_mode       = AERON_CULL_NONE,
				.depth_format    = AERON_TEXTURE_FORMAT_D32_FLOAT,
				.depth = { .depth_test = 1, .depth_write = 0, .compare = AERON_COMPARE_GREATER_EQUAL },
				.color_target_count = 1,
				.color_targets      = cts,
				.sample_count       = s->sample_count,
			});
		}
	}
	if (!s->sky_sun_pipe)
		return 0;
	Aeron_BindGraphicsPipeline(pass, s->sky_sun_pipe);
	for (int i = 0; i < s->sky_sun_count; ++i) {
		const AeronSceneSkySun* sun = &s->sky_suns[i];

		struct {
			float view_proj[16];
			float center[4];
			float tangent0[4];
			float tangent1[4];
		} vsu;

		memset(&vsu, 0, sizeof vsu);
		memcpy(vsu.view_proj, s->jittered_view_proj, sizeof vsu.view_proj);
		for (int c = 0; c < 3; ++c) {
			vsu.center[c]   = s->camera.pos[c] + sun->direction[c];
			vsu.tangent0[c] = sun->tangent0[c];
			vsu.tangent1[c] = sun->tangent1[c];
		}
		vsu.tangent0[3] = sun->half_extent;
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vsu, sizeof vsu);
		float fsu[12] = { sun->disc[0], sun->disc[1],     sun->disc[2], sun->disc[3],
						  sun->glow[0], sun->glow[1],     sun->glow[2], 0.0f,
						  sun->radius,  sun->glow_radius, 0.0f,         0.0f };
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, fsu, sizeof fsu);
		Aeron_Draw(pass, 6, 0);
	}
	return 1;
}

static int scene_draw_sky_cube(AeronScene3D* s, AeronRenderPass* pass);

/* Each part is its own debug group, balanced inside the pass, so frame
 * captures and profilers time the sky cube and the sun discs apart. */
static int scene_draw_sky(AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderPass* pass) {
	if (s->sky_cube) {
		Aeron_GpuDebugPush(cmd, "Sky cube");
		const int ok = scene_draw_sky_cube(s, pass);
		Aeron_GpuDebugPop(cmd);
		if (!ok)
			return 0;
	}
	if (s->sky_sun_count > 0) {
		Aeron_GpuDebugPush(cmd, "Sky suns");
		const int ok = scene_draw_sky_suns(s, pass);
		Aeron_GpuDebugPop(cmd);
		if (!ok)
			return 0;
	}
	return 1;
}

static int scene_draw_sky_cube(AeronScene3D* s, AeronRenderPass* pass) {
	if (!s->sky_cube) {
		return 1;
	}
	if (!scene_sky_ensure(s)) {
		return 0;
	}
	if (!s->sky_pipe) {
		s->sky_pipe = scene_sky_pipeline(s, s->sky_fs, AeronSceneInternal_BlendOpaque());
		if (!s->sky_pipe) {
			return 0;
		}
	}
	Aeron_BindGraphicsPipeline(pass, s->sky_pipe);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, s->sky_cube, s->pbr_sampler);

	/* eye→cube = world_to_cube × eye→world; eye→world is the transpose
	 * (orthogonal inverse) of the camera's world→eye rotation. Pushed
	 * pre-multiplied so the VS stays a single mul(). Row-major float3x3
	 * cbuffer packing: three float4-aligned rows. */
	scene_sky_bind_vs(s, pass, s->sky_world_to_cube);

	float psu[4] = { s->sky_exposure > 0.0f ? s->sky_exposure : 1.0f, 0.0f, 0.0f, 0.0f };
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, psu, sizeof psu);

	Aeron_Draw(pass, 3, 0);
	return 1;
}

/* ---- sky overlay (AeronPreparedEnvironment.overlay) ----
 *
 * The overlay cube of the sky's environment, drawn over the SKY billboards
 * and under the meshes, with the sky's vertex shader and depth state, so
 * meshes cover it in both pass shapes. Its colour is premultiplied by
 * alpha: target = overlay.rgb * intensity + target * (1 - overlay.a).
 * Overwrites the stage-0 uniforms, so the caller binds the frame uniforms
 * again. */
static int scene_draw_sky_overlay(AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderPass* pass) {
	if (!scene_sky_ensure(s)) {
		return 0;
	}
	if (!s->sky_overlay_tried) {
		s->sky_overlay_tried = 1;
		s->sky_overlay_fs =
			AeronSceneInternal_CompileShader("scene_sky_overlay.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
		if (s->sky_overlay_fs) {
			const AeronBlendStateDesc blend = {
				.enabled   = 1,
				.src_color = AERON_BLEND_ONE,
				.dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
				.color_op  = AERON_BLEND_OP_ADD,
				.src_alpha = AERON_BLEND_ZERO,
				.dst_alpha = AERON_BLEND_ONE,
				.alpha_op  = AERON_BLEND_OP_ADD,
			};
			s->sky_overlay_pipe = scene_sky_pipeline(s, s->sky_overlay_fs, blend);
		}
	}
	if (!s->sky_overlay_pipe) {
		return 0;
	}
	Aeron_GpuDebugPush(cmd, "Sky overlay");
	Aeron_BindGraphicsPipeline(pass, s->sky_overlay_pipe);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, s->sky_overlay, s->pbr_sampler);
	scene_sky_bind_vs(s, pass, s->sky_overlay_world_to_cube);
	float psu[4] = { s->sky_exposure > 0.0f ? s->sky_exposure : 1.0f, 0.0f, 0.0f, 0.0f };
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, psu, sizeof psu);
	Aeron_Draw(pass, 3, 0);
	Aeron_GpuDebugPop(cmd);
	return 1;
}

int AeronScene_SetMeshSampler(AeronScene3D* s, AeronSampler* sampler) {
	if (!s) {
		return 0;
	}
	if (sampler == s->mesh_sampler_source) {
		return 1;
	}
	AeronSamplerDesc base_desc = scene_world_sampler_desc(0.0f);
	if (sampler && !Aeron_SamplerGetDesc(sampler, &base_desc)) {
		return 0;
	}
	AeronSampler* replacements[AERON_TEMPORAL_PERFORMANCE + 1] = { 0 };
	if (!scene_create_atlas_samplers(s, &base_desc, replacements)) {
		Aeron_LogError("aeron.scene", "failed to create temporal mesh sampler variants");
		return 0;
	}
	scene_release_sampler_set(s->mesh_samplers);
	memcpy(s->mesh_samplers, replacements, sizeof replacements);
	s->mesh_sampler_source = sampler;
	scene_select_world_samplers(s);
	return 1;
}

static float s_emissive_scale = 1.0f;

float AeronScene_EmissiveScale(void) { return s_emissive_scale; }

void AeronScene_SetEmissiveScale(float scale) { s_emissive_scale = scale >= 0.0f ? scale : 0.0f; }

void AeronScene_SetPbrDebugMaterial(AeronScene3D* s, int enabled, float metallic, float roughness) {
#ifdef AERON_DEBUG_UI
	if (!s) {
		return;
	}
	memset(s->pbr_debug_material, 0, sizeof s->pbr_debug_material);
	if (enabled && isfinite(metallic) && isfinite(roughness)) {
		s->pbr_debug_material[0] = 1.0f;
		s->pbr_debug_material[1] = fminf(fmaxf(metallic, 0.0f), 1.0f);
		s->pbr_debug_material[2] = fminf(fmaxf(roughness, 0.0f), 1.0f);
		s->pbr_debug_views       = 1;
	}
#else
	(void)s;
	(void)enabled;
	(void)metallic;
	(void)roughness;
#endif
}

void AeronScene_SetPbrDebugViews(AeronScene3D* s, int enabled) {
#ifdef AERON_DEBUG_UI
	if (s) {
		s->pbr_debug_views = enabled != 0;
	}
#else
	(void)s;
	(void)enabled;
#endif
}

void AeronScene_SetBackfaceFill(AeronScene3D* s, int enabled) {
	if (s) {
		s->backface_fill = enabled != 0;
	}
}

void AeronScene_SetSmallInstanceCull(AeronScene3D* s, float prepass_min_px, float draw_min_px) {
	if (s) {
		s->small_cull_prepass_px = prepass_min_px > 0.0f ? prepass_min_px : 0.0f;
		s->small_cull_draw_px    = draw_min_px > 0.0f ? draw_min_px : 0.0f;
	}
}

typedef char AeronSceneScreenFrameSizeCheck[sizeof(AeronSceneScreenFrame) == 1072 ? 1 : -1];

void AeronScene_SetScreenSources(AeronScene3D* s, AeronTexture* graphics, AeronTexture* text,
								 const AeronSceneScreenFrame* frame) {
	if (!s) {
		return;
	}
	if (!graphics || !text || !frame) {
		s->screen_graphics = NULL;
		s->screen_text     = NULL;
		memset(&s->screen_frame, 0, sizeof s->screen_frame);
		return;
	}
	s->screen_graphics = graphics;
	s->screen_text     = text;
	s->screen_frame    = *frame;
}

void AeronScene_SetEffectAtlas(AeronScene3D* s, AeronEffectAtlas* atlas) {
	if (s)
		s->effect_atlas = atlas;
}

void AeronScene_SetAnimationTime(AeronScene3D* s, double seconds) {
	if (s)
		s->animation_time = (float)seconds;
}

void AeronScene_SetExposure(AeronScene3D* s, float exposure) {
	if (s && isfinite(exposure) && exposure > 0.0f)
		s->exposure = exposure;
}

void AeronScene_SetImageRadiance(AeronScene3D* s, float radiance) {
	if (s && isfinite(radiance) && radiance >= 0.0f)
		s->image_radiance = radiance;
}

void AeronScene_SetPbrEnvironment(AeronScene3D* s, const AeronScenePbrEnvironment* environment) {
	if (!s) {
		return;
	}
	memset(&s->pbr_environment, 0, sizeof s->pbr_environment);
	s->pbr_environment_set = 0;
	if (!environment || !environment->sampler || !environment->diffuse ||
		!(environment->diffuse_strength > 0.0f) || !isfinite(environment->diffuse_strength)) {
		return;
	}
	s->pbr_environment     = *environment;
	s->pbr_environment_set = 1;
}

void AeronScene_SetPbrPreparedEnvironment(AeronScene3D* s, const AeronPreparedEnvironment* prepared) {
	if (!prepared) {
		AeronScene_SetPbrEnvironment(s, NULL);
		return;
	}
	AeronScenePbrEnvironment environment;
	memset(&environment, 0, sizeof environment);
	environment.diffuse            = prepared->diffuse;
	environment.specular           = prepared->specular;
	environment.sampler            = prepared->sampler;
	environment.specular_mip_count = prepared->specular_mip_count;
	/* The environment's intensity scales everything derived from the sky
	 * image; the strengths are relative adjustments on top. */
	environment.diffuse_strength  = prepared->settings.intensity * prepared->settings.diffuse_strength;
	environment.specular_strength = prepared->settings.intensity * prepared->settings.specular_strength;
	memcpy(environment.world_to_local, prepared->world_to_cube, sizeof environment.world_to_local);
	AeronScene_SetPbrEnvironment(s, &environment);
}

void AeronScene_SetProbeKeyShadow(AeronScene3D* s, int enabled) {
	if (s) {
		s->probe_key_shadow = enabled != 0;
	}
}

void AeronScene_SetProbeOcclusion(AeronScene3D* s, int enabled) {
	if (s) {
		s->probe_occlusion_enabled = enabled != 0;
	}
}

void AeronScene_SetPlanarReflection(AeronScene3D* s, int enabled) {
	if (s) {
		s->planar_reflection_enabled = enabled != 0;
	}
}

int AeronScene_AddProbeSet(AeronScene3D* s, const AeronProbeSet* probes, const float m[12]) {
	if (!s || !probes || !m || s->probe_set_count >= AERON_SCENE_MAX_PROBE_SETS) {
		return -1;
	}
	/* World to model: the inverse of the affine model-to-world. */
	const float a[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
	const float c0 = a[4] * a[8] - a[5] * a[7], c1 = a[5] * a[6] - a[3] * a[8],
				c2  = a[3] * a[7] - a[4] * a[6];
	const float det = a[0] * c0 + a[1] * c1 + a[2] * c2;
	if (!(fabsf(det) > 1e-20f) || !isfinite(det)) {
		return -1;
	}
	const float inv[9] = { c0 / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
						   c1 / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
						   c2 / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det };
	AeronSceneProbeSet* set = &s->probe_sets[s->probe_set_count];
	memset(set, 0, sizeof *set);
	set->probes = probes;
	for (int row = 0; row < 3; ++row) {
		float length_sq = 0.0f;
		for (int col = 0; col < 3; ++col) {
			set->world_to_model[row * 4 + col] = inv[row * 3 + col];
			length_sq += inv[row * 3 + col] * inv[row * 3 + col];
		}
		set->world_to_model[row * 4 + 3] =
			-(inv[row * 3 + 0] * m[3] + inv[row * 3 + 1] * m[7] + inv[row * 3 + 2] * m[11]);
		set->metres_per_unit = fmaxf(set->metres_per_unit, sqrtf(length_sq));
	}
	const AeronFlightProbeLayout* d = &probes->layout;
	set->params[0]                  = (float)d->probe_count;
	set->params[1]                  = (float)probes->mip_count;
	set->params[2]                  = (float)d->grid_resolution[0];
	set->params[3]                  = (float)d->grid_resolution[1];
	set->strengths[0]               = d->intensity * d->diffuse_strength;
	set->strengths[1]               = d->intensity * d->specular_strength;
	/* Grid texel coordinates: point i of r at i + 0.5, Blender's volume
	 * probe placing it at (i + 1) / (r + 1) of the box. */
	for (int axis = 0; axis < 3; ++axis) {
		const float r          = (float)d->grid_resolution[axis];
		set->grid_scale[axis]  = (r + 1.0f) / d->grid_size[axis];
		set->grid_offset[axis] = (0.5f - d->grid_center[axis] / d->grid_size[axis]) * (r + 1.0f) - 0.5f;
	}
	set->grid_scale[3] = (float)d->grid_resolution[2];
	return s->probe_set_count++;
}

static const float scene_world_axes[3][3] = { { 1.0f, 0.0f, 0.0f },
											  { 0.0f, 1.0f, 0.0f },
											  { 0.0f, 0.0f, 1.0f } };

/* The instance's bound along the world axes, or NULL when it has no usable
 * bound. */
static const AeronSceneInstanceBox* scene_instance_world_box(const AeronSceneMeshInstance* instance,
															 AeronSceneInstanceBox*        box) {
	return AeronSceneInternal_InstanceBox(instance, scene_world_axes[0], scene_world_axes[1],
										  scene_world_axes[2], box)
			   ? box
			   : NULL;
}

/* The probe set an instance uses: the one it owns, else the first whose
 * extent its bounding sphere overlaps; -1 for none. `box` is the instance's
 * world bound (scene_instance_world_box). */
static int32_t scene_instance_probe_set_for_box(const AeronScene3D* s, const AeronSceneMeshInstance* instance,
												const AeronSceneInstanceBox* box) {
	if (s->probe_set_count == 0) {
		return -1;
	}
	if (instance->probe_set_owner) {
		return instance->probe_set_owner <= s->probe_set_count ? (int32_t)instance->probe_set_owner - 1 : -1;
	}
	if (!box) {
		return -1;
	}
	const float radius =
		sqrtf(box->half[0] * box->half[0] + box->half[1] * box->half[1] + box->half[2] * box->half[2]);
	for (int k = 0; k < s->probe_set_count; ++k) {
		const AeronSceneProbeSet* set         = &s->probe_sets[k];
		const AeronProbeSet*      probes      = set->probes;
		float                     distance_sq = 0.0f;
		for (int row = 0; row < 3; ++row) {
			const float* w = &set->world_to_model[row * 4];
			const float  p = w[0] * box->center[0] + w[1] * box->center[1] + w[2] * box->center[2] + w[3];
			const float  outside =
				fmaxf(fmaxf(probes->bounds_min[row] - p, p - probes->bounds_max[row]), 0.0f);
			distance_sq += outside * outside;
		}
		const float reach = radius * set->metres_per_unit;
		if (distance_sq <= reach * reach) {
			return k;
		}
	}
	return -1;
}

int32_t AeronSceneInternal_InstanceProbeSet(const AeronScene3D* s, const AeronSceneMeshInstance* instance) {
	AeronSceneInstanceBox box;
	return scene_instance_probe_set_for_box(s, instance, scene_instance_world_box(instance, &box));
}

void AeronScene_SetFrameUniformData(AeronScene3D* s, AeronShaderStage stage, uint32_t slot, const void* data,
									uint32_t size) {
	if (!s || !data || size == 0 || size > AERON_SCENE_FRAME_UNIFORM_CAP) {
		if (s && size > AERON_SCENE_FRAME_UNIFORM_CAP) {
			Aeron_LogError("aeron.scene", "frame uniform blob too large (%u > %u)", size,
						   (unsigned)AERON_SCENE_FRAME_UNIFORM_CAP);
		}
		return;
	}
	if (s->frame_uniform_count >= AERON_SCENE_MAX_FRAME_UNIFORMS) {
		Aeron_LogWarn("aeron.scene", "frame uniform cap (%d) hit; dropping", AERON_SCENE_MAX_FRAME_UNIFORMS);
		return;
	}
	AeronSceneFrameUniform* u = &s->frame_uniforms[s->frame_uniform_count++];
	u->stage                  = (uint8_t)stage;
	u->slot                   = slot;
	u->size                   = size;
	memcpy(u->data, data, size);
}

/* Bind the queued frame-uniform blobs — in the color pass, after the
 * BEFORE_OPAQUE hook (hook-side pushes to the same slots must not
 * survive into the instance walk). */
void AeronSceneInternal_BindFrameUniforms(AeronScene3D* s, AeronRenderPass* pass) {
	for (int i = 0; i < s->frame_uniform_count; i++) {
		const AeronSceneFrameUniform* u = &s->frame_uniforms[i];
		Aeron_BindUniformData(pass, (AeronShaderStage)u->stage, u->slot, u->data, u->size);
	}
}

void AeronScene_SetPassHook(AeronScene3D* s, AeronScenePassSlot slot, AeronScenePassHookFn fn, void* user) {
	if (!s || slot < 0 || slot >= AERON_SCENE_HOOK_COUNT) {
		return;
	}
	s->hook_fn[slot]   = fn;
	s->hook_user[slot] = user;
}

void AeronScene_SetAfterMeshes(AeronScene3D* s, AeronSceneAfterMeshesFn fn, void* user) {
	if (!s) {
		return;
	}
	s->after_meshes_fn   = fn;
	s->after_meshes_user = user;
}

static void run_hook(AeronScene3D* s, AeronScenePassSlot slot, AeronCommandBuffer* cmd,
					 AeronRenderPass* pass) {
	if (s->hook_fn[slot]) {
		static const char* labels[AERON_SCENE_HOOK_COUNT] = {
			[AERON_SCENE_HOOK_AFTER_OPAQUE]      = "Game hook: after opaque",
			[AERON_SCENE_HOOK_AFTER_TRANSPARENT] = "Game hook: after transparent",
			[AERON_SCENE_HOOK_BEFORE_POST]       = "Game hook: before post",
			[AERON_SCENE_HOOK_BEFORE_OPAQUE]     = "Game hook: before opaque",
			[AERON_SCENE_HOOK_PREPASS]           = "Game hook: prepass",
			[AERON_SCENE_HOOK_AFTER_UPSCALE]     = "Game hook: after upscale",
		};
		Aeron_GpuDebugMarker(cmd, labels[slot]);
		const int output_space =
			slot == AERON_SCENE_HOOK_AFTER_UPSCALE || slot == AERON_SCENE_HOOK_BEFORE_POST;
		s->hook_fn[slot](cmd, pass, output_space ? s->output_w : s->render_w,
						 output_space ? s->output_h : s->render_h, s->hook_user[slot]);
	}
}

static void scene_run_after_meshes(AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderPass* pass,
								   AeronRenderTarget* color_target) {
	if (!s->after_meshes_fn) {
		return;
	}
	s->after_meshes_fn(cmd, pass, color_target, s->after_meshes_user);
	/* A callback may leave a 2D viewport/scissor active. */
	Aeron_SetViewport(pass, &s->camera.viewport);
	Aeron_SetScissor(pass, &s->camera.viewport);
}

/* Temporal reconstruction needs every representable camera rotation: even
 * subpixel steps accumulate across frames. Motion blur shares the same field
 * so both paths use an exact change test. Treat q and -q as equivalent. */
static int scene_camera_orientation_changed(const AeronScene3D* s) {
	int same    = 1;
	int negated = 1;
	for (int i = 0; i < 4; ++i) {
		same    = same && s->camera.ori[i] == s->mb_prev_ori[i];
		negated = negated && s->camera.ori[i] == -s->mb_prev_ori[i];
	}
	return !same && !negated;
}

static int scene_storage_reserve(void** data, uint32_t* capacity, uint32_t required, size_t element_size) {
	if (*capacity >= required) {
		return 1;
	}
	uint32_t new_capacity = *capacity ? *capacity : 16u;
	while (new_capacity < required) {
		if (new_capacity > UINT32_MAX / 2u) {
			new_capacity = required;
			break;
		}
		new_capacity *= 2u;
	}
	void* replacement = realloc(*data, (size_t)new_capacity * element_size);
	if (!replacement) {
		return 0;
	}
	*data     = replacement;
	*capacity = new_capacity;
	return 1;
}

static uint32_t scene_mesh_table_hash(const AeronSceneMeshTable* table) {
	uintptr_t value = (uintptr_t)table;
	value ^= value >> 17;
	value *= (uintptr_t)0xed5ad4bbU;
	value ^= value >> 11;
	return (uint32_t)value;
}

/* Appends `table` to the frame's storage, packed at its component count (see
 * mesh_table_layout.hlsli), once per distinct pointer. Returns its float4
 * base and component count, or UINT32_MAX. */
static uint32_t scene_register_mesh_table(AeronScene3D* s, const AeronSceneMeshTable* table,
										  uint32_t* out_component_count) {
	if (!table) {
		table = AeronScenePbr_IdentityTable();
	}
	const uint32_t mask = s->mesh_table_hash_capacity - 1u;
	uint32_t       slot = scene_mesh_table_hash(table) & mask;
	for (uint32_t probe = 0; probe < s->mesh_table_hash_capacity; ++probe) {
		if (!s->mesh_table_keys[slot]) {
			const uint32_t n    = table->component_count && table->component_count < AERON_MAX_MESH_COMPONENTS
									  ? table->component_count
									  : AERON_MAX_MESH_COMPONENTS;
			const uint32_t base = s->mesh_table_vec4_count;
			if (s->mesh_table_count * 2u >= s->mesh_table_hash_capacity ||
				!scene_storage_reserve((void**)&s->mesh_table_staging, &s->mesh_table_staging_cap,
									   base + AERON_MESH_TABLE_VEC4(n), sizeof *s->mesh_table_staging)) {
				return UINT32_MAX;
			}
			float (*out)[4]      = s->mesh_table_staging + base;
			const uint32_t lanes = AERON_MESH_LANE_VEC4(n);
			memcpy(out, table->rows, (size_t)n * sizeof table->rows[0]);
			out += 3u * n;
			memcpy(out + AERON_MESH_LANE_VISIBILITY * lanes, table->visibility_packed, lanes * sizeof *out);
			memcpy(out + AERON_MESH_LANE_HIGHLIGHT * lanes, table->highlight_packed, lanes * sizeof *out);
			memcpy(out + AERON_MESH_LANE_MARKINGS * lanes, table->markings_packed, lanes * sizeof *out);
			memcpy(out + AERON_MESH_LANE_EMISSIVE * lanes, table->emissive_packed, lanes * sizeof *out);
			s->mesh_table_count++;
			s->mesh_table_vec4_count += AERON_MESH_TABLE_VEC4(n);
			s->mesh_table_keys[slot]             = table;
			s->mesh_table_values[slot]           = base;
			s->mesh_table_component_counts[slot] = n;
			*out_component_count                 = n;
			return base;
		}
		if (s->mesh_table_keys[slot] == table) {
			*out_component_count = s->mesh_table_component_counts[slot];
			return s->mesh_table_values[slot];
		}
		slot = (slot + 1u) & mask;
	}
	return UINT32_MAX;
}

static int scene_storage_ensure_buffer_usage(AeronBuffer** buffer, uint32_t* capacity, uint32_t required,
											 uint32_t usage, const char* name) {
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
		.memory_usage = AERON_MEMORY_USAGE_DYNAMIC,
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

static int scene_storage_ensure_buffer(AeronBuffer** buffer, uint32_t* capacity, uint32_t required,
									   const char* name) {
	return scene_storage_ensure_buffer_usage(buffer, capacity, required, AERON_BUFFER_USAGE_STORAGE, name);
}

static void scene_storage_report_failure(AeronScene3D* s, const char* resource, uint32_t bytes) {
	if (!s->storage_error_logged) {
		Aeron_LogError("aeron.scene", "%s preparation failed (%u bytes): %s", resource, bytes,
					   SDL_GetError());
		s->storage_error_logged = 1;
	}
}

/* Side and near planes of the camera frustum, from the unjittered
 * view-projection (row-major, clip = M * p; reversed-Z with an infinite far
 * plane, so there is no far plane). The side planes are widened by
 * SCENE_CULL_MARGIN_PX render pixels so temporal jitter cannot reveal a
 * culled edge. Each plane is normalised: dot(xyz, p) + w is the signed
 * distance, positive inside. */
#define SCENE_CULL_MARGIN_PX 2.0f

void AeronSceneInternal_FrustumPlanes(const float view_proj[16], int viewport_width, int viewport_height,
									  float planes[5][4]) {
	const float* m       = view_proj;
	const float  width   = viewport_width > 0 ? (float)viewport_width : 1.0f;
	const float  height  = viewport_height > 0 ? (float)viewport_height : 1.0f;
	const float  widen_x = 1.0f + 2.0f * SCENE_CULL_MARGIN_PX / width;
	const float  widen_y = 1.0f + 2.0f * SCENE_CULL_MARGIN_PX / height;
	for (int k = 0; k < 4; k++) {
		const float w = m[12 + k];
		planes[0][k]  = w * widen_x + m[0 + k]; /* left: x >= -w */
		planes[1][k]  = w * widen_x - m[0 + k]; /* right: x <= w */
		planes[2][k]  = w * widen_y + m[4 + k]; /* y >= -w */
		planes[3][k]  = w * widen_y - m[4 + k]; /* y <= w */
		planes[4][k]  = w - m[8 + k];           /* near: z <= w (eye depth >= near) */
	}
	for (int p = 0; p < 5; p++) {
		const float length =
			sqrtf(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
		const float inverse = length > 0.0f ? 1.0f / length : 0.0f;
		for (int k = 0; k < 4; k++) {
			planes[p][k] *= inverse;
		}
	}
}

static void scene_camera_frustum(const AeronScene3D* s, float planes[5][4]) {
	AeronSceneInternal_FrustumPlanes(s->unjittered_view_proj, s->camera.viewport.width,
									 s->camera.viewport.height, planes);
}

/* Draw-order key of a camera-visible instance: the distance from the camera
 * to its bound, 0 when the bound contains the camera, then the distance to
 * the bound's centre, so the cockpit sorts before an enclosing hangar. */
typedef struct AeronSceneOrderKey {
	float    surface;
	float    center;
	uint16_t index;
} SceneOrderKey;

/* Grows the instances, the shadow-only casters and the arrays that follow
 * their counts to the given capacities. Returns 0 when out of memory; the
 * arrays that grew keep their new size. */
static int scene_reserve_instances(AeronScene3D* s, int instance_capacity, int shadow_only_capacity) {
#define SCENE_GROW(field, count)                                                                             \
	do {                                                                                                     \
		void* grown = realloc((field), (size_t)(count) * sizeof *(field));                                   \
		if (!grown) {                                                                                        \
			return 0;                                                                                        \
		}                                                                                                    \
		(field) = grown;                                                                                     \
	} while (0)
	if (instance_capacity > s->instance_capacity) {
		SCENE_GROW(s->instances, instance_capacity);
		SCENE_GROW(s->prepared_instances, instance_capacity);
		SCENE_GROW(s->camera_order, instance_capacity);
		SCENE_GROW(s->order_keys, instance_capacity);
		SCENE_GROW(s->order_keys_scratch, instance_capacity);
		SCENE_GROW(s->planar_reflection.order, instance_capacity);
	}
	if (shadow_only_capacity > s->shadow_only_capacity) {
		SCENE_GROW(s->shadow_only, shadow_only_capacity);
		SCENE_GROW(s->prepared_shadow_only, shadow_only_capacity);
	}
	const int casters = instance_capacity + shadow_only_capacity;
	if (casters > s->instance_capacity + s->shadow_only_capacity) {
		for (int cascade = 0; cascade < AERON_SCENE_SHADOW_MAX_CASCADES; cascade++) {
			SCENE_GROW(s->shadow_casters[cascade], casters);
		}
		SCENE_GROW(s->receiver_local_shadow_casters, casters);
		SCENE_GROW(s->shadow_object_bounds, casters);
		SCENE_GROW(s->shadow_object_indices, casters);
		SCENE_GROW(s->shadow_bound_valid, casters);
		SCENE_GROW(s->shadow_caster_valid, casters);
	}
#undef SCENE_GROW
	if (instance_capacity > s->instance_capacity) {
		s->instance_capacity = instance_capacity;
	}
	if (shadow_only_capacity > s->shadow_only_capacity) {
		s->shadow_only_capacity = shadow_only_capacity;
	}
	return 1;
}

static void scene_free_instances(AeronScene3D* s) {
	free(s->instances);
	free(s->prepared_instances);
	free(s->camera_order);
	free(s->order_keys);
	free(s->order_keys_scratch);
	free(s->planar_reflection.order);
	free(s->shadow_only);
	free(s->prepared_shadow_only);
	for (int cascade = 0; cascade < AERON_SCENE_SHADOW_MAX_CASCADES; cascade++) {
		free(s->shadow_casters[cascade]);
	}
	free(s->receiver_local_shadow_casters);
	free(s->shadow_object_bounds);
	free(s->shadow_object_indices);
	free(s->shadow_bound_valid);
	free(s->shadow_caster_valid);
	free(s->mesh_table_keys);
	free(s->mesh_table_values);
	free(s->mesh_table_component_counts);
}

/* Makes the mesh table hash at least twice `tables`, a power of two. */
static int scene_reserve_mesh_table_hash(AeronScene3D* s, uint32_t tables) {
	uint32_t capacity = s->mesh_table_hash_capacity ? s->mesh_table_hash_capacity : 1024u;
	while (capacity < 2u * tables) {
		capacity *= 2u;
	}
	if (capacity == s->mesh_table_hash_capacity) {
		return 1;
	}
	const AeronSceneMeshTable** keys = realloc(s->mesh_table_keys, capacity * sizeof *keys);
	if (!keys) {
		return 0;
	}
	s->mesh_table_keys = keys;
	uint32_t* values   = realloc(s->mesh_table_values, capacity * sizeof *values);
	if (!values) {
		return 0;
	}
	s->mesh_table_values = values;
	uint32_t* counts     = realloc(s->mesh_table_component_counts, capacity * sizeof *counts);
	if (!counts) {
		return 0;
	}
	s->mesh_table_component_counts = counts;
	s->mesh_table_hash_capacity    = capacity;
	return 1;
}

static int scene_order_key_less(const SceneOrderKey* x, const SceneOrderKey* y) {
	if (x->surface != y->surface)
		return x->surface < y->surface;
	if (x->center != y->center)
		return x->center < y->center;
	return x->index < y->index;
}

/* Sorts `count` keys with scene_order_key_less, using `scratch`, which holds
 * as many keys. No two keys are equal because their indexes differ, so any
 * correct sort gives the same order. This is a merge sort with an inline
 * comparison because qsort, which calls its comparison through a pointer,
 * was slow here. */
static void scene_sort_order_keys(SceneOrderKey* keys, SceneOrderKey* scratch, int count) {
	enum { RUN = 16 };

	for (int start = 0; start < count; start += RUN) {
		const int end = start + RUN < count ? start + RUN : count;
		for (int i = start + 1; i < end; i++) {
			const SceneOrderKey key = keys[i];
			int                 j   = i;
			while (j > start && scene_order_key_less(&key, &keys[j - 1])) {
				keys[j] = keys[j - 1];
				j--;
			}
			keys[j] = key;
		}
	}
	SceneOrderKey* from = keys;
	SceneOrderKey* to   = scratch;
	for (int width = RUN; width < count; width *= 2) {
		for (int start = 0; start < count; start += 2 * width) {
			const int middle = start + width < count ? start + width : count;
			const int end    = start + 2 * width < count ? start + 2 * width : count;
			int       a      = start;
			int       b      = middle;
			int       out    = start;
			while (a < middle && b < end) {
				to[out++] = scene_order_key_less(&from[b], &from[a]) ? from[b++] : from[a++];
			}
			while (a < middle) {
				to[out++] = from[a++];
			}
			while (b < end) {
				to[out++] = from[b++];
			}
		}
		SceneOrderKey* swap = from;
		from                = to;
		to                  = swap;
	}
	if (from != keys) {
		memcpy(keys, from, (size_t)count * sizeof *keys);
	}
}

/* The larger side of the bound's projection, clipped to the viewport, in
 * render pixels: 0 outside the viewport, FLT_MAX across the near plane or
 * without a usable bound. */
static float scene_instance_screen_size(const AeronScene3D* s, const AeronSceneInstanceBox* box) {
	const float* m      = s->unjittered_view_proj;
	const float  width  = (float)s->camera.viewport.width;
	const float  height = (float)s->camera.viewport.height;
	if (!box) {
		return FLT_MAX;
	}
	float lo[2] = { FLT_MAX, FLT_MAX };
	float hi[2] = { -FLT_MAX, -FLT_MAX };
	for (int c = 0; c < 8; ++c) {
		const float p[3] = { box->center[0] + ((c & 1) ? box->half[0] : -box->half[0]),
							 box->center[1] + ((c & 2) ? box->half[1] : -box->half[1]),
							 box->center[2] + ((c & 4) ? box->half[2] : -box->half[2]) };
		float       clip[4];
		for (int r = 0; r < 4; ++r) {
			clip[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
		}
		if (clip[3] <= 1.0e-4f) {
			return FLT_MAX;
		}
		const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * width;
		const float y = (clip[1] / clip[3] * 0.5f + 0.5f) * height;
		lo[0]         = fminf(lo[0], x);
		lo[1]         = fminf(lo[1], y);
		hi[0]         = fmaxf(hi[0], x);
		hi[1]         = fmaxf(hi[1], y);
	}
	const float w = fminf(hi[0], width) - fmaxf(lo[0], 0.0f);
	const float h = fminf(hi[1], height) - fmaxf(lo[1], 0.0f);
	return w > 0.0f && h > 0.0f ? fmaxf(w, h) : 0.0f;
}

/* An instance without a usable bound stays visible, and sorts last. */
static int scene_instance_in_frustum(const AeronScene3D* s, const AeronSceneInstanceBox* box,
									 const float planes[5][4], SceneOrderKey* key) {
	key->surface = FLT_MAX;
	key->center  = FLT_MAX;
	if (!box) {
		return 1;
	}
	for (int p = 0; p < 5; p++) {
		const float distance = planes[p][0] * box->center[0] + planes[p][1] * box->center[1] +
							   planes[p][2] * box->center[2] + planes[p][3];
		const float reach    = fabsf(planes[p][0]) * box->half[0] + fabsf(planes[p][1]) * box->half[1] +
							   fabsf(planes[p][2]) * box->half[2];
		if (distance < -reach) {
			return 0;
		}
	}
	float center_sq  = 0.0f;
	float surface_sq = 0.0f;
	for (int axis = 0; axis < 3; axis++) {
		const float delta  = box->center[axis] - s->camera.pos[axis];
		const float beyond = fmaxf(fabsf(delta) - box->half[axis], 0.0f);
		center_sq += delta * delta;
		surface_sq += beyond * beyond;
	}
	key->center  = sqrtf(center_sq);
	key->surface = sqrtf(surface_sq);
	return 1;
}

/* Whether a batch can draw a visible instance: its mesh has only plain
 * blended faces and no animation, and the instance has no probe set and no
 * receiver-local shadow, which batches don't bind. */
static int scene_instance_batchable(const AeronSceneMeshInstance*     instance,
									const AeronScenePreparedInstance* prepared) {
	const AeronSceneMesh* m = instance->mesh;
	return m && m->vbo && m->ibo && m->opaque_index_count == 0 && m->mask_index_count == 0 &&
		   m->blend_index_count != 0 && m->blend_screen_index_count == 0 && !m->anim_buffer &&
		   prepared->probe_set < 0 && !instance->velocity_stamp_screens &&
		   (instance->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) == 0;
}

/* Returns the batch for the instance's mesh, cull mode, velocity stamp and
 * emission_only flag, and adds it if needed. Returns -1 when it can't be
 * stored. There are few batches: one per projectile model in view. */
static int32_t scene_instance_batch(AeronScene3D* s, const AeronSceneMeshInstance* instance) {
	for (uint32_t b = s->pbr_batch_count; b-- > 0;) {
		const AeronScenePbrBatch* batch = &s->pbr_batches[b];
		if (batch->mesh == instance->mesh && batch->cull_mode == instance->cull_mode &&
			batch->velocity_stamp == instance->velocity_stamp &&
			batch->emission_only == (instance->emission_only != 0)) {
			return (int32_t)b;
		}
	}
	if (!scene_storage_reserve((void**)&s->pbr_batches, &s->pbr_batch_cap, s->pbr_batch_count + 1u,
							   sizeof *s->pbr_batches)) {
		return -1;
	}
	s->pbr_batches[s->pbr_batch_count] = (AeronScenePbrBatch) { .mesh           = instance->mesh,
																.cull_mode      = instance->cull_mode,
																.velocity_stamp = instance->velocity_stamp,
																.emission_only  = instance->emission_only != 0 };
	return (int32_t)s->pbr_batch_count++;
}

/* The instance's values, as pbr_push_instance_uniforms gives them to one
 * instance. */
static void scene_instance_record(const AeronSceneMeshInstance*     instance,
								  const AeronScenePreparedInstance* prepared,
								  AeronScenePbrInstanceRecord*      out) {
	const AeronSceneMesh* m       = instance->mesh;
	uint32_t              variant = instance->variant;
	if (variant >= m->variant_rows) {
		variant = m->variant_rows ? m->variant_rows - 1u : 0u;
	}
	const int moving = !instance->zero_velocity;
	memcpy(out->model_to_world, instance->transform, sizeof out->model_to_world);
	memcpy(out->prev_model_to_world, moving ? instance->prev_transform : instance->transform,
		   sizeof out->prev_model_to_world);
	out->variant_row_base             = variant * m->variant_groups_per_row;
	out->current_table_base           = prepared->current_table_base;
	out->previous_table_base          = moving ? prepared->previous_table_base : prepared->current_table_base;
	out->table_component_count        = prepared->table_component_count;
	out->receive_shadow = (instance->shadow_flags & AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW) == 0 ? 1u : 0u;
	out->zero_velocity  = moving ? 0u : 1u;
	out->hidden_tag_groups = instance->hidden_tag_groups;
}

/* Groups the batchable visible instances into batches and writes their
 * records, from far to near. Returns the number of records; an instance
 * whose batch can't be stored draws alone. */
static uint32_t scene_prepare_batches(AeronScene3D* s) {
	s->pbr_batch_count = 0;
	uint32_t records   = 0;
	for (int k = s->camera_order_count - 1; k >= 0; --k) {
		const int                   i        = s->camera_order[k];
		AeronScenePreparedInstance* prepared = &s->prepared_instances[i];
		if (scene_instance_batchable(&s->instances[i], prepared)) {
			prepared->batch = scene_instance_batch(s, &s->instances[i]);
			if (prepared->batch >= 0) {
				s->pbr_batches[prepared->batch].count++;
				records++;
			}
		}
	}
	if (records == 0 ||
		!scene_storage_reserve((void**)&s->instance_record_staging, &s->instance_record_staging_cap, records,
							   sizeof *s->instance_record_staging)) {
		for (int k = 0; k < s->camera_order_count; ++k) {
			s->prepared_instances[s->camera_order[k]].batch = -1;
		}
		s->pbr_batch_count = 0;
		return 0;
	}
	uint32_t first = 0;
	for (uint32_t b = 0; b < s->pbr_batch_count; ++b) {
		s->pbr_batches[b].first = first;
		first += s->pbr_batches[b].count;
		s->pbr_batches[b].count = 0;
	}
	for (int k = s->camera_order_count - 1; k >= 0; --k) {
		const int                         i        = s->camera_order[k];
		const AeronScenePreparedInstance* prepared = &s->prepared_instances[i];
		if (prepared->batch >= 0) {
			AeronScenePbrBatch* batch = &s->pbr_batches[prepared->batch];
			scene_instance_record(&s->instances[i], prepared,
								  &s->instance_record_staging[batch->first + batch->count++]);
		}
	}
	return records;
}

int AeronSceneStorage_Prepare(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd) {
		return 0;
	}
	s->storage_ready         = 0;
	s->mesh_table_count      = 0;
	s->mesh_table_vec4_count = 0;
	s->point_light_count     = 0;
	/* The identity table, two tables per instance, one per shadow-only caster
	 * and one per overlay. */
	if (!scene_reserve_mesh_table_hash(s, 1u + 2u * (uint32_t)s->instance_count +
											  (uint32_t)s->shadow_only_count + s->overlay_count)) {
		scene_storage_report_failure(s, "scene.mesh_tables.hash", 0);
		return 0;
	}
	memset(s->mesh_table_keys, 0, s->mesh_table_hash_capacity * sizeof *s->mesh_table_keys);

	uint32_t identity_components = 0;
	if (scene_register_mesh_table(s, AeronScenePbr_IdentityTable(), &identity_components) != 0u) {
		scene_storage_report_failure(s, "scene.mesh_tables.cpu", (uint32_t)sizeof(AeronSceneMeshTable));
		return 0;
	}
	float frustum[5][4];
	scene_camera_frustum(s, frustum);
	SceneOrderKey* order_keys = s->order_keys;
	s->camera_order_count     = 0;
	int tables_dropped        = 0;
	for (int i = 0; i < s->instance_count; ++i) {
		const AeronSceneMeshInstance* instance = &s->instances[i];
		AeronScenePreparedInstance*   prepared = &s->prepared_instances[i];
		memset(prepared, 0, sizeof *prepared);
		SceneOrderKey                key;
		AeronSceneInstanceBox        box_storage;
		const AeronSceneInstanceBox* box = scene_instance_world_box(instance, &box_storage);
		prepared->camera_visible         = (uint32_t)scene_instance_in_frustum(s, box, frustum, &key);
		prepared->screen_size_px         = FLT_MAX;
		if (prepared->camera_visible) {
			prepared->screen_size_px = scene_instance_screen_size(s, box);
			/* A mesh with only blended faces, such as a laser bolt, is thin
			 * but bright: it stays. */
			if (s->small_cull_active && prepared->screen_size_px < s->small_cull_draw_px && instance->mesh &&
				(instance->mesh->opaque_index_count + instance->mesh->mask_index_count) != 0) {
				prepared->camera_visible = 0;
			}
		}
		prepared->probe_set =
			prepared->camera_visible ? scene_instance_probe_set_for_box(s, instance, box) : -1;
		prepared->batch          = -1;
		if (prepared->camera_visible) {
			key.index                           = (uint16_t)i;
			order_keys[s->camera_order_count++] = key;
		}
		prepared->current_table_base =
			scene_register_mesh_table(s, instance->mesh_table, &prepared->table_component_count);
		uint32_t previous_components = 0;
		prepared->previous_table_base =
			instance->prev_mesh_table
				? scene_register_mesh_table(s, instance->prev_mesh_table, &previous_components)
				: prepared->current_table_base;
		/* A table that can't be stored shows the instance unarticulated,
		 * rather than failing the frame. */
		if (prepared->current_table_base == UINT32_MAX) {
			prepared->current_table_base    = 0;
			prepared->table_component_count = identity_components;
			tables_dropped++;
		}
		if (prepared->previous_table_base == UINT32_MAX) {
			prepared->previous_table_base = prepared->current_table_base;
			tables_dropped++;
		}
		/* The shader indexes both tables with the current component range. */
		if (instance->prev_mesh_table && previous_components < prepared->table_component_count) {
			prepared->previous_table_base = prepared->current_table_base;
		}
	}
	scene_sort_order_keys(order_keys, s->order_keys_scratch, s->camera_order_count);
	for (int k = 0; k < s->camera_order_count; ++k) {
		s->camera_order[k] = order_keys[k].index;
	}
	const uint32_t record_count = scene_prepare_batches(s);
	for (int i = 0; i < s->shadow_only_count; ++i) {
		AeronScenePreparedInstance* prepared = &s->prepared_shadow_only[i];
		memset(prepared, 0, sizeof *prepared);
		prepared->current_table_base =
			scene_register_mesh_table(s, s->shadow_only[i].mesh_table, &prepared->table_component_count);
		if (prepared->current_table_base == UINT32_MAX) {
			prepared->current_table_base    = 0;
			prepared->table_component_count = identity_components;
			tables_dropped++;
		}
		prepared->previous_table_base = prepared->current_table_base;
	}
	for (uint32_t i = 0; i < s->overlay_count; ++i) {
		s->overlays[i].mesh_table_base =
			scene_register_mesh_table(s, s->overlays[i].mesh_table, &s->overlays[i].table_component_count);
		if (s->overlays[i].mesh_table_base == UINT32_MAX) {
			s->overlays[i].mesh_table_base       = 0;
			s->overlays[i].table_component_count = identity_components;
			tables_dropped++;
		}
	}
	if (tables_dropped && !s->mesh_tables_dropped_logged) {
		Aeron_LogWarn("aeron.scene", "%d mesh tables couldn't be stored; their instances show unarticulated",
					  tables_dropped);
		s->mesh_tables_dropped_logged = 1;
	}

	const uint32_t point_count = s->light_count > 0 ? (uint32_t)s->light_count : 0u;
	if (point_count > 0 &&
		!scene_storage_reserve((void**)&s->point_light_staging, &s->point_light_staging_cap, point_count,
							   sizeof *s->point_light_staging)) {
		scene_storage_report_failure(s, "scene.point_lights.cpu",
									 point_count * (uint32_t)sizeof(AeronScenePointLightGPU));
		return 0;
	}
	if (point_count > 0 &&
		!scene_storage_reserve((void**)&s->cluster_light_staging, &s->cluster_light_staging_cap, point_count,
							   sizeof *s->cluster_light_staging)) {
		scene_storage_report_failure(s, "scene.cluster_lights.cpu",
									 point_count * (uint32_t)sizeof(AeronSceneClusterLightGPU));
		return 0;
	}
	float view_rotation[9];
	AeronSceneInternal_QuatToMat3(s->camera.ori, view_rotation);
	/* Spheres, then spot lights, then tubes, so the shader takes each in its
	 * own loop. Each kind is sorted by the depth of the lights' bounds. */
	SceneLightOrder order[AERON_SCENE_MAX_LIGHTS];
	float           bounds[AERON_SCENE_MAX_LIGHTS][4];
	uint32_t        kind_counts[3]          = { 0, 0, 0 };
	uint32_t        spots_without_direction = 0;
	for (uint32_t light = 0; light < point_count; ++light) {
		const AeronSceneLight* source = &s->lights[light];
		AeronScene_LightBounds(source, bounds[light], &bounds[light][3]);
		const float dx    = bounds[light][0] - s->camera.pos[0];
		const float dy    = bounds[light][1] - s->camera.pos[1];
		const float dz    = bounds[light][2] - s->camera.pos[2];
		const float depth = view_rotation[6] * dx + view_rotation[7] * dy + view_rotation[8] * dz;
		float       cos_inner, cos_outer, direction[3];
		uint32_t    kind = SCENE_LIGHT_SPHERE;
		if (scene_light_is_tube(source)) {
			kind = SCENE_LIGHT_TUBE;
		} else if (scene_light_spot_cosines(source, &cos_inner, &cos_outer)) {
			if (scene_light_direction(source, 0, direction)) {
				kind = SCENE_LIGHT_SPOT;
			} else {
				spots_without_direction++;
			}
		}
		order[light] = (SceneLightOrder) { kind, isfinite(depth) ? depth : 0.0f, light };
		kind_counts[kind]++;
	}
	if (spots_without_direction) {
		Aeron_LogWarn("aeron.scene", "%u spot lights without a direction; drawn as point lights",
					  spots_without_direction);
	}
	qsort(order, point_count, sizeof order[0], scene_light_order_compare);
	for (uint32_t i = 0; i < point_count; ++i) {
		const AeronSceneLight*     source      = &s->lights[order[i].light];
		const float*               bound       = bounds[order[i].light];
		AeronScenePointLightGPU*   destination = &s->point_light_staging[i];
		AeronSceneClusterLightGPU* cluster     = &s->cluster_light_staging[i];
		destination->position_range[0]         = source->pos[0];
		destination->position_range[1]         = source->pos[1];
		destination->position_range[2]         = source->pos[2];
		/* Infinite for no range: the shader then treats every surface as
		 * out of range. */
		destination->position_range[3] =
			source->radius > 0.0f ? 1.0f / (source->radius * source->radius) : INFINITY;
		destination->color[0]     = source->color[0];
		destination->color[1]     = source->color[1];
		destination->color[2]     = source->color[2];
		const float source_radius = source->source_radius > 1e-3f ? source->source_radius : 1e-3f;
		/* A tube's segment; clusters bound it with a sphere around its
		 * centre. */
		const int is_tube     = scene_light_is_tube(source);
		float     half_length = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			destination->half_axis[axis] = is_tube ? source->half_axis[axis] : 0.0f;
			half_length += destination->half_axis[axis] * destination->half_axis[axis];
		}
		half_length               = sqrtf(half_length);
		destination->half_axis[3] = half_length;
		memset(destination->direction, 0, sizeof destination->direction);
		const int one_sided       = scene_light_direction(source, is_tube, destination->direction);
		destination->direction[3] = is_tube ? 1.0f / half_length : 0.0f;
		/* w: the sphere radius, negative for a one-sided sphere or tube. */
		destination->color[3] = one_sided ? -source_radius : source_radius;
		/* A spot light's falloff, saturate(cos * scale + offset)^2 as
		 * KHR_lights_punctual suggests: the scale in direction.w and the
		 * offset in the sphere's unused half axis. Its loop knows it has a
		 * direction, so its radius stays positive. */
		float cos_inner, cos_outer;
		memset(cluster, 0, sizeof *cluster);
		cluster->view_spot_direction_cos[3] = -1.0f;
		if (order[i].kind == SCENE_LIGHT_SPOT && scene_light_spot_cosines(source, &cos_inner, &cos_outer)) {
			const float scale         = 1.0f / fmaxf(cos_inner - cos_outer, 1e-3f);
			destination->direction[3] = scale;
			destination->half_axis[0] = -cos_outer * scale;
			destination->color[3]     = source_radius;
			/* The cluster build culls the depth bins outside the cone. */
			const float px = source->pos[0] - s->camera.pos[0];
			const float py = source->pos[1] - s->camera.pos[1];
			const float pz = source->pos[2] - s->camera.pos[2];
			for (int row = 0; row < 3; ++row) {
				const float* r = &view_rotation[row * 3];
				cluster->view_spot_position_range[row] = r[0] * px + r[1] * py + r[2] * pz;
				cluster->view_spot_direction_cos[row]  = r[0] * destination->direction[0] +
													   r[1] * destination->direction[1] +
													   r[2] * destination->direction[2];
			}
			cluster->view_spot_position_range[3] = source->radius;
			cluster->view_spot_direction_cos[3]  = cos_outer;
		}
		const float dx = bound[0] - s->camera.pos[0];
		const float dy = bound[1] - s->camera.pos[1];
		const float dz = bound[2] - s->camera.pos[2];
		for (int row = 0; row < 3; ++row) {
			cluster->view_position_range[row] = view_rotation[row * 3 + 0] * dx +
												view_rotation[row * 3 + 1] * dy +
												view_rotation[row * 3 + 2] * dz;
		}
		cluster->view_position_range[3] = bound[3];
	}
	s->point_light_count = point_count;
	s->point_spot_start  = kind_counts[SCENE_LIGHT_SPHERE];
	s->point_tube_start  = kind_counts[SCENE_LIGHT_SPHERE] + kind_counts[SCENE_LIGHT_SPOT];

	if (s->point_light_count == 0) {
		if (!scene_storage_reserve((void**)&s->point_light_staging, &s->point_light_staging_cap, 1u,
								   sizeof *s->point_light_staging)) {
			scene_storage_report_failure(s, "scene.point_lights.cpu",
										 (uint32_t)sizeof(AeronScenePointLightGPU));
			return 0;
		}
		memset(s->point_light_staging, 0, sizeof *s->point_light_staging);
	}

	const uint32_t table_bytes = s->mesh_table_vec4_count * (uint32_t)sizeof *s->mesh_table_staging;
	const uint32_t point_bytes =
		(s->point_light_count ? s->point_light_count : 1u) * (uint32_t)sizeof *s->point_light_staging;
	const uint32_t cluster_light_bytes = s->point_light_count * (uint32_t)sizeof *s->cluster_light_staging;
	if (!scene_storage_ensure_buffer(&s->mesh_table_buffer, &s->mesh_table_buffer_cap, table_bytes,
									 "scene.mesh_tables")) {
		scene_storage_report_failure(s, "scene.mesh_tables", table_bytes);
		return 0;
	}
	if (!scene_storage_ensure_buffer(&s->point_light_buffer, &s->point_light_buffer_cap, point_bytes,
									 "scene.point_lights")) {
		scene_storage_report_failure(s, "scene.point_lights", point_bytes);
		return 0;
	}
	if (cluster_light_bytes > 0 &&
		!scene_storage_ensure_buffer_usage(&s->cluster_light_buffer, &s->cluster_light_buffer_cap,
										   cluster_light_bytes, AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ,
										   "scene.cluster_lights")) {
		scene_storage_report_failure(s, "scene.cluster_lights", cluster_light_bytes);
		return 0;
	}
	const uint32_t screen_bytes = (uint32_t)sizeof s->screen_frame;
	if (!scene_storage_ensure_buffer(&s->screen_frame_buffer, &s->screen_frame_buffer_cap, screen_bytes,
									 "scene.screen_frame")) {
		scene_storage_report_failure(s, "scene.screen_frame", screen_bytes);
		return 0;
	}
	const uint32_t record_bytes = record_count * (uint32_t)sizeof *s->instance_record_staging;
	if (record_bytes > 0 &&
		!scene_storage_ensure_buffer(&s->instance_record_buffer, &s->instance_record_buffer_cap, record_bytes,
									 "scene.instance_records")) {
		scene_storage_report_failure(s, "scene.instance_records", record_bytes);
		return 0;
	}
	AeronBufferUploadDesc uploads[5] = {
		{ s->mesh_table_buffer, 0, s->mesh_table_staging, table_bytes },
		{ s->point_light_buffer, 0, s->point_light_staging, point_bytes },
		{ s->screen_frame_buffer, 0, &s->screen_frame, screen_bytes },
	};
	uint32_t upload_count = 3;
	if (cluster_light_bytes > 0) {
		uploads[upload_count++] = (AeronBufferUploadDesc) { s->cluster_light_buffer, 0,
															s->cluster_light_staging, cluster_light_bytes };
	}
	if (record_bytes > 0) {
		uploads[upload_count++] = (AeronBufferUploadDesc) { s->instance_record_buffer, 0,
															s->instance_record_staging, record_bytes };
	}
	s->storage_ready = Aeron_UploadBufferBatchCmd(cmd, uploads, upload_count);
	if (!s->storage_ready) {
		scene_storage_report_failure(s, "scene.dynamic_storage", table_bytes + point_bytes);
	} else {
		s->storage_error_logged = 0;
	}
	return s->storage_ready;
}

uint32_t AeronSceneStorage_ShadowTableBase(const AeronScene3D* s, uint16_t encoded_caster,
										   uint32_t* component_count) {
	const AeronScenePreparedInstance* prepared = NULL;
	if (s && s->storage_ready) {
		if (encoded_caster < AERON_SCENE_SHADOW_ONLY_BASE) {
			if (encoded_caster < (uint16_t)s->instance_count) {
				prepared = &s->prepared_instances[encoded_caster];
			}
		} else {
			const uint16_t index = (uint16_t)(encoded_caster - AERON_SCENE_SHADOW_ONLY_BASE);
			if (index < (uint16_t)s->shadow_only_count) {
				prepared = &s->prepared_shadow_only[index];
			}
		}
	}
	/* Base 0 is the identity table, registered first at full capacity. */
	*component_count = prepared ? prepared->table_component_count : AERON_MAX_MESH_COMPONENTS;
	return prepared ? prepared->current_table_base : 0u;
}

void AeronSceneStorage_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	Aeron_DestroyBuffer(s->mesh_table_buffer);
	Aeron_DestroyBuffer(s->point_light_buffer);
	Aeron_DestroyBuffer(s->cluster_light_buffer);
	Aeron_DestroyBuffer(s->screen_frame_buffer);
	s->screen_frame_buffer     = NULL;
	s->screen_frame_buffer_cap = 0;
	Aeron_DestroyBuffer(s->instance_record_buffer);
	s->instance_record_buffer     = NULL;
	s->instance_record_buffer_cap = 0;
	free(s->instance_record_staging);
	s->instance_record_staging     = NULL;
	s->instance_record_staging_cap = 0;
	free(s->pbr_batches);
	s->pbr_batches     = NULL;
	s->pbr_batch_count = 0;
	s->pbr_batch_cap   = 0;
	free(s->mesh_table_staging);
	free(s->point_light_staging);
	free(s->cluster_light_staging);
	s->mesh_table_buffer     = NULL;
	s->point_light_buffer    = NULL;
	s->cluster_light_buffer  = NULL;
	s->mesh_table_staging    = NULL;
	s->point_light_staging   = NULL;
	s->cluster_light_staging = NULL;
}

static int scene_render_failure(AeronScene3D* s, AeronCommandBuffer* cmd, const char* message) {
	if (s) {
		s->scene_rt_out          = NULL;
		s->scene_rt_out_borrowed = 0;
		s->mb_tile_valid         = 0;
		s->mb_neighbor_valid     = 0;
	}
	Aeron_CommandBufferSetFailure(cmd, message);
	return 0;
}

static int scene_finalize_output(AeronScene3D* s, AeronCommandBuffer* cmd, int mb_active, int mb_regen,
								 int ssao_active) {
	Aeron_GpuDebugPush(cmd, "Exposure");
	const int exposure_ok = AeronSceneExposure_Update(s, cmd);
	Aeron_GpuDebugPop(cmd);
	if (!exposure_ok) {
		return scene_render_failure(s, cmd, "Scene exposure update failed");
	}
	int temporal_dispatch_ok = 0;
	if (s->temporal_active) {
		char label[64];
		snprintf(label, sizeof label, "FSR 3.1.4 %s", AeronTemporal_ModeName(s->temporal_active_mode));
		Aeron_GpuDebugPush(cmd, label);
		if (AeronSceneTemporal_Dispatch(s, cmd, mb_regen)) {
			temporal_dispatch_ok = 1;
			s->temporal_phase++;
			s->temporal.reset_history = 0;
		}
		Aeron_GpuDebugPop(cmd);
		if (!temporal_dispatch_ok) {
			return scene_render_failure(s, cmd, "Scene temporal dispatch failed");
		}
	}

	if (temporal_dispatch_ok && mb_active) {
		Aeron_GpuDebugPush(cmd, "Motion blur");
		if (mb_regen) {
			s->mb_temporal_motion_valid = AeronScenePost_MbPrepareTemporalMotion(s, cmd);
			if (!s->mb_temporal_motion_valid) {
				Aeron_GpuDebugPop(cmd);
				return scene_render_failure(s, cmd, "Temporal motion preparation failed");
			}
		}
		/* FSR's render-resolution motion, retained from its dispatch. */
		AeronTexture* temporal_motion =
			AeronTemporalUpscaler_RetainedDilatedMotionVectors(s->temporal_upscaler);
		if (s->post.mb_velocity_viz) {
			if (s->mb_temporal_motion_valid && temporal_motion &&
				(!AeronSceneTemporal_EnsureMutableOutput(s, cmd) ||
				 !AeronScenePost_MbVisualizeTemporal(s, cmd, temporal_motion, 1))) {
				Aeron_GpuDebugPop(cmd);
				return scene_render_failure(s, cmd, "Temporal motion visualization failed");
			}
		} else if (s->post.mb_shutter > 0.0f && s->mb_temporal_motion_valid && temporal_motion) {
			if (!AeronScenePost_MbResolve(s, cmd, s->scene_rt_out, temporal_motion, 1)) {
				Aeron_GpuDebugPop(cmd);
				return scene_render_failure(s, cmd, "Temporal motion-blur resolve failed");
			}
			s->scene_rt_out          = s->mb_rt;
			s->scene_rt_out_borrowed = 0;
		}
		Aeron_GpuDebugPop(cmd);
	}

	if (!AeronSceneLensFlare_Visibility(s, cmd) || !AeronSceneLensFlare_DrawSmooth(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene lens flare passes failed");
	}
	if (s->bb_frame_has_lens || s->flare_frame) {
		if (!AeronSceneTemporal_EnsureMutableOutput(s, cmd)) {
			return scene_render_failure(s, cmd, "Scene lens output materialization failed");
		}
		AeronRenderPass* lens = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target   = s->scene_rt_out,
			.viewport       = s->output_camera.viewport,
			.command_buffer = cmd,
			.debug_label    = "Scene lens billboards",
		});
		if (!lens) {
			return scene_render_failure(s, cmd, "Scene lens pass creation failed");
		}
		AeronSceneBb3d_DrawLens(s, lens);
		AeronSceneLensFlare_Draw(s, lens);
		Aeron_EndRenderPass(lens);
	}

	if (s->hook_fn[AERON_SCENE_HOOK_AFTER_UPSCALE]) {
		if (!AeronSceneTemporal_EnsureMutableOutput(s, cmd)) {
			return scene_render_failure(s, cmd, "Scene overlay output materialization failed");
		}
		AeronRenderPass* overlay = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target   = s->scene_rt_out,
			.viewport       = s->output_camera.viewport,
			.command_buffer = cmd,
			.debug_label    = "Scene output-resolution overlays",
		});
		if (!overlay) {
			return scene_render_failure(s, cmd, "Scene output-overlay pass creation failed");
		}
		run_hook(s, AERON_SCENE_HOOK_AFTER_UPSCALE, cmd, overlay);
		Aeron_EndRenderPass(overlay);
	}

	if (ssao_active && s->post.ssao_debug_viz && !AeronScenePost_DebugVisualizeSsao(s, cmd)) {
		return scene_render_failure(s, cmd, "SSAO diagnostic visualization failed");
	}
	if (s->directional_shadow.debug_atlas && !AeronSceneDirectionalShadow_DebugVisualize(s, cmd)) {
		return scene_render_failure(s, cmd, "Directional-shadow diagnostic visualization failed");
	}
	if (s->hook_fn[AERON_SCENE_HOOK_BEFORE_POST]) {
		if (!AeronSceneTemporal_EnsureMutableOutput(s, cmd)) {
			return scene_render_failure(s, cmd, "Scene post-hook output materialization failed");
		}
		run_hook(s, AERON_SCENE_HOOK_BEFORE_POST, cmd, NULL);
	}
	return 1;
}

int AeronScene_Render(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd) {
		return 0;
	}
	s->scene_rt_out          = s->color_rt;
	s->scene_rt_out_borrowed = 0;
	s->screen_layer_active   = 0;

	/* With MSAA, the colour pass has its own depth, so the prepass can leave
	 * instances out; FSR needs every instance's depth and motion. */
	s->small_cull_active = s->sample_count != AERON_SAMPLE_COUNT_1 && s->temporal_active_mode == AERON_TEMPORAL_OFF &&
						   (s->small_cull_prepass_px > 0.0f || s->small_cull_draw_px > 0.0f);

	/* Batched billboards: build + upload this frame's VB (copy pass —
	 * must precede the render passes below). */
	if (!AeronSceneStorage_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene storage preparation failed");
	}
	if (s->bb_count > 0 && !AeronSceneBb3d_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene billboard preparation failed");
	}
	if (!AeronSceneLensFlare_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene lens flare preparation failed");
	}
	if (s->overlay_vertex_count > 0 && !AeronSceneMeshOverlay_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene mesh-overlay preparation failed");
	}
	if (!AeronSceneDirectionalShadow_Prepare(s) || !AeronSceneDirectionalShadow_Render(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene directional-shadow preparation failed");
	}

	const int temporal_requested = s->temporal_active_mode != AERON_TEMPORAL_OFF;
	const int ssao_requested     = s->post.ssao_quality > 0 && s->post.ssao_intensity > 0.0f;
	const int mb_requested       = s->post.mb_quality > 0;
	const int pbr_required       = s->instance_count > 0 || s->shadow_only_count > 0 || temporal_requested ||
								   ssao_requested || mb_requested;
	const int pbr_ok             = !pbr_required || AeronScenePbr_Ensure(s);
	if (!pbr_ok) {
		return scene_render_failure(s, cmd, "Scene PBR resource preparation failed");
	}
	if (pbr_required && !AeronSceneClusteredLights_Build(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene clustered-light preparation failed");
	}
	/* Before the visibility pass and the colour passes, which read them;
	 * the mirrored view's ships take the occlusion grid's light. */
	if (pbr_required) {
		AeronSceneProbeOcclusion_Update(s, cmd);
		AeronScenePlanarReflection_Render(s, cmd);
	}
	/* The GPU starts on the shadows while the rest is recorded. */
	(void)Aeron_FlushCommandBuffer(cmd);
	s->temporal_active = temporal_requested;
	if (temporal_requested &&
		(!s->normal_rt || !AeronSceneTemporal_Ensure(s) ||
		 !AeronScenePbr_Pipeline(s, AERON_PBR_PIPE_PREPASS_TEMPORAL, AERON_CULL_NONE))) {
		return scene_render_failure(s, cmd, "Scene temporal resource preparation failed");
	}
	const int ssao_active = ssao_requested;
	if (ssao_requested &&
		(!s->normal_rt || !s->pbr_pipes[AERON_PBR_PIPE_PREPASS][AERON_CULL_NONE] ||
		 !s->pbr_pipes[AERON_PBR_PIPE_FORWARD][AERON_CULL_NONE] || !AeronScenePost_EnsureSsao(s))) {
		return scene_render_failure(s, cmd, "Scene SSAO resource preparation failed");
	}
	/* Opaque receivers take their directional shadow from the
	 * half-resolution visibility pass, which therefore also runs without
	 * SSAO; the cascade debug view shades it in the material shader. It
	 * needs the prepass's normals: without them, the material shader's
	 * single comparison serves. */
	const int screen_shadow_wanted =
		s->shadow_stats.active && !s->directional_shadow.debug_cascades && s->instance_count > 0;
	if (screen_shadow_wanted && !s->normal_rt && !s->screen_shadow_warned) {
		Aeron_LogWarn("aeron.scene",
					  "directional shadows without a normal render target take one comparison");
		s->screen_shadow_warned = 1;
	}
	const int screen_shadow_active = screen_shadow_wanted && s->normal_rt;
	if (screen_shadow_active && !ssao_active &&
		(!s->pbr_pipes[AERON_PBR_PIPE_PREPASS][AERON_CULL_NONE] ||
		 !s->pbr_pipes[AERON_PBR_PIPE_FORWARD][AERON_CULL_NONE] || !AeronScenePost_EnsureSsao(s))) {
		return scene_render_failure(s, cmd, "Scene shadow visibility resource preparation failed");
	}
	const int visibility_active = ssao_active || screen_shadow_active;
	const int mb_active         = mb_requested;
	if (mb_requested && (!s->normal_rt || !s->pbr_pipes[AERON_PBR_PIPE_PREPASS_VEL][AERON_CULL_NONE] ||
						 !s->pbr_pipes[AERON_PBR_PIPE_FORWARD][AERON_CULL_NONE] ||
						 !AeronScenePost_EnsureMb(s) || !s->mb_camera_fill_pipeline)) {
		return scene_render_failure(s, cmd, "Scene motion-blur resource preparation failed");
	}
	const int msaa_active    = s->sample_count != AERON_SAMPLE_COUNT_1;
	const int prepass_active = visibility_active || mb_active || s->temporal_active || msaa_active;
	s->screen_layer_active   = pbr_ok && scene_screen_layer_wanted(s);
	const int mb_regen       = mb_active && s->mb_velocity_regen;
	const int velocity_write = mb_regen || s->temporal_active;
	if (mb_regen) {
		s->mb_tile_valid     = 0;
		s->mb_neighbor_valid = 0;
	}
	const int mb_resolve = !s->temporal_active && mb_active && !s->post.mb_velocity_viz &&
						   s->post.mb_shutter > 0.0f && s->mb_velocity_valid;

	AeronRenderPass* pass = NULL;
	if (prepass_active) {
		/* ---- Pre-pass: normal (+ velocity when needed) + depth. FSR writes
		 * current vectors every presentation frame; non-temporal blur retains
		 * velocity_rt when the game pose is repeated. */
		AeronRenderTarget* pre_extra[2] = { s->velocity_rt, s->temporal_depth_rt };
		AeronRenderPass*   pre          = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target             = s->normal_rt,
			.extra_color_target_count = s->temporal_active ? 2u : (velocity_write ? 1u : 0u),
			.extra_color_targets      = pre_extra,
			.depth_target             = s->depth_rt,
			.viewport                 = s->camera.viewport,
			.clear_color              = 1,
			.clear_color_rgba         = { 0.0f, 0.0f, 0.0f, 0.0f },
			.clear_depth              = 1,
			.clear_depth_value        = 0.0f,
			.command_buffer           = cmd,
			.debug_label              = "Scene prepass (depth + normal + velocity)",
		});
		if (!pre) {
			return scene_render_failure(s, cmd, "Scene prepass creation failed");
		}
		AeronGraphicsPipeline* camera_fill =
			s->temporal_active ? s->temporal_sky_velocity_pipeline : s->mb_camera_fill_pipeline;
		if (velocity_write && camera_fill && (s->temporal_active || s->post.mb_camera_blur) &&
			scene_camera_orientation_changed(s)) {
			Aeron_GpuDebugMarker(cmd, "Camera velocity fill");

			/* Camera-rotational sky fill — first draw, no depth; seeds
			 * velocity everywhere from the prev-frame view rotation. */
			struct {
				float tan_h_half;
				float tan_v_half;
				float proj_y_offset;
				float proj_x_offset;
				float inv_view_rot[3][4];
				float prev_view_proj[16];
			} cfu;

			memset(&cfu, 0, sizeof cfu);
			cfu.tan_h_half    = tanf(s->camera.h_half_rad);
			cfu.tan_v_half    = tanf(s->camera.v_half_rad);
			cfu.proj_y_offset = s->camera.proj_y_offset;
			cfu.proj_x_offset = s->camera.proj_x_offset;
			float r3[9];
			AeronSceneInternal_QuatToMat3(s->camera.ori, r3); /* world->eye */
			for (int r = 0; r < 3; ++r) {                     /* transpose: eye->world */
				cfu.inv_view_rot[r][0] = r3[0 * 3 + r];
				cfu.inv_view_rot[r][1] = r3[1 * 3 + r];
				cfu.inv_view_rot[r][2] = r3[2 * 3 + r];
			}
			memcpy(cfu.prev_view_proj, s->mb_prev_view_proj, sizeof cfu.prev_view_proj);
			Aeron_BindGraphicsPipeline(pre, camera_fill);
			Aeron_BindUniformData(pre, AERON_SHADER_STAGE_FRAGMENT, 0, &cfu, sizeof cfu);
			Aeron_Draw(pre, 4, 0);
		}
		Aeron_GpuDebugMarker(cmd, "PBR prepass instances");
		if (!AeronScenePbr_DrawInstances(
				s, cmd, pre,
				s->temporal_active ? AERON_PBR_PIPE_PREPASS_TEMPORAL
								   : (velocity_write ? AERON_PBR_PIPE_PREPASS_VEL : AERON_PBR_PIPE_PREPASS),
				/*depth_only=*/1, /*velocity=*/velocity_write, NULL)) {
			Aeron_EndRenderPass(pre);
			return scene_render_failure(s, cmd, "Scene PBR prepass recording failed");
		}
		/* Velocity stamping (batched billboards with prev corners) +
		 * game-side velocity emitters. Only on regen frames — both draw
		 * into the 2-RT layout. */
		if (mb_regen || s->temporal_active) {
			Aeron_GpuDebugMarker(cmd, "Billboard and game velocity");
			AeronSceneBb3d_DrawVelocity(s, pre);
			if (!s->temporal_active) {
				run_hook(s, AERON_SCENE_HOOK_PREPASS, cmd, pre);
			}
		}
		Aeron_EndRenderPass(pre);

		if (visibility_active) {
			Aeron_GpuDebugPush(cmd, "AO and shadow visibility");
			const int visibility_ok = AeronScenePost_RunSsao(s, cmd, ssao_active);
			Aeron_GpuDebugPop(cmd);
			if (!visibility_ok) {
				return scene_render_failure(s, cmd, "Scene visibility recording failed");
			}
		}

		/* ---- Forward color pass. At 1x it reuses prepass depth with
		 * EQUAL/no-write. MSAA clears independent transient depth and
		 * resolves only color because SDL GPU does not resolve depth. */
		pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target         = msaa_active ? s->msaa_color_rt : s->color_rt,
			.color_resolve_target = msaa_active ? s->color_rt : NULL,
			.depth_target         = msaa_active ? s->msaa_depth_rt : s->depth_rt,
			.viewport             = s->camera.viewport,
			.clear_color          = 1,
			.clear_color_rgba  = { s->clear_rgba[0], s->clear_rgba[1], s->clear_rgba[2], s->clear_rgba[3] },
			.clear_depth       = msaa_active,
			.clear_depth_value = 0.0f,
			/* The screen layer draws against this depth after the pass. */
			.discard_depth  = msaa_active && !s->screen_layer_active,
			.command_buffer = cmd,
			.debug_label    = "Scene forward color",
		});
		if (!pass) {
			return scene_render_failure(s, cmd, "Scene forward pass creation failed");
		}
		if (!scene_draw_sky(s, cmd, pass)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene sky recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_BEFORE_OPAQUE, cmd, pass);
		AeronSceneInternal_BindFrameUniforms(s, pass);
		Aeron_GpuDebugMarker(cmd, "Sky billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_SKY);
		if (s->sky_overlay) {
			if (!scene_draw_sky_overlay(s, cmd, pass)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene sky overlay recording failed");
			}
			AeronSceneInternal_BindFrameUniforms(s, pass);
		}
		Aeron_GpuDebugMarker(cmd, "PBR forward instances");
		if (!AeronScenePbr_DrawInstances(s, cmd, pass, AERON_PBR_PIPE_FORWARD, /*depth_only=*/0,
										 /*velocity=*/0,
										 visibility_active ? Aeron_RenderTargetGetTexture(s->ao_rt) : NULL)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene PBR forward recording failed");
		}
		Aeron_GpuDebugMarker(cmd, "PBR back-face fill");
		if (!AeronScenePbr_DrawBackfaceFill(s, pass)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene PBR back-face fill recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_AFTER_OPAQUE, cmd, pass);
		Aeron_GpuDebugMarker(cmd, "Mesh overlays");
		AeronSceneMeshOverlay_Draw(s, pass,
								   visibility_active ? Aeron_RenderTargetGetTexture(s->ao_rt) : NULL);
		Aeron_GpuDebugMarker(cmd, "PBR transparent instances");
		if (!AeronScenePbr_DrawTransparentInstances(s, pass, AERON_PBR_PIPE_FORWARD,
													visibility_active ? Aeron_RenderTargetGetTexture(s->ao_rt)
																	  : NULL,
													/*layer=*/0)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene PBR transparent recording failed");
		}
		scene_run_after_meshes(s, cmd, pass, msaa_active ? s->msaa_color_rt : s->color_rt);
		Aeron_GpuDebugMarker(cmd, "Overlay billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_OVERLAY);

		/* Velocity-buffer debug viz — false-colour over the scene. */
		if (!s->temporal_active && mb_active && s->post.mb_velocity_viz && s->mb_velocity_viz_pipeline) {
			Aeron_BindGraphicsPipeline(pass, s->mb_velocity_viz_pipeline);
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
									 Aeron_RenderTargetGetTexture(s->velocity_rt), s->post_linear_sampler);
			float viz_u[4] = { 20.0f, 0.0f, 0.0f, 0.0f }; /* gain */
			Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, viz_u, sizeof viz_u);
			Aeron_Draw(pass, 4, 0);
		}

		/* Motion-blur resolve: close the color pass and gather color+velocity
		 * into mb_rt. Reopen it only when a late hook needs to composite a
		 * sharp overlay over the blurred scene. */
		if (mb_resolve) {
			Aeron_EndRenderPass(pass);
			Aeron_GpuDebugPush(cmd, "Motion blur");
			const int resolved = AeronScenePost_MbResolve(s, cmd, s->color_rt,
														  Aeron_RenderTargetGetTexture(s->velocity_rt), 0);
			Aeron_GpuDebugPop(cmd);
			if (!resolved) {
				return scene_render_failure(s, cmd, "Scene motion-blur resolve failed");
			}
			s->scene_rt_out          = s->mb_rt;
			s->scene_rt_out_borrowed = 0;
			pass                     = NULL;
			if (s->hook_fn[AERON_SCENE_HOOK_AFTER_TRANSPARENT]) {
				pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
					.color_target   = s->scene_rt_out,
					.depth_target   = s->depth_rt,
					.viewport       = s->camera.viewport,
					.command_buffer = cmd,
					.debug_label    = "Scene sharp overlays after motion blur",
				});
				if (!pass) {
					return scene_render_failure(s, cmd, "Scene sharp-overlay pass creation failed");
				}
			}
		}
	} else {
		/* ---- Monolithic single color pass. No normal prepass is needed
		 * because no active effect consumes the normal G-buffer. */
		pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target      = s->color_rt,
			.depth_target      = s->depth_rt,
			.viewport          = s->camera.viewport,
			.clear_color       = 1,
			.clear_color_rgba  = { s->clear_rgba[0], s->clear_rgba[1], s->clear_rgba[2], s->clear_rgba[3] },
			.clear_depth       = 1,
			.clear_depth_value = 0.0f,
			.command_buffer    = cmd,
			.debug_label       = "Scene main color",
		});
		if (!pass) {
			return scene_render_failure(s, cmd, "Scene main pass creation failed");
		}
		if (!scene_draw_sky(s, cmd, pass)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene sky recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_BEFORE_OPAQUE, cmd, pass);
		AeronSceneInternal_BindFrameUniforms(s, pass);
		Aeron_GpuDebugMarker(cmd, "Sky billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_SKY);
		if (s->sky_overlay) {
			if (!scene_draw_sky_overlay(s, cmd, pass)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene sky overlay recording failed");
			}
			AeronSceneInternal_BindFrameUniforms(s, pass);
		}
		if (pbr_ok) {
			Aeron_GpuDebugMarker(cmd, "PBR instances");
			if (!AeronScenePbr_DrawInstances(s, cmd, pass, AERON_PBR_PIPE_MESH,
											 /*depth_only=*/0, /*velocity=*/0, NULL)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene PBR mesh recording failed");
			}
			Aeron_GpuDebugMarker(cmd, "PBR back-face fill");
			if (!AeronScenePbr_DrawBackfaceFill(s, pass)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene PBR back-face fill recording failed");
			}
		}
		run_hook(s, AERON_SCENE_HOOK_AFTER_OPAQUE, cmd, pass);
		Aeron_GpuDebugMarker(cmd, "Mesh overlays");
		AeronSceneMeshOverlay_Draw(s, pass, NULL);
		if (pbr_ok) {
			Aeron_GpuDebugMarker(cmd, "PBR transparent instances");
			if (!AeronScenePbr_DrawTransparentInstances(s, pass, AERON_PBR_PIPE_MESH, NULL, /*layer=*/0)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene PBR transparent recording failed");
			}
		}
		scene_run_after_meshes(s, cmd, pass, s->color_rt);
		Aeron_GpuDebugMarker(cmd, "Overlay billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_OVERLAY);
	}

	/* AFTER_TRANSPARENT remains a render-resolution world hook. Motion
	 * blur may already have reopened the final render-resolution pass. */
	if (pass) {
		run_hook(s, AERON_SCENE_HOOK_AFTER_TRANSPARENT, cmd, pass);
		Aeron_EndRenderPass(pass);
	}

	if (s->screen_layer_active) {
		Aeron_GpuDebugPush(cmd, "Screen layer");
		const int layer_ok =
			scene_draw_screen_layer(s, cmd, prepass_active ? AERON_PBR_PIPE_FORWARD : AERON_PBR_PIPE_MESH);
		Aeron_GpuDebugPop(cmd);
		if (!layer_ok) {
			return scene_render_failure(s, cmd, "Scene screen layer recording failed");
		}
	}

	return scene_finalize_output(s, cmd, mb_active, mb_regen, ssao_active);
}

/* ---- accessors ---- */

AeronTexture* AeronScene_ColorTexture(AeronScene3D* s) {
	return s ? Aeron_RenderTargetGetTexture(s->color_rt) : NULL;
}

AeronRenderTarget* AeronScene_ColorRt(AeronScene3D* s) { return s ? s->color_rt : NULL; }

AeronDepthTarget* AeronScene_DepthRt(AeronScene3D* s) { return s ? s->depth_rt : NULL; }

float AeronScene_DepthAtDistance(const AeronScene3D* s, float distance) {
	if (!s || !(distance > 0.0f)) {
		return 0.0f;
	}
	/* Reversed-Z with an infinite far plane: the projection's z is the near
	 * distance and its w the view distance (scene_mat4_perspective_reverse_z_xy). */
	const float near_z = s->camera.near_z > 0.0f ? s->camera.near_z : 0.001f;
	return near_z / distance;
}

AeronSampleCount AeronScene_SampleCount(const AeronScene3D* s) {
	return s ? s->sample_count : AERON_SAMPLE_COUNT_1;
}

AeronRenderTarget* AeronScene_NormalRt(AeronScene3D* s) { return s ? s->normal_rt : NULL; }

void AeronScene_RtDims(const AeronScene3D* s, int* w, int* h) {
	if (w) {
		*w = s ? s->output_w : 0;
	}
	if (h) {
		*h = s ? s->output_h : 0;
	}
}

void AeronScene_RenderDims(const AeronScene3D* s, int* w, int* h) {
	if (w) {
		*w = s ? s->render_w : 0;
	}
	if (h) {
		*h = s ? s->render_h : 0;
	}
}

const float* AeronScene_ViewProj(const AeronScene3D* s) { return s ? s->unjittered_view_proj : NULL; }

const float* AeronScene_JitteredViewProj(const AeronScene3D* s) { return s ? s->jittered_view_proj : NULL; }

void AeronScene_ComputeViewProj(const AeronSceneCamera* camera, float out[16]) {
	if (!camera || !out) {
		return;
	}
	float proj[16];
	float view[16];
	scene_mat4_perspective_reverse_z_xy(proj, camera->h_half_rad, camera->v_half_rad,
										camera->near_z > 0.0f ? camera->near_z : 0.001f,
										camera->proj_x_offset, camera->proj_y_offset);
	scene_mat4_view(view, camera->ori, camera->pos);
	scene_mat4_mul(out, proj, view);
}
