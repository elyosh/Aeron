/*
 * AeronScene3D — see aeron/scene/scene3d.h.
 *
 * Camera math uses a reversed-Z perspective projection and quaternion
 * view transform.
 */

#include "internal.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char AeronSceneMeshTableSizeCheck[
	sizeof(AeronSceneMeshTable) == AERON_MESH_TABLE_STRIDE_VEC4 * 16 ? 1 : -1];
typedef char AeronSceneLightGPUSizeCheck[sizeof(AeronSceneLightGPU) == 32 ? 1 : -1];
typedef char AeronScenePointLightGPUSizeCheck[sizeof(AeronScenePointLightGPU) == 32 ? 1 : -1];

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

static int scene_create_world_samplers(AeronScene3D* s) {
	for (int mode = AERON_TEMPORAL_OFF; mode <= AERON_TEMPORAL_PERFORMANCE; ++mode) {
		const AeronSamplerDesc desc = scene_world_sampler_desc(scene_mode_mip_lod_bias(s, mode));
		s->pbr_samplers[mode]       = Aeron_CreateSampler(&desc);
		if (!s->pbr_samplers[mode]) {
			scene_release_sampler_set(s->pbr_samplers);
			return 0;
		}
	}
	return 1;
}

static void scene_select_world_samplers(AeronScene3D* s) {
	const int mode =
		scene_temporal_mode_valid(s->temporal_active_mode) ? s->temporal_active_mode : AERON_TEMPORAL_OFF;
	s->pbr_sampler  = s->pbr_samplers[mode];
	s->mesh_sampler = s->mesh_sampler_source ? s->mesh_samplers[mode] : NULL;
}

static void scene_release_targets(AeronScene3D* s) {
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
		Aeron_LogError("aeron.scene", "failed to configure temporal mode %s", AeronTemporal_ModeName(requested));
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
	if (!scene_calculate_render_dims(s, s->temporal_active_mode, &s->render_w, &s->render_h)) {
		free(s);
		return NULL;
	}
	s->clear_rgba[0] = 0.01f; /* deep-space baseline */
	s->clear_rgba[1] = 0.015f;
	s->clear_rgba[2] = 0.03f;
	s->clear_rgba[3] = 1.0f;

	if (!scene_create_targets(s)) {
		AeronScene_Destroy(s);
		return NULL;
	}
	/* World samplers carry AMD's per-mode mip bias. The atlas shaders handle
	 * UV wrapping through their sub-rect transform and frac operation. */
	if (!scene_create_world_samplers(s)) {
		AeronScene_Destroy(s);
		return NULL;
	}
	scene_select_world_samplers(s);
	return s;
}

void AeronScene_Destroy(AeronScene3D* s) {
	if (!s) {
		return;
	}
	AeronSceneBb3d_Release(s);
	AeronSceneMeshOverlay_Release(s);
	AeronSceneDirectionalShadow_Release(s);
	AeronSceneStorage_Release(s);
	AeronScenePbr_Release(s);
	AeronSceneTemporal_Release(s);
	AeronScenePost_Release(s);
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
	s->frame_uniform_count = 0;
	s->pbr_debug_views     = 0;
	memset(&s->directional_shadow, 0, sizeof s->directional_shadow);
	memset(&s->shadow_uniform, 0, sizeof s->shadow_uniform);
	memset(&s->shadow_stats, 0, sizeof s->shadow_stats);
	s->shadow_only_count    = 0;
	s->shadow_only_dropped  = 0;
	s->sky_cube             = NULL;
	s->bb_count             = 0;
	s->bb_dropped           = 0;
	s->bb_frame_verts       = 0;
	s->bb_frame_has_vel     = 0;
	s->overlay_count        = 0;
	s->overlays_dropped     = 0;
	s->overlay_vertex_count = 0;
	s->overlay_frame_ready  = 0;
	s->storage_ready        = 0;
	/* Default motion context: prev = current (zero camera velocity). */
	memcpy(s->mb_prev_view_proj, s->unjittered_view_proj, sizeof s->mb_prev_view_proj);
	s->mb_velocity_regen     = 0;
	s->scene_rt_out          = s->color_rt;
	s->scene_rt_out_borrowed = 0;
	return 1;
}

void AeronScene_AddMeshInstance(AeronScene3D* s, const AeronSceneMeshInstance* instance) {
	if (!s || !instance || !instance->mesh) {
		return;
	}
	if (s->instance_count >= AERON_SCENE_MAX_INSTANCES) {
		if (!s->instances_dropped) {
			Aeron_LogWarn("aeron.scene", "instance cap (%d) hit; dropping", AERON_SCENE_MAX_INSTANCES);
		}
		s->instances_dropped++;
		return;
	}
	s->instances[s->instance_count++] = *instance;
}

void AeronScene_AddLight(AeronScene3D* s, const AeronSceneLight* light) {
	if (!s || !light || s->light_count >= AERON_SCENE_MAX_LIGHTS) {
		return;
	}
	s->lights[s->light_count++] = *light;
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
	if (s->shadow_only_count >= AERON_SCENE_MAX_SHADOW_ONLY) {
		if (!s->shadow_only_dropped) {
			Aeron_LogWarn("aeron.scene", "shadow-only caster cap (%d) hit; dropping",
						  AERON_SCENE_MAX_SHADOW_ONLY);
		}
		s->shadow_only_dropped++;
		return;
	}
	s->shadow_only[s->shadow_only_count++] = *instance;
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
	const int previous_fsr_direct_motion = s->post.mb_fsr_direct_motion;
	if (post) {
		s->post = *post;
	} else {
		memset(&s->post, 0, sizeof s->post);
	}
	if (s->post.mb_fsr_direct_motion != previous_fsr_direct_motion) {
		AeronSceneTemporal_Release(s);
		AeronScenePost_Release(s);
		s->temporal.reset_history    = 1;
		s->mb_temporal_motion_valid  = 0;
		s->mb_temporal_motion_direct = 0;
		s->mb_temporal_tile_valid    = 0;
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
			s->mb_temporal_motion_valid  = 0;
			s->mb_temporal_motion_direct = 0;
			s->mb_temporal_tile_valid    = 0;
		}
	} else {
		memset(&s->temporal, 0, sizeof s->temporal);
		s->temporal_phase            = 0;
		s->mb_temporal_motion_valid  = 0;
		s->mb_temporal_motion_direct = 0;
		s->mb_temporal_tile_valid    = 0;
	}
}

int AeronScene_GetTemporalProfileInfo(const AeronScene3D* s, AeronTemporalProfileInfo* info) {
	if (!info) {
		return 0;
	}
	memset(info, 0, sizeof *info);
	return s && s->temporal_upscaler ? AeronTemporalUpscaler_GetProfileInfo(s->temporal_upscaler, info) : 0;
}

void AeronScene_SetMotionContext(AeronScene3D* s, const float prev_view_proj[16], int velocity_regen) {
	if (!s) {
		return;
	}
	if (prev_view_proj) {
		memcpy(s->mb_prev_view_proj, prev_view_proj, sizeof s->mb_prev_view_proj);
	} else {
		memcpy(s->mb_prev_view_proj, s->unjittered_view_proj, sizeof s->mb_prev_view_proj);
		s->mb_velocity_valid         = 0;
		s->mb_temporal_motion_valid  = 0;
		s->mb_temporal_motion_direct = 0;
		s->mb_temporal_tile_valid    = 0;
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

void AeronScene_SetClearColor(AeronScene3D* s, const float rgba[4]) {
	if (s && rgba) {
		memcpy(s->clear_rgba, rgba, sizeof s->clear_rgba);
	}
}

void AeronScene_SetSkyCube(AeronScene3D* s, AeronTexture* cube, const float world_to_cube[9],
						   float exposure) {
	if (!s) {
		return;
	}
	s->sky_cube     = cube;
	s->sky_exposure = exposure;
	if (world_to_cube) {
		memcpy(s->sky_world_to_cube, world_to_cube, sizeof s->sky_world_to_cube);
	} else {
		static const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		memcpy(s->sky_world_to_cube, ident, sizeof s->sky_world_to_cube);
	}
}

/* ---- sky cube (AeronScene_SetSkyCube) ----
 *
 * Fullscreen-triangle cube-map sky, first draw of the color pass.
 * No vertex buffer (the VS synthesizes the triangle from SV_VertexID);
 * reversed-Z far plane (z = 0), depth GE test, write OFF — valid in
 * both pass shapes: against the cleared 0.0 depth of the monolithic
 * pass, and against the prepass-laid depth of the forward pass (mesh
 * pixels carry z > 0 and reject the sky). */

static AeronGraphicsPipeline* scene_sky_pipeline(AeronScene3D* s, AeronShader* fs) {
	AeronColorTargetStateDesc cts[1] = { 0 };
	cts[0].format                    = s->color_format;
	cts[0].blend                     = AeronSceneInternal_BlendOpaque();
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
static void scene_sky_bind_vs(AeronScene3D* s, AeronRenderPass* pass) {
	float r3[9];
	AeronSceneInternal_QuatToMat3(s->camera.ori, r3); /* world->eye */
	struct {
		float rot[3][4];
		float unproj[4];
	} vsu;
	memset(&vsu, 0, sizeof vsu);
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			vsu.rot[r][c] = s->sky_world_to_cube[r * 3 + 0] * r3[c * 3 + 0] +
							s->sky_world_to_cube[r * 3 + 1] * r3[c * 3 + 1] +
							s->sky_world_to_cube[r * 3 + 2] * r3[c * 3 + 2];
		}
	}
	vsu.unproj[0] = tanf(s->camera.h_half_rad);
	vsu.unproj[1] = tanf(s->camera.v_half_rad);
	scene_jittered_projection_offsets(s, &vsu.unproj[2], &vsu.unproj[3]);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vsu, sizeof vsu);
}

static int scene_draw_sky(AeronScene3D* s, AeronRenderPass* pass) {
	if (!s->sky_cube) {
		return 1;
	}
	if (!scene_sky_ensure(s)) {
		return 0;
	}
	if (!s->sky_pipe) {
		s->sky_pipe = scene_sky_pipeline(s, s->sky_fs);
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
	scene_sky_bind_vs(s, pass);

	float psu[4] = { s->sky_exposure > 0.0f ? s->sky_exposure : 1.0f, 0.0f, 0.0f, 0.0f };
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, psu, sizeof psu);

	Aeron_Draw(pass, 3, 0);
	return 1;
}

int AeronScene_SetMeshSampler(AeronScene3D* s, AeronSampler* sampler) {
	if (!s) {
		return 0;
	}
	if (sampler == s->mesh_sampler_source) {
		return 1;
	}
	if (!sampler) {
		scene_release_sampler_set(s->mesh_samplers);
		s->mesh_sampler_source = NULL;
		scene_select_world_samplers(s);
		return 1;
	}

	AeronSamplerDesc base_desc;
	if (!Aeron_SamplerGetDesc(sampler, &base_desc)) {
		return 0;
	}
	AeronSampler* replacements[AERON_TEMPORAL_PERFORMANCE + 1] = { 0 };
	for (int mode = AERON_TEMPORAL_OFF; mode <= AERON_TEMPORAL_PERFORMANCE; ++mode) {
		AeronSamplerDesc desc = base_desc;
		desc.mip_lod_bias += scene_mode_mip_lod_bias(s, (AeronTemporalMode)mode);
		replacements[mode] = Aeron_CreateSampler(&desc);
		if (!replacements[mode]) {
			scene_release_sampler_set(replacements);
			Aeron_LogError("aeron.scene", "failed to create temporal mesh sampler variants");
			return 0;
		}
	}
	scene_release_sampler_set(s->mesh_samplers);
	memcpy(s->mesh_samplers, replacements, sizeof replacements);
	s->mesh_sampler_source = sampler;
	scene_select_world_samplers(s);
	return 1;
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
		Aeron_LogWarn("aeron.scene", "frame uniform cap (%d) hit; dropping",
					  AERON_SCENE_MAX_FRAME_UNIFORMS);
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
static void scene_bind_frame_uniforms(AeronScene3D* s, AeronRenderPass* pass) {
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

static int scene_storage_reserve(void** data, uint32_t* capacity, uint32_t required,
								 size_t element_size) {
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
	return (uint32_t)value & (AERON_SCENE_MESH_TABLE_HASH_CAP - 1u);
}

static uint32_t scene_register_mesh_table(AeronScene3D* s, const AeronSceneMeshTable* table) {
	if (!table) {
		table = AeronScenePbr_IdentityTable();
	}
	uint32_t slot = scene_mesh_table_hash(table);
	for (uint32_t probe = 0; probe < AERON_SCENE_MESH_TABLE_HASH_CAP; ++probe) {
		if (!s->mesh_table_keys[slot]) {
			if (s->mesh_table_count >= AERON_SCENE_MAX_MESH_TABLES ||
				!scene_storage_reserve((void**)&s->mesh_table_staging,
									 &s->mesh_table_staging_cap,
									 s->mesh_table_count + 1u,
									 sizeof *s->mesh_table_staging)) {
				return UINT32_MAX;
			}
			const uint32_t index       = s->mesh_table_count++;
			s->mesh_table_keys[slot]   = table;
			s->mesh_table_values[slot] = index;
			s->mesh_table_staging[index] = *table;
			return index;
		}
		if (s->mesh_table_keys[slot] == table) {
			return s->mesh_table_values[slot];
		}
		slot = (slot + 1u) & (AERON_SCENE_MESH_TABLE_HASH_CAP - 1u);
	}
	return UINT32_MAX;
}

static int scene_storage_ensure_buffer(AeronBuffer** buffer, uint32_t* capacity,
									   uint32_t required, const char* name) {
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
	AeronBuffer* replacement = Aeron_CreateBuffer(&(AeronBufferDesc){
		.size         = new_capacity,
		.usage        = AERON_BUFFER_USAGE_STORAGE,
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

static void scene_storage_report_failure(AeronScene3D* s, const char* resource, uint32_t bytes) {
	if (!s->storage_error_logged) {
		Aeron_LogError("aeron.scene", "%s preparation failed (%u bytes): %s", resource, bytes, SDL_GetError());
		s->storage_error_logged = 1;
	}
}

int AeronSceneStorage_Prepare(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || !cmd) {
		return 0;
	}
	s->storage_ready     = 0;
	s->mesh_table_count  = 0;
	s->local_light_count = 0;
	s->point_light_count = 0;
	memset(s->mesh_table_keys, 0, sizeof s->mesh_table_keys);

	if (scene_register_mesh_table(s, AeronScenePbr_IdentityTable()) != 0u) {
		scene_storage_report_failure(s, "scene.mesh_tables.cpu",
									(uint32_t)sizeof(AeronSceneMeshTable));
		return 0;
	}
	for (int i = 0; i < s->instance_count; ++i) {
		const AeronSceneMeshInstance* instance = &s->instances[i];
		AeronScenePreparedInstance*   prepared = &s->prepared_instances[i];
		memset(prepared, 0, sizeof *prepared);
		prepared->current_table_index = scene_register_mesh_table(s, instance->mesh_table);
		const AeronSceneMeshTable* previous =
			instance->prev_mesh_table ? instance->prev_mesh_table : instance->mesh_table;
		prepared->previous_table_index = scene_register_mesh_table(s, previous);
		if (prepared->current_table_index == UINT32_MAX ||
			prepared->previous_table_index == UINT32_MAX) {
			scene_storage_report_failure(
				s, "scene.mesh_tables.cpu",
				(s->mesh_table_count + 1u) * (uint32_t)sizeof(AeronSceneMeshTable));
			return 0;
		}
		if (!instance->no_local_lights && instance->lights) {
			uint32_t count = instance->lights->light_count;
			if (count > 16u) {
				count = 16u;
			}
			if (count > 0) {
				if (!scene_storage_reserve((void**)&s->local_light_staging,
										 &s->local_light_staging_cap,
										 s->local_light_count + count,
										 sizeof *s->local_light_staging)) {
					scene_storage_report_failure(
						s, "scene.local_lights.cpu",
						(s->local_light_count + count) * (uint32_t)sizeof(AeronSceneLightGPU));
					return 0;
				}
				prepared->local_light_base   = s->local_light_count;
				prepared->local_light_count = count;
				memcpy(&s->local_light_staging[s->local_light_count], instance->lights->lights,
					   (size_t)count * sizeof *s->local_light_staging);
				s->local_light_count += count;
			}
		}
	}
	for (int i = 0; i < s->shadow_only_count; ++i) {
		AeronScenePreparedInstance* prepared = &s->prepared_shadow_only[i];
		memset(prepared, 0, sizeof *prepared);
		prepared->current_table_index =
			scene_register_mesh_table(s, s->shadow_only[i].mesh_table);
		if (prepared->current_table_index == UINT32_MAX) {
			scene_storage_report_failure(
				s, "scene.mesh_tables.cpu",
				(s->mesh_table_count + 1u) * (uint32_t)sizeof(AeronSceneMeshTable));
			return 0;
		}
		prepared->previous_table_index = prepared->current_table_index;
	}
	for (uint32_t i = 0; i < s->overlay_count; ++i) {
		s->overlays[i].mesh_table_index =
			scene_register_mesh_table(s, s->overlays[i].mesh_table);
		if (s->overlays[i].mesh_table_index == UINT32_MAX) {
			scene_storage_report_failure(
				s, "scene.mesh_tables.cpu",
				(s->mesh_table_count + 1u) * (uint32_t)sizeof(AeronSceneMeshTable));
			return 0;
		}
	}

	const uint32_t point_count = s->light_count > 0 ? (uint32_t)s->light_count : 0u;
	if (point_count > 0 &&
		!scene_storage_reserve((void**)&s->point_light_staging,
							 &s->point_light_staging_cap, point_count,
							 sizeof *s->point_light_staging)) {
		scene_storage_report_failure(
			s, "scene.point_lights.cpu",
			point_count * (uint32_t)sizeof(AeronScenePointLightGPU));
		return 0;
	}
	for (uint32_t i = 0; i < point_count; ++i) {
		const AeronSceneLight* source = &s->lights[i];
		AeronScenePointLightGPU* destination = &s->point_light_staging[i];
		destination->position_range[0] = source->pos[0];
		destination->position_range[1] = source->pos[1];
		destination->position_range[2] = source->pos[2];
		destination->position_range[3] = source->radius;
		destination->color[0] = source->color[0];
		destination->color[1] = source->color[1];
		destination->color[2] = source->color[2];
		destination->color[3] = 0.0f;
	}
	s->point_light_count = point_count;

	if (s->local_light_count == 0) {
		if (!scene_storage_reserve((void**)&s->local_light_staging,
								 &s->local_light_staging_cap, 1u,
								 sizeof *s->local_light_staging)) {
			scene_storage_report_failure(s, "scene.local_lights.cpu",
										(uint32_t)sizeof(AeronSceneLightGPU));
			return 0;
		}
		memset(s->local_light_staging, 0, sizeof *s->local_light_staging);
	}
	if (s->point_light_count == 0) {
		if (!scene_storage_reserve((void**)&s->point_light_staging,
								 &s->point_light_staging_cap, 1u,
								 sizeof *s->point_light_staging)) {
			scene_storage_report_failure(s, "scene.point_lights.cpu",
										(uint32_t)sizeof(AeronScenePointLightGPU));
			return 0;
		}
		memset(s->point_light_staging, 0, sizeof *s->point_light_staging);
	}

	const uint32_t table_bytes =
		s->mesh_table_count * (uint32_t)sizeof *s->mesh_table_staging;
	const uint32_t local_bytes =
		(s->local_light_count ? s->local_light_count : 1u) *
		(uint32_t)sizeof *s->local_light_staging;
	const uint32_t point_bytes =
		(s->point_light_count ? s->point_light_count : 1u) *
		(uint32_t)sizeof *s->point_light_staging;
	if (!scene_storage_ensure_buffer(&s->mesh_table_buffer, &s->mesh_table_buffer_cap,
									table_bytes, "scene.mesh_tables")) {
		scene_storage_report_failure(s, "scene.mesh_tables", table_bytes);
		return 0;
	}
	if (!scene_storage_ensure_buffer(&s->local_light_buffer, &s->local_light_buffer_cap,
									local_bytes, "scene.local_lights")) {
		scene_storage_report_failure(s, "scene.local_lights", local_bytes);
		return 0;
	}
	if (!scene_storage_ensure_buffer(&s->point_light_buffer, &s->point_light_buffer_cap,
									point_bytes, "scene.point_lights")) {
		scene_storage_report_failure(s, "scene.point_lights", point_bytes);
		return 0;
	}
	const AeronBufferUploadDesc uploads[3] = {
		{ s->mesh_table_buffer, 0, s->mesh_table_staging, table_bytes },
		{ s->local_light_buffer, 0, s->local_light_staging, local_bytes },
		{ s->point_light_buffer, 0, s->point_light_staging, point_bytes },
	};
	s->storage_ready = Aeron_UploadBufferBatchCmd(cmd, uploads, 3);
	if (!s->storage_ready) {
		scene_storage_report_failure(s, "scene.dynamic_storage",
									table_bytes + local_bytes + point_bytes);
	} else {
		s->storage_error_logged = 0;
	}
	return s->storage_ready;
}

uint32_t AeronSceneStorage_ShadowTableIndex(const AeronScene3D* s, uint16_t encoded_caster) {
	if (!s || !s->storage_ready) {
		return 0;
	}
	if (encoded_caster < AERON_SCENE_MAX_INSTANCES) {
		return encoded_caster < (uint16_t)s->instance_count
				   ? s->prepared_instances[encoded_caster].current_table_index
				   : 0u;
	}
	const uint16_t index = (uint16_t)(encoded_caster - AERON_SCENE_MAX_INSTANCES);
	return index < (uint16_t)s->shadow_only_count
			   ? s->prepared_shadow_only[index].current_table_index
			   : 0u;
}

void AeronSceneStorage_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	Aeron_DestroyBuffer(s->mesh_table_buffer);
	Aeron_DestroyBuffer(s->local_light_buffer);
	Aeron_DestroyBuffer(s->point_light_buffer);
	free(s->mesh_table_staging);
	free(s->local_light_staging);
	free(s->point_light_staging);
	s->mesh_table_buffer = NULL;
	s->local_light_buffer = NULL;
	s->point_light_buffer = NULL;
	s->mesh_table_staging = NULL;
	s->local_light_staging = NULL;
	s->point_light_staging = NULL;
}

static int scene_render_failure(AeronScene3D* s, AeronCommandBuffer* cmd, const char* message) {
	if (s) {
		s->scene_rt_out          = NULL;
		s->scene_rt_out_borrowed = 0;
	}
	Aeron_CommandBufferSetFailure(cmd, message);
	return 0;
}

static int scene_finalize_output(AeronScene3D* s, AeronCommandBuffer* cmd, int mb_active, int mb_regen,
								 int ssao_active) {
	int temporal_dispatch_ok = 0;
	if (s->temporal_active) {
		char label[64];
		snprintf(label, sizeof label, "FSR 3.1.4 %s", AeronTemporal_ModeName(s->temporal_active_mode));
		Aeron_GpuDebugPush(cmd, label);
		if (AeronSceneTemporal_Dispatch(s, cmd, mb_regen && s->post.mb_fsr_direct_motion)) {
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
		AeronTexture* temporal_motion =
			s->mb_temporal_motion_direct
				? AeronTemporalUpscaler_RetainedDilatedMotionVectors(s->temporal_upscaler)
				: (s->mb_temporal_velocity_rt ? Aeron_RenderTargetGetTexture(s->mb_temporal_velocity_rt)
											  : NULL);
		if (s->post.mb_velocity_viz) {
			if (s->mb_temporal_motion_valid && temporal_motion &&
				(!AeronSceneTemporal_EnsureMutableOutput(s, cmd) ||
				 !AeronScenePost_MbVisualizeTemporal(s, cmd, temporal_motion,
													  s->mb_temporal_motion_direct))) {
				Aeron_GpuDebugPop(cmd);
				return scene_render_failure(s, cmd, "Temporal motion visualization failed");
			}
		} else if (s->post.mb_shutter > 0.0f && s->mb_temporal_motion_valid && temporal_motion) {
			if (!AeronScenePost_MbResolve(s, cmd, s->scene_rt_out, temporal_motion,
										  s->mb_temporal_motion_direct)) {
				Aeron_GpuDebugPop(cmd);
				return scene_render_failure(s, cmd, "Temporal motion-blur resolve failed");
			}
			s->scene_rt_out          = s->mb_rt;
			s->scene_rt_out_borrowed = 0;
		}
		Aeron_GpuDebugPop(cmd);
	}

	if (s->bb_frame_has_lens) {
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

	if (ssao_active && s->post.ssao_debug_viz &&
		!AeronScenePost_DebugVisualizeSsao(s, cmd)) {
		return scene_render_failure(s, cmd, "SSAO diagnostic visualization failed");
	}
	if (s->directional_shadow.debug_atlas &&
		!AeronSceneDirectionalShadow_DebugVisualize(s, cmd)) {
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

	/* Batched billboards: build + upload this frame's VB (copy pass —
	 * must precede the render passes below). */
	if (!AeronSceneStorage_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene storage preparation failed");
	}
	if (s->bb_count > 0 && !AeronSceneBb3d_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene billboard preparation failed");
	}
	if (s->overlay_vertex_count > 0 && !AeronSceneMeshOverlay_Prepare(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene mesh-overlay preparation failed");
	}
	if (!AeronSceneDirectionalShadow_Prepare(s) ||
		!AeronSceneDirectionalShadow_Render(s, cmd)) {
		return scene_render_failure(s, cmd, "Scene directional-shadow preparation failed");
	}

	const int temporal_requested = s->temporal_active_mode != AERON_TEMPORAL_OFF;
	const int ssao_requested     = s->post.ssao_quality > 0 && s->post.ssao_intensity > 0.0f;
	const int mb_requested       = s->post.mb_quality > 0;
	const int pbr_required =
		s->instance_count > 0 || s->shadow_only_count > 0 || temporal_requested || ssao_requested ||
		mb_requested;
	const int pbr_ok = !pbr_required || AeronScenePbr_Ensure(s);
	if (!pbr_ok) {
		return scene_render_failure(s, cmd, "Scene PBR resource preparation failed");
	}
	s->temporal_active = temporal_requested;
	if (temporal_requested &&
		(!s->normal_rt || !AeronSceneTemporal_Ensure(s) ||
		 !AeronScenePbr_Pipeline(s, AERON_PBR_PIPE_PREPASS_TEMPORAL, AERON_CULL_NONE))) {
		return scene_render_failure(s, cmd, "Scene temporal resource preparation failed");
	}
	const int ssao_active = ssao_requested;
	if (ssao_requested &&
		(!s->normal_rt || !s->pbr_pipes[AERON_PBR_PIPE_PREPASS][AERON_CULL_NONE] ||
		 !s->pbr_pipes[AERON_PBR_PIPE_FORWARD][AERON_CULL_NONE] ||
		 !AeronScenePost_EnsureSsao(s))) {
		return scene_render_failure(s, cmd, "Scene SSAO resource preparation failed");
	}
	const int mb_active = mb_requested;
	if (mb_requested &&
		(!s->normal_rt || !s->pbr_pipes[AERON_PBR_PIPE_PREPASS_VEL][AERON_CULL_NONE] ||
		 !s->pbr_pipes[AERON_PBR_PIPE_FORWARD][AERON_CULL_NONE] ||
		 !AeronScenePost_EnsureMb(s) || !s->mb_camera_fill_pipeline)) {
		return scene_render_failure(s, cmd, "Scene motion-blur resource preparation failed");
	}
	const int msaa_active    = s->sample_count != AERON_SAMPLE_COUNT_1;
	const int prepass_active = ssao_active || mb_active || s->temporal_active || msaa_active;
	const int mb_regen       = mb_active && s->mb_velocity_regen;
	const int velocity_write = mb_regen || s->temporal_active;
	const int mb_resolve     = !s->temporal_active && mb_active && !s->post.mb_velocity_viz &&
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

		if (ssao_active) {
			Aeron_GpuDebugPush(cmd, "SSAO");
			const int ssao_ok = AeronScenePost_RunSsao(s, cmd);
			Aeron_GpuDebugPop(cmd);
			if (!ssao_ok) {
				return scene_render_failure(s, cmd, "Scene SSAO recording failed");
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
			.discard_depth     = msaa_active,
			.command_buffer    = cmd,
			.debug_label       = "Scene forward color",
		});
		if (!pass) {
			return scene_render_failure(s, cmd, "Scene forward pass creation failed");
		}
		Aeron_GpuDebugMarker(cmd, "Sky cube");
		if (!scene_draw_sky(s, pass)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene sky recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_BEFORE_OPAQUE, cmd, pass);
		scene_bind_frame_uniforms(s, pass);
		Aeron_GpuDebugMarker(cmd, "Sky billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_SKY);
		Aeron_GpuDebugMarker(cmd, "PBR forward instances");
		if (!AeronScenePbr_DrawInstances(
				s, cmd, pass, AERON_PBR_PIPE_FORWARD, /*depth_only=*/0, /*velocity=*/0,
				ssao_active ? Aeron_RenderTargetGetTexture(s->ao_rt) : NULL)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene PBR forward recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_AFTER_OPAQUE, cmd, pass);
		Aeron_GpuDebugMarker(cmd, "Mesh overlays");
		AeronSceneMeshOverlay_Draw(s, pass);
		Aeron_GpuDebugMarker(cmd, "PBR transparent instances");
		if (!AeronScenePbr_DrawTransparentInstances(
				s, pass, AERON_PBR_PIPE_FORWARD,
				ssao_active ? Aeron_RenderTargetGetTexture(s->ao_rt) : NULL)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene PBR transparent recording failed");
		}
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
					return scene_render_failure(s, cmd,
												"Scene sharp-overlay pass creation failed");
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
		Aeron_GpuDebugMarker(cmd, "Sky cube");
		if (!scene_draw_sky(s, pass)) {
			Aeron_EndRenderPass(pass);
			return scene_render_failure(s, cmd, "Scene sky recording failed");
		}
		run_hook(s, AERON_SCENE_HOOK_BEFORE_OPAQUE, cmd, pass);
		scene_bind_frame_uniforms(s, pass);
		Aeron_GpuDebugMarker(cmd, "Sky billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_SKY);
		if (pbr_ok) {
			Aeron_GpuDebugMarker(cmd, "PBR instances");
			if (!AeronScenePbr_DrawInstances(s, cmd, pass, AERON_PBR_PIPE_MESH,
											 /*depth_only=*/0, /*velocity=*/0, NULL)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene PBR mesh recording failed");
			}
		}
		run_hook(s, AERON_SCENE_HOOK_AFTER_OPAQUE, cmd, pass);
		Aeron_GpuDebugMarker(cmd, "Mesh overlays");
		AeronSceneMeshOverlay_Draw(s, pass);
		if (pbr_ok) {
			Aeron_GpuDebugMarker(cmd, "PBR transparent instances");
			if (!AeronScenePbr_DrawTransparentInstances(s, pass, AERON_PBR_PIPE_MESH, NULL)) {
				Aeron_EndRenderPass(pass);
				return scene_render_failure(s, cmd, "Scene PBR transparent recording failed");
			}
		}
		Aeron_GpuDebugMarker(cmd, "Overlay billboards");
		AeronSceneBb3d_DrawStage(s, pass, AERON_SCENE_BILLBOARD_STAGE_OVERLAY);
	}

	/* AFTER_TRANSPARENT remains a render-resolution world hook. Motion
	 * blur may already have reopened the final render-resolution pass. */
	if (pass) {
		run_hook(s, AERON_SCENE_HOOK_AFTER_TRANSPARENT, cmd, pass);
		Aeron_EndRenderPass(pass);
	}

	return scene_finalize_output(s, cmd, mb_active, mb_regen, ssao_active);
}

/* ---- accessors ---- */

AeronTexture* AeronScene_ColorTexture(AeronScene3D* s) {
	return s ? Aeron_RenderTargetGetTexture(s->color_rt) : NULL;
}
AeronRenderTarget* AeronScene_ColorRt(AeronScene3D* s) { return s ? s->color_rt : NULL; }
AeronDepthTarget*  AeronScene_DepthRt(AeronScene3D* s) { return s ? s->depth_rt : NULL; }
AeronSampleCount   AeronScene_SampleCount(const AeronScene3D* s) {
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
