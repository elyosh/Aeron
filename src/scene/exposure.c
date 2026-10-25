/*
 * The scene's exposure: the 1x1 texture that FSR and the present chain read,
 * and the image meter (AeronScene_SetAutoExposure) that writes it on the
 * GPU. The meter runs two compute passes after the scene's 3D draws: a
 * histogram of the pixels' EV100, and the exposure pass, which adapts the
 * current EV and writes the texture. The CPU gets the result a few frames
 * later through a readback.
 */

#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "aeron/time.h"

#define EXPOSURE_GROUP_SIZE 16u
/* The histogram meters the centre pixel of each 4x4 block. */
#define EXPOSURE_PIXEL_STEP 4u

typedef struct ExposureHistogramUniforms {
	uint32_t size[2];
	float    edge_weight;
	float    sky_weight;
	uint32_t channel;
	float    near_depth;
	float    near_weight;
	uint32_t pad;
} ExposureHistogramUniforms;

typedef struct ExposureAdaptUniforms {
	float    ev100;
	float    compensation;
	float    metering;
	float    min_ev100;
	float    max_ev100;
	float    brighten_speed;
	float    darken_speed;
	float    dt;
	float    low_fraction;
	float    high_fraction;
	uint32_t snap;
	uint32_t freeze;
} ExposureAdaptUniforms;

/* Matches ExposureState in scene_exposure_adapt.comp. */
typedef struct ExposureState {
	float    ev100;
	float    target_ev100;
	float    metered_ev100;
	float    exposure;
	float    low_ev100;
	float    high_ev100;
	float    geometry_share;
	uint32_t flags; /* 1 = valid, 2 = this frame was metered */
	float    histogram[AERON_SCENE_EXPOSURE_BINS];
} ExposureState;

/* The bins, then the weight of the counted geometry and sky pixels. */
#define EXPOSURE_BINS_SIZE ((AERON_SCENE_EXPOSURE_BINS + 2u) * (uint32_t)sizeof(uint32_t))

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The texture, with compute writes for the meter. Without them, the
 * exposure stays fixed. */
static int exposure_ensure_texture(AeronScene3D* s) {
	if (s->exposure_tex)
		return 1;
	AeronTextureDesc desc = {
		.width      = 1,
		.height     = 1,
		.format     = AERON_TEXTURE_FORMAT_R32G32_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST |
					  AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "Aeron exposure",
	};
	s->exposure_tex = Aeron_CreateTexture(&desc);
	if (!s->exposure_tex) {
		Aeron_LogWarn("aeron.scene",
					  "exposure: no compute writes to R32G32_FLOAT, so the exposure stays fixed");
		s->exposure_meter_failed = 1;
		desc.usage               = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST;
		s->exposure_tex          = Aeron_CreateTexture(&desc);
	}
	s->exposure_uploaded = 0.0f;
	return s->exposure_tex != NULL;
}

static int exposure_upload_fixed(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (s->exposure_uploaded == s->exposure)
		return 1;
	const float texel[2] = { s->exposure, log2f(AERON_SCENE_LUMENS_PER_WATT / (1.2f * s->exposure)) };
	const AeronTextureUploadDesc upload = {
		.texture = s->exposure_tex, .width = 1, .height = 1, .raw_data = texel, .raw_size = sizeof texel
	};
	if (!Aeron_UploadTextureDataCmd(cmd, &upload))
		return 0;
	s->exposure_uploaded = s->exposure;
	return 1;
}

static void exposure_release_meter(AeronScene3D* s) {
	Aeron_DestroyComputePipeline(s->exposure_histogram_pipeline);
	Aeron_DestroyComputePipeline(s->exposure_adapt_pipeline);
	Aeron_DestroySampler(s->exposure_sampler);
	Aeron_DestroyBuffer(s->exposure_bins);
	Aeron_DestroyBuffer(s->exposure_state);
	Aeron_DestroyBufferReadback(s->exposure_readback);
	s->exposure_histogram_pipeline = NULL;
	s->exposure_adapt_pipeline     = NULL;
	s->exposure_sampler            = NULL;
	s->exposure_bins               = NULL;
	s->exposure_state              = NULL;
	s->exposure_readback           = NULL;
	s->exposure_readout_valid      = 0;
}

static int exposure_ensure_meter(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (s->exposure_readback)
		return 1;
	s->exposure_sampler            = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_NEAREST,
		.mag_filter = AERON_FILTER_NEAREST,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	s->exposure_histogram_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name                           = "scene_exposure_histogram.comp",
		.sampler_count                  = 2,
		.readwrite_storage_buffer_count = 1,
		.uniform_buffer_count           = 1,
		.thread_count_x                 = EXPOSURE_GROUP_SIZE,
		.thread_count_y                 = EXPOSURE_GROUP_SIZE,
		.thread_count_z                 = 1,
	});
	s->exposure_adapt_pipeline     = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name                            = "scene_exposure_adapt.comp",
		.readwrite_storage_texture_count = 1,
		.readwrite_storage_buffer_count  = 2,
		.uniform_buffer_count            = 1,
		.thread_count_x                  = AERON_SCENE_EXPOSURE_BINS,
		.thread_count_y                  = 1,
		.thread_count_z                  = 1,
	});
	const uint32_t usage           = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ |
									 AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	s->exposure_bins               = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size = EXPOSURE_BINS_SIZE, .usage = usage, .debug_name = "Aeron exposure histogram" });
	s->exposure_state              = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size = (uint32_t)sizeof(ExposureState), .usage = usage, .debug_name = "Aeron exposure state" });
	s->exposure_readback           = Aeron_CreateBufferReadback((uint32_t)sizeof(ExposureState));
	static const uint8_t zeros[sizeof(ExposureState)];
	if (!s->exposure_sampler || !s->exposure_histogram_pipeline || !s->exposure_adapt_pipeline ||
		!s->exposure_bins || !s->exposure_state || !s->exposure_readback ||
		!Aeron_UploadBufferDataCmd(cmd, s->exposure_bins, 0, zeros, EXPOSURE_BINS_SIZE) ||
		!Aeron_UploadBufferDataCmd(cmd, s->exposure_state, 0, zeros, (uint32_t)sizeof zeros)) {
		Aeron_LogError("aeron.scene",
					   "exposure: could not create the image meter, so the exposure stays fixed");
		exposure_release_meter(s);
		s->exposure_meter_failed = 1;
		return 0;
	}
	s->exposure_snap = 1;
	return 1;
}

/* Takes the newest readback, whose exposure becomes the scene's. */
static void exposure_poll(AeronScene3D* s) {
	ExposureState state;
	if (!Aeron_PollBufferReadback(s->exposure_readback, &state) || !(state.flags & 1u))
		return;
	AeronSceneExposureReadout* r = &s->exposure_readout;
	r->exposure                  = state.exposure;
	r->ev100                     = state.ev100;
	r->target_ev100              = state.target_ev100;
	r->metered_ev100             = state.metered_ev100;
	r->low_ev100                 = state.low_ev100;
	r->high_ev100                = state.high_ev100;
	r->geometry_share            = state.geometry_share;
	r->metered                   = (state.flags & 2u) != 0;
	memcpy(r->histogram, state.histogram, sizeof r->histogram);
	s->exposure_readout_valid = 1;
	if (isfinite(state.exposure) && state.exposure > 0.0f)
		s->exposure = state.exposure;
}

static int exposure_meter(AeronScene3D* s, AeronCommandBuffer* cmd) {
	const AeronSceneAutoExposure* e = &s->exposure_settings;
	exposure_poll(s);

	const uint64_t now  = Aeron_NowUs();
	float          dt   = s->exposure_last_us ? (float)(now - s->exposure_last_us) * 1e-6f : 0.0f;
	s->exposure_last_us = now;
	/* A long stall, such as a load, doesn't jump the exposure. */
	dt = clampf(dt, 0.0f, 0.25f);

	const AeronComputeBufferBinding bins = { .buffer = s->exposure_bins };
	AeronComputePass*               pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = &bins,
		.write_buffer_count = 1,
		.debug_label        = "Exposure histogram",
	});
	if (!pass)
		return 0;
	/* Reversed-Z with an infinite far plane: depth = near / view distance. */
	const float                     near_view = e->near_distance / s->view_space_to_meters;
	const ExposureHistogramUniforms hu        = {
		.size        = { (uint32_t)s->render_w, (uint32_t)s->render_h },
		.edge_weight = clampf(e->edge_weight, 0.0f, 1.0f),
		.sky_weight  = clampf(e->sky_weight, 0.0f, 1.0f),
		.channel     = e->channel == AERON_SCENE_EXPOSURE_MAX ? 1u : 0u,
		.near_depth  = near_view > 0.0f ? s->camera.near_z / near_view : INFINITY,
		.near_weight = clampf(e->near_weight, 0.0f, 1.0f),
	};
	Aeron_BindComputePipeline(pass, s->exposure_histogram_pipeline);
	Aeron_BindComputeTextureSampler(pass, 0, Aeron_RenderTargetGetTexture(s->color_rt), s->exposure_sampler);
	Aeron_BindComputeTextureSampler(pass, 1, Aeron_DepthTargetGetTexture(s->depth_rt), s->exposure_sampler);
	Aeron_BindComputeUniformData(pass, 0, &hu, sizeof hu);
	const uint32_t block = EXPOSURE_GROUP_SIZE * EXPOSURE_PIXEL_STEP;
	Aeron_DispatchCompute(pass, ((uint32_t)s->render_w + block - 1u) / block,
						  ((uint32_t)s->render_h + block - 1u) / block, 1);
	Aeron_EndComputePass(pass);

	const AeronComputeTextureBinding texture    = { .texture = s->exposure_tex };
	const AeronComputeBufferBinding  buffers[2] = { { .buffer = s->exposure_bins },
													{ .buffer = s->exposure_state } };
	pass                                        = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &texture,
		.write_texture_count = 1,
		.write_buffers       = buffers,
		.write_buffer_count  = 2,
		.debug_label         = "Exposure adaptation",
	});
	if (!pass)
		return 0;
	const float                 low  = clampf(e->low_percent, 0.0f, 100.0f) * 0.01f;
	const float                 high = clampf(e->high_percent, 0.0f, 100.0f) * 0.01f;
	const ExposureAdaptUniforms au   = {
		.ev100          = e->ev100,
		.compensation   = e->compensation,
		.metering       = clampf(e->metering, 0.0f, 1.0f),
		.min_ev100      = e->min_ev100,
		.max_ev100      = e->max_ev100 > e->min_ev100 ? e->max_ev100 : e->min_ev100,
		.brighten_speed = e->brighten_speed > 0.0f ? e->brighten_speed : 0.0f,
		.darken_speed   = e->darken_speed > 0.0f ? e->darken_speed : 0.0f,
		.dt             = dt,
		.low_fraction   = low,
		.high_fraction  = high > low ? high : low,
		.snap           = s->exposure_snap ? 1u : 0u,
		.freeze         = e->freeze ? 1u : 0u,
	};
	Aeron_BindComputePipeline(pass, s->exposure_adapt_pipeline);
	Aeron_BindComputeUniformData(pass, 0, &au, sizeof au);
	Aeron_DispatchCompute(pass, 1, 1, 1);
	Aeron_EndComputePass(pass);
	s->exposure_snap = 0;
	/* Returning to a fixed exposure uploads it again. */
	s->exposure_uploaded = 0.0f;
	return Aeron_DownloadBufferCmd(cmd, s->exposure_state, s->exposure_readback);
}

int AeronSceneExposure_Update(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!exposure_ensure_texture(s))
		return 0;
	if (s->exposure_metered && !s->exposure_meter_failed && exposure_ensure_meter(s, cmd))
		return exposure_meter(s, cmd);
	return exposure_upload_fixed(s, cmd);
}

void AeronSceneExposure_Release(AeronScene3D* s) {
	if (!s)
		return;
	exposure_release_meter(s);
	Aeron_DestroyTexture(s->exposure_tex);
	s->exposure_tex = NULL;
}

void AeronScene_SetAutoExposure(AeronScene3D* s, const AeronSceneAutoExposure* settings) {
	if (!s)
		return;
	if (!settings) {
		s->exposure_metered       = 0;
		s->exposure_readout_valid = 0;
		return;
	}
	/* New settings take their target at once; freezing doesn't. */
	AeronSceneAutoExposure compared = *settings;
	compared.freeze                 = s->exposure_settings.freeze;
	if (!s->exposure_metered || memcmp(&compared, &s->exposure_settings, sizeof compared) != 0)
		s->exposure_snap = 1;
	s->exposure_settings = *settings;
	s->exposure_metered  = 1;
}

void AeronScene_SnapExposure(AeronScene3D* s) {
	if (s)
		s->exposure_snap = 1;
}

AeronTexture* AeronScene_ExposureTexture(AeronScene3D* s) { return s ? s->exposure_tex : NULL; }

int AeronScene_GetExposureReadout(const AeronScene3D* s, AeronSceneExposureReadout* out) {
	if (!s || !out || !s->exposure_metered || !s->exposure_readout_valid)
		return 0;
	*out = s->exposure_readout;
	return 1;
}
