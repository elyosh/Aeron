#include "aeron/temporal.h"

#include "aeron/log.h"
#include "backend.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

struct AeronTemporalUpscaler {
	AeronFsr3Backend*         backend;
	FfxFsr3UpscalerContext    context;
	AeronTexture*             dilatedDepth;
	AeronTexture*             scratchDilatedMotionVectors;
	AeronTexture*             retainedDilatedMotionVectors;
	AeronTexture*             currentDilatedMotionVectors;
	AeronBuffer*              reconstructedDepth;
	AeronRenderTarget*        outputTarget;
	AeronTemporalUpscalerDesc description;
	char                      lastError[256];
	bool                      contextCreated;
	bool                      dispatchSucceeded;
	bool                      retainedMotionValid;
	uint32_t                  pipelineRecreationCount;
};

namespace {

static void SetError(AeronTemporalUpscaler* upscaler, const char* format, ...) {
	if (!upscaler) {
		return;
	}
	va_list args;
	va_start(args, format);
	std::vsnprintf(upscaler->lastError, sizeof(upscaler->lastError), format, args);
	va_end(args);
	Aeron_Log("fsr3", "%s", upscaler->lastError);
}

static void FsrMessage(FfxMsgType type, const wchar_t* message) {
	char text[512] {};
	if (message) {
		std::wcstombs(text, message, sizeof(text) - 1);
	}
	Aeron_Log("fsr3", "%s: %s", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

static FfxFsr3UpscalerQualityMode ToFfxQuality(AeronTemporalMode mode) {
	switch (mode) {
		case AERON_TEMPORAL_NATIVE_AA:
			return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
		case AERON_TEMPORAL_QUALITY:
			return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
		case AERON_TEMPORAL_BALANCED:
			return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
		case AERON_TEMPORAL_PERFORMANCE:
			return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
		default:
			return static_cast<FfxFsr3UpscalerQualityMode>(-1);
	}
}

static bool TextureHasUsage(const AeronTexture* texture, uint32_t usage) {
	return texture && (Aeron_TextureGetUsage(texture) & usage) == usage;
}

static bool TextureMatches(const AeronTexture* texture, uint32_t width, uint32_t height) {
	return texture && Aeron_TextureGetWidth(texture) >= static_cast<int>(width) &&
		   Aeron_TextureGetHeight(texture) >= static_cast<int>(height);
}

static bool CreateSharedResources(AeronTemporalUpscaler* upscaler) {
	AeronTextureDesc texture {};
	texture.width          = static_cast<int>(upscaler->description.max_render_width);
	texture.height         = static_cast<int>(upscaler->description.max_render_height);
	texture.mip_count      = 1;
	texture.usage          = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ |
							 AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE;
	texture.format         = AERON_TEXTURE_FORMAT_R32_FLOAT;
	texture.debug_name     = "FSR3 shared dilated depth";
	upscaler->dilatedDepth = Aeron_CreateTexture(&texture);
	texture.format         = AERON_TEXTURE_FORMAT_R16G16_FLOAT;
	texture.debug_name     = "FSR3 scratch dilated motion vectors";
	upscaler->scratchDilatedMotionVectors = Aeron_CreateTexture(&texture);
	if (upscaler->description.retain_motion_vectors) {
		texture.debug_name                     = "FSR3 retained dilated motion vectors";
		upscaler->retainedDilatedMotionVectors = Aeron_CreateTexture(&texture);
	}
	upscaler->currentDilatedMotionVectors = upscaler->scratchDilatedMotionVectors;

	const uint64_t reconstructedSize = static_cast<uint64_t>(upscaler->description.max_render_width) *
									   upscaler->description.max_render_height * sizeof(uint32_t);
	if (reconstructedSize > UINT32_MAX) {
		return false;
	}
	AeronBufferDesc buffer {};
	buffer.size         = static_cast<uint32_t>(reconstructedSize);
	buffer.usage        = AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
	buffer.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY;
	buffer.debug_name   = "FSR3 shared reconstructed depth";
	upscaler->reconstructedDepth = Aeron_CreateBuffer(&buffer);
	if (!upscaler->dilatedDepth || !upscaler->scratchDilatedMotionVectors ||
		(upscaler->description.retain_motion_vectors && !upscaler->retainedDilatedMotionVectors) ||
		!upscaler->reconstructedDepth) {
		return false;
	}

	/* The fused end-of-frame clear maintains this invariant after the first
	 * dispatch; initialize the allocation once so the first clear is redundant. */
	uint32_t* initialDepth = new (std::nothrow) uint32_t[buffer.size / sizeof(uint32_t)] {};
	if (!initialDepth) {
		return false;
	}
	const bool initialized =
		Aeron_UploadBufferData(upscaler->reconstructedDepth, 0, initialDepth, buffer.size) != 0;
	delete[] initialDepth;
	if (!initialized) {
		return false;
	}
	AeronFsr3Backend_SetReconstructedDepthInitialized(upscaler->backend, upscaler->reconstructedDepth);
	return true;
}

static bool ValidateDispatch(AeronTemporalUpscaler* upscaler, const AeronTemporalDispatchDesc* desc,
							 AeronTexture* output) {
	if (!desc || !desc->command_buffer || !desc->color || !desc->depth || !desc->motion_vectors || !output) {
		SetError(upscaler, "missing command buffer or required FSR input");
		return false;
	}
	if (desc->render_width == 0 || desc->render_height == 0 || desc->output_width == 0 ||
		desc->output_height == 0 || desc->render_width > upscaler->description.max_render_width ||
		desc->render_height > upscaler->description.max_render_height ||
		desc->output_width > upscaler->description.max_output_width ||
		desc->output_height > upscaler->description.max_output_height) {
		SetError(upscaler, "FSR dispatch dimensions exceed the configured context");
		return false;
	}
	if (!TextureMatches(desc->color, desc->render_width, desc->render_height) ||
		!TextureMatches(desc->depth, desc->render_width, desc->render_height) ||
		!TextureMatches(desc->motion_vectors, desc->render_width, desc->render_height) ||
		!TextureMatches(output, desc->output_width, desc->output_height)) {
		SetError(upscaler, "FSR resource dimensions do not cover the dispatch dimensions");
		return false;
	}
	if (!TextureHasUsage(desc->color,
						 AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) ||
		!TextureHasUsage(desc->depth, AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) ||
		!TextureHasUsage(desc->motion_vectors, AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) ||
		!TextureHasUsage(output, AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE)) {
		SetError(upscaler, "FSR resources were not created with the required compute usages");
		return false;
	}
	if (Aeron_TextureGetFormat(desc->depth) != AERON_TEXTURE_FORMAT_R32_FLOAT) {
		SetError(upscaler, "FSR depth input must be an R32_FLOAT color texture");
		return false;
	}
	if (desc->reactive && (!TextureMatches(desc->reactive, desc->render_width, desc->render_height) ||
						   !TextureHasUsage(desc->reactive, AERON_TEXTURE_USAGE_SAMPLED))) {
		SetError(upscaler, "invalid FSR reactive mask");
		return false;
	}
	if (desc->transparency_and_composition &&
		(!TextureMatches(desc->transparency_and_composition, desc->render_width, desc->render_height) ||
		 !TextureHasUsage(desc->transparency_and_composition, AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ))) {
		SetError(upscaler, "invalid FSR transparency/composition mask");
		return false;
	}
	if (desc->update_retained_motion_vectors && !upscaler->retainedDilatedMotionVectors) {
		SetError(upscaler, "retained FSR motion was not enabled for this context");
		return false;
	}
	if (desc->frame_time_delta_ms <= 0.0f || desc->pre_exposure <= 0.0f || desc->camera_near <= 0.0f ||
		desc->camera_vertical_fov_radians <= 0.0f) {
		SetError(upscaler, "invalid FSR timing, exposure, or camera values");
		return false;
	}
	return true;
}

static bool TryCreateContext(AeronTemporalUpscaler* upscaler, const AeronTemporalUpscalerDesc* desc,
							 bool directHistory, char* failedProfiles, size_t failedProfilesSize,
							 FfxErrorCode* lastResult) {
	for (uint32_t profile = 0; profile < AeronFsr3Backend_ProfileCount(); ++profile) {
		upscaler->backend = AeronFsr3Backend_Create(profile, directHistory);
		if (!upscaler->backend) {
			continue;
		}

		FfxFsr3UpscalerContextDescription contextDesc {};
		contextDesc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
							FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
		if (desc->debug_checking) {
			contextDesc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
			contextDesc.fpMessage = FsrMessage;
		}
		contextDesc.maxRenderSize    = { desc->max_render_width, desc->max_render_height };
		contextDesc.maxUpscaleSize   = { desc->max_output_width, desc->max_output_height };
		contextDesc.backendInterface = AeronFsr3Backend_GetInterface(upscaler->backend);
		++upscaler->pipelineRecreationCount;
		*lastResult = ffxFsr3UpscalerContextCreate(&upscaler->context, &contextDesc);
		if (*lastResult == FFX_OK) {
			if (!AeronFsr3Backend_FinishInitialization(upscaler->backend)) {
				ffxFsr3UpscalerContextDestroy(&upscaler->context);
				std::memset(&upscaler->context, 0, sizeof(upscaler->context));
				*lastResult = FFX_ERROR_BACKEND_API_ERROR;
			} else {
				upscaler->contextCreated = true;
				AeronFsr3Backend_AppendFallbackReason(upscaler->backend, failedProfiles);
				return true;
			}
		}

		char failedAttempt[256];
		std::snprintf(failedAttempt, sizeof(failedAttempt), "%s%s failed at %s (0x%08x)",
					  AeronFsr3Backend_ProfileName(upscaler->backend), directHistory ? " direct-history" : "",
					  AeronFsr3Backend_LastFailedPipeline(upscaler->backend),
					  static_cast<unsigned>(*lastResult));
		const size_t used = std::strlen(failedProfiles);
		if (used < failedProfilesSize - 1) {
			std::snprintf(failedProfiles + used, failedProfilesSize - used, "%s%s", used ? "; " : "",
						  failedAttempt);
		}
		Aeron_Log("fsr3", "%s; trying the next complete profile", failedAttempt);
		AeronFsr3Backend_Destroy(upscaler->backend);
		upscaler->backend = nullptr;
		std::memset(&upscaler->context, 0, sizeof(upscaler->context));
	}
	return false;
}

static FfxResource OptionalTexture(AeronTexture* texture, const wchar_t* name) {
	return texture ? AeronFsr3Backend_TextureResource(texture, name, FFX_RESOURCE_STATE_COMPUTE_READ)
				   : FfxResource {};
}

} // namespace

extern "C" AeronTemporalUpscaler* AeronTemporalUpscaler_Create(const AeronTemporalUpscalerDesc* desc) {
	if (!desc || desc->max_render_width == 0 || desc->max_render_height == 0 || desc->max_output_width == 0 ||
		desc->max_output_height == 0) {
		return nullptr;
	}
	AeronTemporalUpscaler* upscaler = new (std::nothrow) AeronTemporalUpscaler {};
	if (!upscaler) {
		return nullptr;
	}
	upscaler->description = *desc;
	char         failedProfiles[512] {};
	FfxErrorCode lastResult = FFX_ERROR_BACKEND_API_ERROR;
	if (desc->output_format == AERON_TEXTURE_FORMAT_RGBA16_FLOAT) {
		TryCreateContext(upscaler, desc, true, failedProfiles, sizeof(failedProfiles), &lastResult);
	}
	if (!upscaler->contextCreated) {
		TryCreateContext(upscaler, desc, false, failedProfiles, sizeof(failedProfiles), &lastResult);
	}
	if (!upscaler->contextCreated) {
		SetError(upscaler, "ffxFsr3UpscalerContextCreate failed (0x%08x): %s",
				 static_cast<unsigned>(lastResult), failedProfiles[0] ? failedProfiles : "no shader profile");
		AeronTemporalUpscaler_Destroy(upscaler);
		return nullptr;
	}
	if (!CreateSharedResources(upscaler)) {
		SetError(upscaler, "could not allocate FSR shared resources");
		AeronTemporalUpscaler_Destroy(upscaler);
		return nullptr;
	}
	return upscaler;
}

extern "C" void AeronTemporalUpscaler_Destroy(AeronTemporalUpscaler* upscaler) {
	if (!upscaler) {
		return;
	}
	if (upscaler->contextCreated) {
		ffxFsr3UpscalerContextDestroy(&upscaler->context);
	}
	Aeron_DestroyTexture(upscaler->dilatedDepth);
	Aeron_DestroyTexture(upscaler->scratchDilatedMotionVectors);
	Aeron_DestroyTexture(upscaler->retainedDilatedMotionVectors);
	Aeron_DestroyBuffer(upscaler->reconstructedDepth);
	AeronFsr3Backend_Destroy(upscaler->backend);
	delete upscaler;
}

extern "C" int AeronTemporalUpscaler_Dispatch(AeronTemporalUpscaler*           upscaler,
											  const AeronTemporalDispatchDesc* desc) {
	if (upscaler) {
		upscaler->dispatchSucceeded = false;
		upscaler->outputTarget      = nullptr;
	}
	const bool    directOutput = upscaler && desc && AeronFsr3Backend_UsesDirectHistory(upscaler->backend) &&
								 !desc->enable_sharpening && !desc->debug_view;
	AeronTexture* output =
		directOutput ? AeronFsr3Backend_OutputAlias(upscaler->backend) : (desc ? desc->output : nullptr);
	if (!upscaler || !ValidateDispatch(upscaler, desc, output)) {
		return 0;
	}
	if (desc->reset_history) {
		upscaler->retainedMotionValid = false;
	}
	const bool    updateRetained = desc->update_retained_motion_vectors != 0;
	AeronTexture* motionTarget =
		updateRetained ? upscaler->retainedDilatedMotionVectors : upscaler->scratchDilatedMotionVectors;
	AeronFsr3Backend_BeginDispatch(upscaler->backend);
	FfxFsr3UpscalerDispatchDescription dispatch {};
	dispatch.commandList   = desc->command_buffer;
	dispatch.color         = AeronFsr3Backend_TextureResource(desc->color, L"Aeron FSR input color",
															  FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.depth         = AeronFsr3Backend_TextureResource(desc->depth, L"Aeron FSR input depth",
															  FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors = AeronFsr3Backend_TextureResource(
		desc->motion_vectors, L"Aeron FSR motion vectors", FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.exposure = OptionalTexture(desc->exposure, L"Aeron FSR exposure");
	dispatch.reactive = OptionalTexture(desc->reactive, L"Aeron FSR reactive mask");
	dispatch.transparencyAndComposition =
		OptionalTexture(desc->transparency_and_composition, L"Aeron FSR transparency mask");
	dispatch.dilatedDepth = AeronFsr3Backend_TextureResource(
		upscaler->dilatedDepth, L"Aeron FSR dilated depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.dilatedMotionVectors =
		AeronFsr3Backend_TextureResource(motionTarget,
										 updateRetained ? L"Aeron FSR retained dilated motion vectors"
														: L"Aeron FSR scratch dilated motion vectors",
										 FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.reconstructedPrevNearestDepth = AeronFsr3Backend_BufferResource(
		upscaler->reconstructedDepth, sizeof(uint32_t), L"Aeron FSR reconstructed depth",
		FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.output =
		AeronFsr3Backend_TextureResource(output, L"Aeron FSR output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.jitterOffset      = { desc->jitter_x, desc->jitter_y };
	dispatch.motionVectorScale = { desc->motion_vector_scale_x, desc->motion_vector_scale_y };
	dispatch.renderSize        = { desc->render_width, desc->render_height };
	dispatch.upscaleSize       = { desc->output_width, desc->output_height };
	dispatch.enableSharpening  = desc->enable_sharpening != 0;
	dispatch.sharpness         = std::clamp(desc->sharpness, 0.0f, 1.0f);
	dispatch.frameTimeDelta    = desc->frame_time_delta_ms;
	dispatch.preExposure       = desc->pre_exposure;
	dispatch.reset             = desc->reset_history != 0;
	/* AMD names these by depth-buffer orientation. Reversed infinite depth
	 * uses FLT_MAX in cameraNear and the physical near plane in cameraFar. */
	dispatch.cameraNear              = FLT_MAX;
	dispatch.cameraFar               = desc->camera_near;
	dispatch.cameraFovAngleVertical  = desc->camera_vertical_fov_radians;
	dispatch.viewSpaceToMetersFactor = desc->view_space_to_meters;
	dispatch.flags                   = desc->debug_view ? FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW : 0;

	const FfxErrorCode result = ffxFsr3UpscalerContextDispatch(&upscaler->context, &dispatch);
	if (result != FFX_OK) {
		if (updateRetained) {
			upscaler->retainedMotionValid = false;
		}
		SetError(upscaler, "ffxFsr3UpscalerContextDispatch failed (0x%08x)", static_cast<unsigned>(result));
		return 0;
	}
	if (!AeronFsr3Backend_LastExecutionSucceeded(upscaler->backend)) {
		if (updateRetained) {
			upscaler->retainedMotionValid = false;
		}
		SetError(upscaler, "FSR backend did not execute the queued GPU jobs");
		return 0;
	}
	upscaler->lastError[0] = '\0';
	if (directOutput) {
		upscaler->outputTarget = AeronFsr3Backend_BorrowedOutputTarget(upscaler->backend);
		if (!upscaler->outputTarget) {
			SetError(upscaler, "FSR direct-history accumulate did not expose its output target");
			return 0;
		}
	}
	upscaler->dispatchSucceeded           = true;
	upscaler->currentDilatedMotionVectors = motionTarget;
	if (updateRetained) {
		upscaler->retainedMotionValid = true;
	}
	return 1;
}

extern "C" AeronTexture* AeronTemporalUpscaler_DilatedDepth(const AeronTemporalUpscaler* upscaler) {
	return upscaler && upscaler->dispatchSucceeded ? upscaler->dilatedDepth : nullptr;
}

extern "C" AeronTexture* AeronTemporalUpscaler_DilatedMotionVectors(const AeronTemporalUpscaler* upscaler) {
	return upscaler && upscaler->dispatchSucceeded ? upscaler->currentDilatedMotionVectors : nullptr;
}

extern "C" AeronRenderTarget* AeronTemporalUpscaler_OutputTarget(const AeronTemporalUpscaler* upscaler) {
	return upscaler && upscaler->dispatchSucceeded ? upscaler->outputTarget : nullptr;
}

extern "C" int AeronTemporalUpscaler_UsesDirectHistory(const AeronTemporalUpscaler* upscaler) {
	return upscaler && AeronFsr3Backend_UsesDirectHistory(upscaler->backend);
}

extern "C" AeronTexture*
AeronTemporalUpscaler_RetainedDilatedMotionVectors(const AeronTemporalUpscaler* upscaler) {
	return upscaler && upscaler->retainedMotionValid ? upscaler->retainedDilatedMotionVectors : nullptr;
}

extern "C" void AeronTemporalUpscaler_InvalidateRetainedMotionVectors(AeronTemporalUpscaler* upscaler) {
	if (upscaler) {
		upscaler->retainedMotionValid = false;
	}
}

extern "C" const char* AeronTemporalUpscaler_LastError(const AeronTemporalUpscaler* upscaler) {
	return upscaler ? upscaler->lastError : "invalid temporal upscaler";
}

extern "C" int AeronTemporalUpscaler_GetProfileInfo(const AeronTemporalUpscaler* upscaler,
													AeronTemporalProfileInfo*    info) {
	if (!upscaler || !upscaler->backend || !info) {
		return 0;
	}
	*info                           = {};
	info->manifest_schema           = AeronFsr3Backend_ManifestSchema();
	info->manifest_hash             = AeronFsr3Backend_ManifestHash();
	info->profile_name              = AeronFsr3Backend_ProfileName(upscaler->backend);
	info->backend_driver            = Aeron_RenderDriverName();
	info->fallback_reason           = AeronFsr3Backend_FallbackReason(upscaler->backend);
	info->atomic_layout             = AeronFsr3Backend_AtomicLayout(upscaler->backend);
	info->pipeline_recreation_count = upscaler->pipelineRecreationCount;
	info->fp16                      = AeronFsr3Backend_UsesFp16(upscaler->backend);
	info->wave_spd                  = AeronFsr3Backend_UsesWaveSpd(upscaler->backend);
	info->lanczos_lut               = AeronFsr3Backend_UsesLanczosLut(upscaler->backend);
	info->direct_history_output     = AeronFsr3Backend_UsesDirectHistory(upscaler->backend);
	return 1;
}

extern "C" const char* AeronTemporal_ModeName(AeronTemporalMode mode) {
	switch (mode) {
		case AERON_TEMPORAL_OFF:
			return "off";
		case AERON_TEMPORAL_NATIVE_AA:
			return "native_aa";
		case AERON_TEMPORAL_QUALITY:
			return "quality";
		case AERON_TEMPORAL_BALANCED:
			return "balanced";
		case AERON_TEMPORAL_PERFORMANCE:
			return "performance";
		default:
			return "invalid";
	}
}

extern "C" int AeronTemporal_ParseMode(const char* text, AeronTemporalMode* mode) {
	if (!text || !mode) {
		return 0;
	}
	for (int value = AERON_TEMPORAL_OFF; value <= AERON_TEMPORAL_PERFORMANCE; ++value) {
		const AeronTemporalMode candidate = static_cast<AeronTemporalMode>(value);
		if (std::strcmp(text, AeronTemporal_ModeName(candidate)) == 0) {
			*mode = candidate;
			return 1;
		}
	}
	return 0;
}

extern "C" int AeronTemporal_GetRenderResolution(AeronTemporalMode mode, uint32_t output_width,
												 uint32_t output_height, uint32_t* render_width,
												 uint32_t* render_height) {
	if (!render_width || !render_height || output_width == 0 || output_height == 0) {
		return 0;
	}
	if (mode == AERON_TEMPORAL_OFF) {
		*render_width  = output_width;
		*render_height = output_height;
		return 1;
	}
	return ffxFsr3UpscalerGetRenderResolutionFromQualityMode(render_width, render_height, output_width,
															 output_height, ToFfxQuality(mode)) == FFX_OK;
}

extern "C" float AeronTemporal_GetMipLodBias(AeronTemporalMode mode, uint32_t render_width,
											 uint32_t output_width) {
	if (mode == AERON_TEMPORAL_OFF || ToFfxQuality(mode) == static_cast<FfxFsr3UpscalerQualityMode>(-1) ||
		render_width == 0 || output_width == 0 || render_width > output_width) {
		return 0.0f;
	}
	return std::log2(static_cast<float>(render_width) / static_cast<float>(output_width)) - 1.0f;
}

extern "C" int AeronTemporal_GetJitter(uint64_t frame_index, uint32_t render_width, uint32_t output_width,
									   float* jitter_x, float* jitter_y) {
	if (!jitter_x || !jitter_y || render_width == 0 || output_width == 0) {
		return 0;
	}
	const int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(static_cast<int32_t>(render_width),
																  static_cast<int32_t>(output_width));
	if (phaseCount <= 0) {
		return 0;
	}
	return ffxFsr3UpscalerGetJitterOffset(jitter_x, jitter_y, static_cast<int32_t>(frame_index % phaseCount),
										  phaseCount) == FFX_OK;
}
