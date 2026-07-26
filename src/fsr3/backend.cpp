#include "backend.h"

#include "aeron/log.h"

#include <FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

constexpr uint32_t kResourceCount    = 128;
constexpr uint32_t kJobCount         = 64;
constexpr uint32_t kMaxConstantBytes = 4096;
/* FSR 3.1.4's private permutation bits passed through FfxInterface. */
constexpr uint32_t kLanczosLutPermutation    = 1u << 0;
constexpr uint32_t kHdrPermutation           = 1u << 1;
constexpr uint32_t kLowResMvPermutation      = 1u << 2;
constexpr uint32_t kJitteredMvPermutation    = 1u << 3;
constexpr uint32_t kInvertedDepthPermutation = 1u << 4;
constexpr uint32_t kSharpenPermutation       = 1u << 5;
constexpr uint32_t kForceWave64Permutation   = 1u << 6;
constexpr uint32_t kAllowFp16Permutation     = 1u << 7;
constexpr uint32_t kKnownPermutations        = (1u << 8) - 1;

struct ResourceEntry {
	bool                   occupied;
	bool                   owned;
	bool                   dynamic;
	bool                   clearPlanLogged;
	uint32_t               resourceId;
	FfxResourceDescription description;
	AeronRenderTarget*     renderTarget;
	AeronTexture*          texture;
	AeronBuffer*           buffer;
	uint64_t               bytes;
	char                   name[96];
};

enum class SamplerKind {
	LinearClamp,
	PointClamp,
};

struct PipelineManifest {
	FfxPass               pass;
	bool                  directHistory;
	const char*           profile;
	bool                  fp16;
	bool                  waveSpd;
	bool                  lanczosLut;
	const char*           atomicLayout;
	const char*           shader;
	uint32_t              threadX;
	uint32_t              threadY;
	uint32_t              sampledTextureCount;
	const SamplerKind*    samplers;
	const wchar_t* const* srvTextures;
	uint32_t              srvTextureCount;
	const wchar_t* const* srvBuffers;
	uint32_t              srvBufferCount;
	const wchar_t* const* uavTextures;
	uint32_t              uavTextureCount;
	const wchar_t* const* uavBuffers;
	uint32_t              uavBufferCount;
	const wchar_t* const* constants;
	uint32_t              constantCount;
};

struct UnavailableProfile {
	const char* profile;
	const char* reason;
};

struct AvailableProfile {
	const char* profile;
	bool        fp16;
	bool        waveSpd;
	bool        lanczosLut;
	const char* atomicLayout;
};

struct PipelineEntry {
	AeronComputePipeline*   pipeline;
	const PipelineManifest* manifest;
};

struct QueuedJob {
	FfxGpuJobDescription                                                          job;
	std::array<std::array<uint8_t, kMaxConstantBytes>, FFX_MAX_NUM_CONST_BUFFERS> constants;
};

/* Luma instability restores the zeroed input required by the next prepare-inputs pass. */
enum class ReconstructedDepthState {
	NeedsClear,
	Ready,
	Dirty,
};

/* Aeron gives FFX internal resources dedicated persistent allocations. SPD's
 * counter self-resets, and unwritten mip texels stay zero after one clear. */
enum class PersistentClearState {
	NeedsClear,
	Ready,
};

} // namespace

struct AeronFsr3Backend {
	std::array<ResourceEntry, kResourceCount>               resources {};
	std::array<QueuedJob, kJobCount>                        jobs {};
	std::array<PipelineEntry*, FFX_FSR3UPSCALER_PASS_COUNT> pipelines {};
	uint32_t                                                jobCount                  = 0;
	uint64_t                                                ownedBytes                = 0;
	AeronSampler*                                           linearSampler             = nullptr;
	AeronSampler*                                           pointSampler              = nullptr;
	AeronComputePipeline*                                   clearTexturePipeline      = nullptr;
	AeronComputePipeline*                                   clearBufferPipeline       = nullptr;
	AeronCommandBuffer*                                     initializationCommandBuffer = nullptr;
	const char*                                             profileName               = "unavailable";
	bool                                                    fp16                      = false;
	bool                                                    waveSpd                   = false;
	bool                                                    lanczosLut                = false;
	const char*                                             atomicLayout              = "unavailable";
	bool                                                    externalClearPlanLogged   = false;
	bool                                                    redundantClearLogged      = false;
	bool                                                    spdClearSuppressionLogged = false;
	bool                                                    lastExecutionSucceeded    = false;
	bool                                                    directHistory             = false;
	AeronRenderTarget*                                      outputAliasTarget         = nullptr;
	AeronRenderTarget*                                      borrowedOutputTarget      = nullptr;
	AeronBuffer*                                            reconstructedDepth        = nullptr;
	ReconstructedDepthState reconstructedDepthState = ReconstructedDepthState::NeedsClear;
	PersistentClearState    spdMipsClearState       = PersistentClearState::NeedsClear;
	PersistentClearState    spdAtomicClearState     = PersistentClearState::NeedsClear;
	char                    fallbackReason[512] {};
	char                    lastFailedPipeline[128] {};
};

namespace {

#include "aeron_fsr3_shader_manifest.generated.h"

static const PipelineManifest* FindPipelineManifest(const AeronFsr3Backend* backend, FfxPass pass,
													bool requestedFp16) {
	for (const PipelineManifest& manifest : kFsr3PipelineManifests) {
		/* AMD sets ALLOW_FP16 broadly, then its blob accessor reuses the FP32
		 * shader for passes without a 16-bit implementation. The generated
		 * manifest records that per-pass distinction. */
		const bool precisionMatches = !manifest.fp16 || requestedFp16;
		const bool outputMatches =
			pass != FFX_FSR3UPSCALER_PASS_ACCUMULATE || manifest.directHistory == backend->directHistory;
		if (manifest.pass == pass && outputMatches && precisionMatches &&
			manifest.waveSpd == backend->waveSpd && manifest.lanczosLut == backend->lanczosLut &&
			std::strcmp(manifest.atomicLayout, backend->atomicLayout) == 0 &&
			std::strcmp(manifest.profile, backend->profileName) == 0) {
			return &manifest;
		}
	}
	return nullptr;
}

static bool IsInternalUpscaledColor(uint32_t resourceId) {
	return resourceId == FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1 ||
		   resourceId == FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2;
}

static bool IsSpdMipsResource(const ResourceEntry& entry) {
	return entry.resourceId == FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS;
}

static bool IsSpdAtomicResource(const ResourceEntry& entry) {
	return entry.resourceId == FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT;
}

static bool IsZeroInitialized(const FfxResourceInitData& initData) {
	if (initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE) {
		return initData.value == 0;
	}
	if (initData.type != FFX_RESOURCE_INIT_DATA_TYPE_BUFFER || !initData.buffer || initData.size == 0) {
		return false;
	}
	const uint8_t* bytes = static_cast<const uint8_t*>(initData.buffer);
	for (size_t index = 0; index < initData.size; ++index) {
		if (bytes[index] != 0) {
			return false;
		}
	}
	return true;
}

static PersistentClearState* SpdClearState(AeronFsr3Backend& backend, const ResourceEntry& entry) {
	if (IsSpdMipsResource(entry)) {
		return &backend.spdMipsClearState;
	}
	if (IsSpdAtomicResource(entry)) {
		return &backend.spdAtomicClearState;
	}
	return nullptr;
}

static void DestroyOwnedResource(ResourceEntry& entry) {
	if (!entry.owned) {
		return;
	}
	if (entry.renderTarget) {
		Aeron_DestroyRenderTarget(entry.renderTarget);
	} else {
		Aeron_DestroyTexture(entry.texture);
	}
	Aeron_DestroyBuffer(entry.buffer);
	entry.renderTarget = nullptr;
	entry.texture      = nullptr;
	entry.buffer       = nullptr;
}

static AeronFsr3Backend* Backend(FfxInterface* interfacePtr) {
	return interfacePtr ? static_cast<AeronFsr3Backend*>(interfacePtr->scratchBuffer) : nullptr;
}

static ResourceEntry* FindResource(AeronFsr3Backend* backend, FfxResourceInternal resource) {
	if (!backend || resource.internalIndex <= 0 ||
		resource.internalIndex >= static_cast<int32_t>(backend->resources.size())) {
		return nullptr;
	}
	ResourceEntry& entry = backend->resources[resource.internalIndex];
	return entry.occupied ? &entry : nullptr;
}

static int AllocateResourceIndex(AeronFsr3Backend* backend) {
	for (uint32_t index = 1; index < backend->resources.size(); ++index) {
		if (!backend->resources[index].occupied) {
			return static_cast<int>(index);
		}
	}
	return 0;
}

static void CopyWideName(wchar_t* destination, size_t count, const wchar_t* source) {
	if (!destination || count == 0) {
		return;
	}
	if (!source) {
		destination[0] = L'\0';
		return;
	}
	std::wcsncpy(destination, source, count - 1);
	destination[count - 1] = L'\0';
}

static void CopyResourceName(char* destination, size_t count, const wchar_t* source) {
	if (!destination || count == 0) {
		return;
	}
	destination[0] = '\0';
	if (!source) {
		return;
	}
	const size_t converted = std::wcstombs(destination, source, count - 1);
	if (converted == static_cast<size_t>(-1)) {
		destination[0] = '\0';
		return;
	}
	destination[converted] = '\0';
}

static AeronTextureFormat ToAeronFormat(FfxSurfaceFormat format) {
	switch (format) {
		case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT:
			return AERON_TEXTURE_FORMAT_RGBA32_FLOAT;
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:
			return AERON_TEXTURE_FORMAT_RGBA16_FLOAT;
		case FFX_SURFACE_FORMAT_R32G32_FLOAT:
			return AERON_TEXTURE_FORMAT_R32G32_FLOAT;
		case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:
			return AERON_TEXTURE_FORMAT_RGBA8_UNORM;
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT:
			return AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT;
		case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM:
			return AERON_TEXTURE_FORMAT_R10G10B10A2_UNORM;
		case FFX_SURFACE_FORMAT_R16G16_FLOAT:
			return AERON_TEXTURE_FORMAT_R16G16_FLOAT;
		case FFX_SURFACE_FORMAT_R16_FLOAT:
			return AERON_TEXTURE_FORMAT_R16_FLOAT;
		case FFX_SURFACE_FORMAT_R16_SNORM:
			return AERON_TEXTURE_FORMAT_R16_SNORM;
		case FFX_SURFACE_FORMAT_R8_UNORM:
			return AERON_TEXTURE_FORMAT_R8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8_UNORM:
			return AERON_TEXTURE_FORMAT_R8G8_UNORM;
		case FFX_SURFACE_FORMAT_R32_FLOAT:
			return AERON_TEXTURE_FORMAT_R32_FLOAT;
		case FFX_SURFACE_FORMAT_R32_UINT:
			return AERON_TEXTURE_FORMAT_R32_UINT;
		default:
			return AERON_TEXTURE_FORMAT_UNKNOWN;
	}
}

static FfxSurfaceFormat ToFfxFormat(AeronTextureFormat format) {
	switch (format) {
		case AERON_TEXTURE_FORMAT_RGBA32_FLOAT:
			return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
		case AERON_TEXTURE_FORMAT_RGBA16_FLOAT:
			return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case AERON_TEXTURE_FORMAT_R32G32_FLOAT:
			return FFX_SURFACE_FORMAT_R32G32_FLOAT;
		case AERON_TEXTURE_FORMAT_RGBA8_UNORM:
			return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT:
			return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
		case AERON_TEXTURE_FORMAT_R10G10B10A2_UNORM:
			return FFX_SURFACE_FORMAT_R10G10B10A2_UNORM;
		case AERON_TEXTURE_FORMAT_R16G16_FLOAT:
			return FFX_SURFACE_FORMAT_R16G16_FLOAT;
		case AERON_TEXTURE_FORMAT_R16_FLOAT:
			return FFX_SURFACE_FORMAT_R16_FLOAT;
		case AERON_TEXTURE_FORMAT_R16_SNORM:
			return FFX_SURFACE_FORMAT_R16_SNORM;
		case AERON_TEXTURE_FORMAT_R8_UNORM:
			return FFX_SURFACE_FORMAT_R8_UNORM;
		case AERON_TEXTURE_FORMAT_R8G8_UNORM:
			return FFX_SURFACE_FORMAT_R8G8_UNORM;
		case AERON_TEXTURE_FORMAT_R32_FLOAT:
		case AERON_TEXTURE_FORMAT_D32_FLOAT:
			return FFX_SURFACE_FORMAT_R32_FLOAT;
		case AERON_TEXTURE_FORMAT_R32_UINT:
			return FFX_SURFACE_FORMAT_R32_UINT;
		default:
			return FFX_SURFACE_FORMAT_UNKNOWN;
	}
}

static uint32_t BytesPerPixel(FfxSurfaceFormat format) {
	switch (format) {
		case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT:
			return 16;
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:
		case FFX_SURFACE_FORMAT_R32G32_FLOAT:
			return 8;
		case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT:
		case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM:
		case FFX_SURFACE_FORMAT_R32_FLOAT:
		case FFX_SURFACE_FORMAT_R32_UINT:
			return 4;
		case FFX_SURFACE_FORMAT_R16G16_FLOAT:
			return 4;
		case FFX_SURFACE_FORMAT_R16_FLOAT:
		case FFX_SURFACE_FORMAT_R16_SNORM:
		case FFX_SURFACE_FORMAT_R8G8_UNORM:
			return 2;
		case FFX_SURFACE_FORMAT_R8_UNORM:
			return 1;
		default:
			return 0;
	}
}

static uint32_t FullMipCount(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	while (width > 1 || height > 1) {
		width  = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		++levels;
	}
	return levels;
}

static uint64_t TextureBytes(const FfxResourceDescription& desc) {
	uint32_t width  = desc.width;
	uint32_t height = desc.height;
	uint32_t levels = desc.mipCount ? desc.mipCount : FullMipCount(width, height);
	uint64_t bytes  = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		bytes += static_cast<uint64_t>(width) * height * BytesPerPixel(desc.format);
		width  = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	return bytes;
}

static FfxVersionNumber GetSdkVersion(FfxInterface*) { return FFX_SDK_MAKE_VERSION(1, 1, 4); }

static FfxErrorCode GetMemoryUsage(FfxInterface* interfacePtr, FfxUInt32, FfxEffectMemoryUsage* usage) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend || !usage) {
		return FFX_ERROR_INVALID_POINTER;
	}
	usage->totalUsageInBytes     = backend->ownedBytes;
	usage->aliasableUsageInBytes = 0;
	return FFX_OK;
}

static FfxErrorCode CreateBackendContext(FfxInterface* interfacePtr, FfxEffect, FfxEffectBindlessConfig*,
										 FfxUInt32*    contextId) {
	if (!Backend(interfacePtr) || !contextId) {
		return FFX_ERROR_INVALID_POINTER;
	}
	*contextId = 1;
	return FFX_OK;
}

static FfxErrorCode GetCapabilities(FfxInterface* interfacePtr, FfxDeviceCapabilities* capabilities) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend || !capabilities) {
		return FFX_ERROR_INVALID_POINTER;
	}
	std::memset(capabilities, 0, sizeof(*capabilities));
	capabilities->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
	/* SDL_GPU does not currently expose subgroup widths. Zero leaves AMD's
	 * wave64 selection disabled instead of inventing a device capability. */
	capabilities->waveLaneCountMin = 0;
	capabilities->waveLaneCountMax = 0;
	capabilities->fp16Supported    = backend->fp16;
	return FFX_OK;
}

static FfxErrorCode DestroyBackendContext(FfxInterface*, FfxUInt32) { return FFX_OK; }

static bool UploadInitialTexture(AeronFsr3Backend* backend, AeronTexture* texture,
								 const FfxCreateResourceDescription& create) {
	if (create.initData.type == FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED) {
		return true;
	}
	const uint64_t bytes64 = TextureBytes(create.resourceDescription);
	if (bytes64 == 0 || bytes64 > UINT32_MAX) {
		return false;
	}
	const uint32_t bytes    = static_cast<uint32_t>(bytes64);
	const void*    source   = create.initData.buffer;
	void*          repeated = nullptr;
	if (create.initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE) {
		repeated = std::malloc(bytes);
		if (!repeated) {
			return false;
		}
		std::memset(repeated, create.initData.value, bytes);
		source = repeated;
	}
	AeronTextureUploadDesc upload {};
	upload.texture  = texture;
	upload.width    = static_cast<int>(create.resourceDescription.width);
	upload.height   = static_cast<int>(create.resourceDescription.height);
	upload.raw_data = source;
	upload.raw_size = create.initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER
						  ? static_cast<uint32_t>(create.initData.size)
						  : bytes;
	const bool ok = source && backend->initializationCommandBuffer &&
					Aeron_UploadTextureDataCmd(backend->initializationCommandBuffer, &upload) != 0;
	std::free(repeated);
	return ok;
}

static FfxErrorCode CreateResource(FfxInterface* interfacePtr, const FfxCreateResourceDescription* create,
								   FfxUInt32, FfxResourceInternal*                                 output) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend || !create || !output) {
		return FFX_ERROR_INVALID_POINTER;
	}
	const int index = AllocateResourceIndex(backend);
	if (index == 0) {
		return FFX_ERROR_OUT_OF_MEMORY;
	}

	ResourceEntry entry {};
	entry.occupied    = true;
	entry.owned       = true;
	entry.resourceId  = create->id;
	entry.description = create->resourceDescription;
	CopyResourceName(entry.name, sizeof(entry.name), create->name);
	if (entry.description.type == FFX_RESOURCE_TYPE_BUFFER) {
		AeronBufferDesc desc {};
		desc.size  = entry.description.size;
		desc.usage = AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE;
		desc.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY;
		desc.debug_name   = "FSR 3.1.4 buffer";
		entry.buffer      = Aeron_CreateBuffer(&desc);
		entry.bytes       = desc.size;
		if (entry.buffer && create->initData.type != FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED) {
			std::array<uint8_t, 16> valueData {};
			const void*             data = create->initData.buffer;
			if (create->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE) {
				if (desc.size > valueData.size()) {
					Aeron_DestroyBuffer(entry.buffer);
					return FFX_ERROR_INVALID_SIZE;
				}
				std::memset(valueData.data(), create->initData.value, desc.size);
				data = valueData.data();
			}
			if (!data || !backend->initializationCommandBuffer ||
				!Aeron_UploadBufferDataCmd(backend->initializationCommandBuffer, entry.buffer, 0, data,
										  desc.size)) {
				Aeron_DestroyBuffer(entry.buffer);
				return FFX_ERROR_BACKEND_API_ERROR;
			}
		}
	} else if (entry.description.type == FFX_RESOURCE_TYPE_TEXTURE2D) {
		const int mipCount = static_cast<int>(
			entry.description.mipCount ? entry.description.mipCount
									   : FullMipCount(entry.description.width, entry.description.height));
		uint32_t usage = AERON_TEXTURE_USAGE_TRANSFER_DST | AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ;
		if ((entry.description.usage & FFX_RESOURCE_USAGE_UAV) != 0) {
			usage |= AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE |
					 AERON_TEXTURE_USAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
		}
		if (backend->directHistory && IsInternalUpscaledColor(entry.resourceId)) {
			if (mipCount != 1) {
				return FFX_ERROR_INVALID_SIZE;
			}
			AeronRenderTargetDesc desc {};
			desc.width         = static_cast<int>(entry.description.width);
			desc.height        = static_cast<int>(entry.description.height);
			desc.format        = ToAeronFormat(entry.description.format);
			desc.usage         = usage;
			desc.debug_name    = "FSR 3.1.4 internal upscaled history";
			entry.renderTarget = Aeron_CreateRenderTarget(&desc);
			entry.texture      = Aeron_RenderTargetGetTexture(entry.renderTarget);
			if (!backend->outputAliasTarget) {
				backend->outputAliasTarget = entry.renderTarget;
			}
		} else {
			AeronTextureDesc desc {};
			desc.width      = static_cast<int>(entry.description.width);
			desc.height     = static_cast<int>(entry.description.height);
			desc.mip_count  = mipCount;
			desc.format     = ToAeronFormat(entry.description.format);
			desc.usage      = AERON_TEXTURE_USAGE_SAMPLED | usage;
			desc.debug_name = "FSR 3.1.4 texture";
			entry.texture   = Aeron_CreateTexture(&desc);
		}
		entry.bytes = TextureBytes(entry.description);
		if (entry.texture && !UploadInitialTexture(backend, entry.texture, *create)) {
			if (backend->outputAliasTarget == entry.renderTarget) {
				backend->outputAliasTarget = nullptr;
			}
			DestroyOwnedResource(entry);
			return FFX_ERROR_BACKEND_API_ERROR;
		}
	} else {
		return FFX_ERROR_INVALID_ENUM;
	}
	if (!entry.texture && !entry.buffer) {
		return FFX_ERROR_BACKEND_API_ERROR;
	}
	backend->resources[index] = entry;
	backend->ownedBytes += entry.bytes;
	if (IsSpdMipsResource(entry)) {
		/* Texture init uploads do not initialize every mip level. */
		backend->spdMipsClearState = PersistentClearState::NeedsClear;
	} else if (IsSpdAtomicResource(entry)) {
		backend->spdAtomicClearState = IsZeroInitialized(create->initData) ? PersistentClearState::Ready
																		   : PersistentClearState::NeedsClear;
	}
	output->internalIndex = index;
	return FFX_OK;
}

static FfxErrorCode RegisterResource(FfxInterface* interfacePtr, const FfxResource* input, FfxUInt32,
									 FfxResourceInternal* output) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend || !input || !output || !input->resource) {
		return FFX_ERROR_INVALID_POINTER;
	}
	const int index = AllocateResourceIndex(backend);
	if (index == 0) {
		return FFX_ERROR_OUT_OF_MEMORY;
	}
	ResourceEntry entry {};
	entry.occupied    = true;
	entry.dynamic     = true;
	entry.description = input->description;
	CopyResourceName(entry.name, sizeof(entry.name), input->name);
	if (entry.description.type == FFX_RESOURCE_TYPE_BUFFER) {
		entry.buffer = static_cast<AeronBuffer*>(input->resource);
	} else {
		entry.texture = static_cast<AeronTexture*>(input->resource);
	}
	backend->resources[index] = entry;
	output->internalIndex     = index;
	return FFX_OK;
}

static FfxResource GetResource(FfxInterface* interfacePtr, FfxResourceInternal resource) {
	FfxResource    result {};
	ResourceEntry* entry = FindResource(Backend(interfacePtr), resource);
	if (!entry) {
		return result;
	}
	result.resource = entry->buffer ? static_cast<void*>(entry->buffer) : static_cast<void*>(entry->texture);
	result.description = entry->description;
	result.state = (entry->description.usage & FFX_RESOURCE_USAGE_UAV) ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
																	   : FFX_RESOURCE_STATE_COMPUTE_READ;
	return result;
}

static FfxErrorCode UnregisterResources(FfxInterface* interfacePtr, FfxCommandList, FfxUInt32) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend) {
		return FFX_ERROR_INVALID_POINTER;
	}
	for (ResourceEntry& entry : backend->resources) {
		if (entry.dynamic) {
			entry = {};
		}
	}
	return FFX_OK;
}

static FfxErrorCode RegisterStaticResource(FfxInterface*, const FfxStaticResourceDescription*, FfxUInt32) {
	return FFX_ERROR_INVALID_ARGUMENT;
}

static FfxResourceDescription GetResourceDescription(FfxInterface*       interfacePtr,
													 FfxResourceInternal resource) {
	ResourceEntry* entry = FindResource(Backend(interfacePtr), resource);
	return entry ? entry->description : FfxResourceDescription {};
}

static FfxErrorCode DestroyResource(FfxInterface* interfacePtr, FfxResourceInternal resource, FfxUInt32) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	ResourceEntry*    entry   = FindResource(backend, resource);
	if (!entry) {
		return FFX_OK;
	}
	PersistentClearState* spdClearState = SpdClearState(*backend, *entry);
	if (entry->owned) {
		if (backend->outputAliasTarget == entry->renderTarget) {
			backend->outputAliasTarget = nullptr;
		}
		if (backend->borrowedOutputTarget == entry->renderTarget) {
			backend->borrowedOutputTarget = nullptr;
		}
		DestroyOwnedResource(*entry);
		backend->ownedBytes -= entry->bytes;
	}
	if (spdClearState) {
		*spdClearState = PersistentClearState::NeedsClear;
	}
	*entry = {};
	return FFX_OK;
}

static FfxErrorCode MapResource(FfxInterface*, FfxResourceInternal, void**) {
	return FFX_ERROR_INVALID_ARGUMENT;
}

static FfxErrorCode UnmapResource(FfxInterface*, FfxResourceInternal) { return FFX_ERROR_INVALID_ARGUMENT; }

static FfxErrorCode StageConstants(FfxInterface*, void* data, FfxUInt32 size, FfxConstantBuffer* output) {
	if (!data || !output || size == 0) {
		return FFX_ERROR_INVALID_POINTER;
	}
	output->data            = static_cast<uint32_t*>(data);
	output->num32BitEntries = (size + 3) / 4;
	return FFX_OK;
}

static void FillBindings(FfxResourceBinding* output, const wchar_t* const* names, uint32_t count) {
	for (uint32_t index = 0; index < count; ++index) {
		output[index].slotIndex  = index;
		output[index].arrayIndex = 0;
		CopyWideName(output[index].name, FFX_RESOURCE_NAME_SIZE, names[index]);
	}
}

static FfxErrorCode CreatePipeline(FfxInterface* interfacePtr, FfxEffect effect, FfxPass pass,
								   uint32_t permutationOptions, const FfxPipelineDescription*, FfxUInt32,
								   FfxPipelineState* output) {
	AeronFsr3Backend* backend   = Backend(interfacePtr);
	const uint32_t    passIndex = static_cast<uint32_t>(pass);
	if (!backend || !output || effect != FFX_EFFECT_FSR3UPSCALER ||
		passIndex >= FFX_FSR3UPSCALER_PASS_COUNT) {
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	const uint32_t requiredOptions = kHdrPermutation | kLowResMvPermutation | kInvertedDepthPermutation;
	const bool     sharpeningPass  = pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN;
	/* AMD's host sets its LUT bit from native-backend wave-width metadata. Aeron
	 * has already selected and validated the LUT profile, so a missing LUT bit
	 * is normalized to that profile; a requested unsupported LUT is rejected. */
	if ((permutationOptions & ~kKnownPermutations) != 0 ||
		(permutationOptions & requiredOptions) != requiredOptions ||
		(permutationOptions & kJitteredMvPermutation) != 0 ||
		(permutationOptions & kForceWave64Permutation) != 0 ||
		((permutationOptions & kSharpenPermutation) != 0) != sharpeningPass ||
		((permutationOptions & kLanczosLutPermutation) != 0 && !backend->lanczosLut)) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline),
					  "invalid permutation for pass %u", static_cast<unsigned>(pass));
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	const bool requestedFp16 = (permutationOptions & kAllowFp16Permutation) != 0;
	if (requestedFp16 && !backend->fp16) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline),
					  "pass %u requested FP16", static_cast<unsigned>(pass));
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	const PipelineManifest* manifestPtr = FindPipelineManifest(backend, pass, requestedFp16);
	if (!manifestPtr) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline),
					  "missing manifest for pass %u", static_cast<unsigned>(pass));
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	if (backend->pipelines[passIndex]) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline), "%s (duplicate)",
					  manifestPtr->shader);
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	const PipelineManifest&  manifest = *manifestPtr;
	AeronComputePipelineDesc desc {};
	desc.name                            = manifest.shader;
	desc.entrypoint                      = "CS";
	desc.sampler_count                   = manifest.sampledTextureCount;
	desc.readonly_storage_texture_count  = manifest.srvTextureCount - manifest.sampledTextureCount;
	desc.readonly_storage_buffer_count   = manifest.srvBufferCount;
	desc.readwrite_storage_texture_count = manifest.uavTextureCount;
	desc.readwrite_storage_buffer_count  = manifest.uavBufferCount;
	desc.uniform_buffer_count            = manifest.constantCount;
	desc.thread_count_x                  = manifest.threadX;
	desc.thread_count_y                  = manifest.threadY;
	desc.thread_count_z                  = 1;

	PipelineEntry* pipeline = new (std::nothrow) PipelineEntry {};
	if (!pipeline) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline), "%s",
					  manifest.shader);
		return FFX_ERROR_OUT_OF_MEMORY;
	}
	pipeline->pipeline = Aeron_CreateComputePipeline(&desc);
	pipeline->manifest = manifestPtr;
	if (!pipeline->pipeline) {
		std::snprintf(backend->lastFailedPipeline, sizeof(backend->lastFailedPipeline), "%s",
					  manifest.shader);
		delete pipeline;
		return FFX_ERROR_BACKEND_API_ERROR;
	}
	backend->pipelines[passIndex] = pipeline;

	std::memset(output, 0, sizeof(*output));
	output->passId          = pass;
	output->pipeline        = pipeline;
	output->srvTextureCount = manifest.srvTextureCount;
	output->srvBufferCount  = manifest.srvBufferCount;
	output->uavTextureCount = manifest.uavTextureCount;
	output->uavBufferCount  = manifest.uavBufferCount;
	output->constCount      = manifest.constantCount;
	FillBindings(output->srvTextureBindings, manifest.srvTextures, manifest.srvTextureCount);
	FillBindings(output->srvBufferBindings, manifest.srvBuffers, manifest.srvBufferCount);
	FillBindings(output->uavTextureBindings, manifest.uavTextures, manifest.uavTextureCount);
	FillBindings(output->uavBufferBindings, manifest.uavBuffers, manifest.uavBufferCount);
	FillBindings(output->constantBufferBindings, manifest.constants, manifest.constantCount);
	return FFX_OK;
}

static FfxErrorCode DestroyPipeline(FfxInterface* interfacePtr, FfxPipelineState* state, FfxUInt32) {
	if (!state) {
		return FFX_ERROR_INVALID_POINTER;
	}
	PipelineEntry* pipeline = static_cast<PipelineEntry*>(state->pipeline);
	if (pipeline) {
		AeronFsr3Backend* backend = Backend(interfacePtr);
		if (backend && state->passId < backend->pipelines.size() &&
			backend->pipelines[state->passId] == pipeline) {
			backend->pipelines[state->passId] = nullptr;
		}
		Aeron_DestroyComputePipeline(pipeline->pipeline);
		delete pipeline;
	}
	state->pipeline = nullptr;
	return FFX_OK;
}

static FfxErrorCode ScheduleJob(FfxInterface* interfacePtr, const FfxGpuJobDescription* job) {
	AeronFsr3Backend* backend = Backend(interfacePtr);
	if (!backend || !job) {
		return FFX_ERROR_INVALID_POINTER;
	}
	if (backend->jobCount >= backend->jobs.size()) {
		return FFX_ERROR_OUT_OF_MEMORY;
	}
	QueuedJob& queued = backend->jobs[backend->jobCount++];
	queued.job        = *job;
	if (job->jobType == FFX_GPU_JOB_COMPUTE) {
		for (uint32_t index = 0; index < job->computeJobDescriptor.pipeline.constCount; ++index) {
			const FfxConstantBuffer& source = job->computeJobDescriptor.cbs[index];
			const uint32_t           bytes  = source.num32BitEntries * sizeof(uint32_t);
			if (!source.data || bytes > kMaxConstantBytes) {
				--backend->jobCount;
				return FFX_ERROR_INVALID_SIZE;
			}
			std::memcpy(queued.constants[index].data(), source.data, bytes);
			queued.job.computeJobDescriptor.cbs[index].data =
				reinterpret_cast<uint32_t*>(queued.constants[index].data());
		}
	}
	return FFX_OK;
}

struct ClearTextureParams {
	float    color[4];
	uint32_t size[2];
	uint32_t padding[2];
};

struct ClearBufferParams {
	uint32_t value;
	uint32_t elementCount;
	uint32_t padding[2];
};

static bool IsReconstructedDepth(const AeronFsr3Backend& backend, const ResourceEntry& entry) {
	return entry.buffer && entry.buffer == backend.reconstructedDepth;
}

static bool ExecuteClear(AeronFsr3Backend* backend, AeronCommandBuffer* commandBuffer,
						 const FfxClearFloatJobDescription& clear) {
	ResourceEntry* target = FindResource(backend, clear.target);
	if (!target) {
		return false;
	}
	AeronComputePassDesc passDesc {};
	passDesc.command_buffer = commandBuffer;
	if (target->texture) {
		uint32_t       width    = target->description.width;
		uint32_t       height   = target->description.height;
		const uint32_t mipCount = static_cast<uint32_t>(Aeron_TextureGetMipCount(target->texture));
		if (!target->clearPlanLogged) {
			Aeron_Log("fsr3", "clear plan resource %u (%s): %ux%u, %u mips, %llu bytes%s", target->resourceId,
					  target->name[0] ? target->name : "unnamed", width, height, mipCount,
					  static_cast<unsigned long long>(target->bytes),
					  IsSpdMipsResource(*target) ? (backend->waveSpd ? ", wave SPD clears mips 0-5"
																	 : ", scalar SPD clears mips 0-2 and 5")
												 : "");
			target->clearPlanLogged = true;
		}
		for (uint32_t mip = 0; mip < mipCount; ++mip) {
			const bool spdResource = IsSpdMipsResource(*target);
			const bool clearSpdMip = backend->waveSpd ? mip <= 5 : (mip <= 2 || mip == 5);
			if (spdResource && !clearSpdMip) {
				width  = width > 1 ? width / 2 : 1;
				height = height > 1 ? height / 2 : 1;
				continue;
			}
			char debugLabel[160];
			std::snprintf(debugLabel, sizeof(debugLabel), "FSR 3.1.4 clear %s mip %u",
						  target->name[0] ? target->name : "texture", mip);
			passDesc.debug_label = debugLabel;
			AeronComputeTextureBinding binding { target->texture, mip, 0, 0 };
			passDesc.write_textures      = &binding;
			passDesc.write_texture_count = 1;
			AeronComputePass* pass       = Aeron_BeginComputePass(&passDesc);
			if (!pass) {
				return false;
			}
			ClearTextureParams params {};
			std::memcpy(params.color, clear.color, sizeof(params.color));
			params.size[0] = width;
			params.size[1] = height;
			Aeron_BindComputePipeline(pass, backend->clearTexturePipeline);
			Aeron_BindComputeUniformData(pass, 0, &params, sizeof(params));
			Aeron_DispatchCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
			Aeron_EndComputePass(pass);
			width  = width > 1 ? width / 2 : 1;
			height = height > 1 ? height / 2 : 1;
		}
		return true;
	}

	if (!target->clearPlanLogged && (!target->dynamic || !backend->externalClearPlanLogged)) {
		Aeron_Log("fsr3", "clear plan resource %u (%s): %u-byte buffer", target->resourceId,
				  target->name[0] ? target->name : "unnamed", target->description.size);
		if (target->dynamic) {
			backend->externalClearPlanLogged = true;
		}
	}
	target->clearPlanLogged = true;
	AeronComputeBufferBinding binding { target->buffer, 0 };
	char                      debugLabel[160];
	std::snprintf(debugLabel, sizeof(debugLabel), "FSR 3.1.4 clear %s",
				  target->name[0] ? target->name : "buffer");
	passDesc.debug_label        = debugLabel;
	passDesc.write_buffers      = &binding;
	passDesc.write_buffer_count = 1;
	AeronComputePass* pass      = Aeron_BeginComputePass(&passDesc);
	if (!pass) {
		return false;
	}
	ClearBufferParams params {};
	std::memcpy(&params.value, &clear.color[0], sizeof(params.value));
	params.elementCount = target->description.size / sizeof(uint32_t);
	Aeron_BindComputePipeline(pass, backend->clearBufferPipeline);
	Aeron_BindComputeUniformData(pass, 0, &params, sizeof(params));
	Aeron_DispatchCompute(pass, (params.elementCount + 63) / 64, 1, 1);
	Aeron_EndComputePass(pass);
	return true;
}

static bool ExecuteCompute(AeronFsr3Backend* backend, AeronCommandBuffer* commandBuffer,
						   const FfxComputeJobDescription& compute) {
	PipelineEntry* pipeline = static_cast<PipelineEntry*>(compute.pipeline.pipeline);
	if (!pipeline || !pipeline->pipeline) {
		return false;
	}
	std::array<AeronComputeTextureBinding, FFX_MAX_NUM_UAVS> writeTextures {};
	std::array<AeronComputeBufferBinding, FFX_MAX_NUM_UAVS>  writeBuffers {};
	AeronRenderTarget*                                       accumulatedOutput = nullptr;
	for (uint32_t index = 0; index < compute.pipeline.uavTextureCount; ++index) {
		ResourceEntry* entry = FindResource(backend, compute.uavTextures[index].resource);
		if (!entry || !entry->texture) {
			return false;
		}
#ifndef NDEBUG
		const AeronTexture* outputAlias =
			backend->outputAliasTarget ? Aeron_RenderTargetGetTexture(backend->outputAliasTarget) : nullptr;
		if (backend->directHistory && entry->texture == outputAlias &&
			std::wcscmp(pipeline->manifest->uavTextures[index], L"rw_upscaled_output") == 0) {
			assert(false && "direct-history output alias was bound as the external output");
		}
#endif
		writeTextures[index] = { entry->texture, compute.uavTextures[index].mip, 0, 0 };
		if (pipeline->manifest->pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE &&
			pipeline->manifest->directHistory && IsInternalUpscaledColor(entry->resourceId)) {
			accumulatedOutput = entry->renderTarget;
		}
	}
	if (pipeline->manifest->pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE && pipeline->manifest->directHistory &&
		!accumulatedOutput) {
		return false;
	}
	for (uint32_t index = 0; index < compute.pipeline.uavBufferCount; ++index) {
		ResourceEntry* entry = FindResource(backend, compute.uavBuffers[index].resource);
		if (!entry || !entry->buffer) {
			return false;
		}
		writeBuffers[index] = { entry->buffer, 0 };
	}

	AeronComputePassDesc passDesc {};
	passDesc.command_buffer      = commandBuffer;
	passDesc.write_textures      = writeTextures.data();
	passDesc.write_texture_count = compute.pipeline.uavTextureCount;
	passDesc.write_buffers       = writeBuffers.data();
	passDesc.write_buffer_count  = compute.pipeline.uavBufferCount;
	passDesc.debug_label         = pipeline->manifest->shader;
	AeronComputePass* pass       = Aeron_BeginComputePass(&passDesc);
	if (!pass) {
		return false;
	}
	Aeron_BindComputePipeline(pass, pipeline->pipeline);

	for (uint32_t index = 0; index < compute.pipeline.srvTextureCount; ++index) {
		ResourceEntry* entry = FindResource(backend, compute.srvTextures[index].resource);
		if (!entry || !entry->texture) {
			Aeron_EndComputePass(pass);
			return false;
		}
		if (index < pipeline->manifest->sampledTextureCount) {
			const SamplerKind samplerKind = pipeline->manifest->samplers[index];
			AeronSampler*     sampler =
				samplerKind == SamplerKind::PointClamp ? backend->pointSampler : backend->linearSampler;
			Aeron_BindComputeTextureSampler(pass, index, entry->texture, sampler);
		} else {
			Aeron_BindComputeStorageTexture(pass, index - pipeline->manifest->sampledTextureCount,
											entry->texture);
		}
	}
	for (uint32_t index = 0; index < compute.pipeline.srvBufferCount; ++index) {
		ResourceEntry* entry = FindResource(backend, compute.srvBuffers[index].resource);
		if (!entry || !entry->buffer) {
			Aeron_EndComputePass(pass);
			return false;
		}
		Aeron_BindComputeStorageBuffer(pass, index, entry->buffer);
	}
	for (uint32_t index = 0; index < compute.pipeline.constCount; ++index) {
		const FfxConstantBuffer& constant = compute.cbs[index];
		Aeron_BindComputeUniformData(pass, index, constant.data, constant.num32BitEntries * sizeof(uint32_t));
	}
	Aeron_DispatchCompute(pass, compute.dimensions[0], compute.dimensions[1], compute.dimensions[2]);
	Aeron_EndComputePass(pass);
	if (accumulatedOutput) {
		backend->borrowedOutputTarget = accumulatedOutput;
	}
	if (pipeline->manifest->pass == FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS) {
		backend->reconstructedDepthState = ReconstructedDepthState::Dirty;
	} else if (pipeline->manifest->pass == FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY) {
		backend->reconstructedDepthState = ReconstructedDepthState::Ready;
	}
	return true;
}

static bool ClearIsSuperseded(const AeronFsr3Backend& backend, uint32_t jobIndex) {
	const FfxResourceInternal target = backend.jobs[jobIndex].job.clearJobDescriptor.target;
	for (uint32_t index = jobIndex + 1; index < backend.jobCount; ++index) {
		const FfxGpuJobDescription& later = backend.jobs[index].job;
		if (later.jobType == FFX_GPU_JOB_COMPUTE) {
			return false;
		}
		if (later.jobType == FFX_GPU_JOB_CLEAR_FLOAT &&
			later.clearJobDescriptor.target.internalIndex == target.internalIndex) {
			return true;
		}
	}
	return false;
}

static FfxErrorCode ExecuteJobs(FfxInterface* interfacePtr, FfxCommandList commandList, FfxUInt32) {
	AeronFsr3Backend*   backend       = Backend(interfacePtr);
	AeronCommandBuffer* commandBuffer = static_cast<AeronCommandBuffer*>(commandList);
	if (!backend || !commandBuffer) {
		return FFX_ERROR_INVALID_POINTER;
	}
	bool ok = true;
	for (uint32_t index = 0; index < backend->jobCount && ok; ++index) {
		const FfxGpuJobDescription& job = backend->jobs[index].job;
		switch (job.jobType) {
			case FFX_GPU_JOB_CLEAR_FLOAT: {
				if (ClearIsSuperseded(*backend, index)) {
					if (!backend->redundantClearLogged) {
						Aeron_Log("fsr3", "suppressed reset clear superseded before the first FSR dispatch");
						backend->redundantClearLogged = true;
					}
					break;
				}
				ResourceEntry*        target = FindResource(backend, job.clearJobDescriptor.target);
				const bool            reconstructedDepth = target && IsReconstructedDepth(*backend, *target);
				PersistentClearState* spdClearState = target ? SpdClearState(*backend, *target) : nullptr;
				if (reconstructedDepth &&
					backend->reconstructedDepthState == ReconstructedDepthState::Ready) {
					break;
				}
				if (spdClearState && *spdClearState == PersistentClearState::Ready) {
					if (target && IsSpdMipsResource(*target) && !backend->spdClearSuppressionLogged) {
						Aeron_Log("fsr3", "suppressed steady-state SPD atomic and mip clears");
						backend->spdClearSuppressionLogged = true;
					}
					break;
				}
				ok = ExecuteClear(backend, commandBuffer, job.clearJobDescriptor);
				if (ok && reconstructedDepth) {
					backend->reconstructedDepthState = ReconstructedDepthState::Ready;
				}
				if (ok && spdClearState) {
					*spdClearState = PersistentClearState::Ready;
				}
				break;
			}
			case FFX_GPU_JOB_COMPUTE:
				ok = ExecuteCompute(backend, commandBuffer, job.computeJobDescriptor);
				break;
			case FFX_GPU_JOB_BARRIER:
			case FFX_GPU_JOB_DISCARD:
				break;
			default:
				ok = false;
				break;
		}
	}
	if (!ok) {
		backend->reconstructedDepthState = ReconstructedDepthState::NeedsClear;
		backend->spdMipsClearState       = PersistentClearState::NeedsClear;
		backend->spdAtomicClearState     = PersistentClearState::NeedsClear;
		backend->borrowedOutputTarget    = nullptr;
	}
	backend->jobCount               = 0;
	backend->lastExecutionSucceeded = ok;
	return ok ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
}

} // namespace

AeronFsr3Backend* AeronFsr3Backend_Create(uint32_t profileIndex, bool directHistory) {
	if (profileIndex >= std::size(kFsr3AvailableProfiles)) {
		return nullptr;
	}
	AeronFsr3Backend* backend = new (std::nothrow) AeronFsr3Backend {};
	if (!backend) {
		return nullptr;
	}
	backend->profileName   = kFsr3AvailableProfiles[profileIndex].profile;
	backend->directHistory = directHistory;
	backend->fp16          = kFsr3AvailableProfiles[profileIndex].fp16;
	backend->waveSpd       = kFsr3AvailableProfiles[profileIndex].waveSpd;
	backend->lanczosLut    = kFsr3AvailableProfiles[profileIndex].lanczosLut;
	backend->atomicLayout  = kFsr3AvailableProfiles[profileIndex].atomicLayout;
	for (const UnavailableProfile& unavailable : kFsr3UnavailableProfiles) {
		if (!unavailable.profile) {
			break;
		}
		const size_t used = std::strlen(backend->fallbackReason);
		if (used >= sizeof(backend->fallbackReason) - 1) {
			break;
		}
		std::snprintf(backend->fallbackReason + used, sizeof(backend->fallbackReason) - used, "%s%s: %s",
					  used ? "; " : "", unavailable.profile, unavailable.reason);
	}
	Aeron_Log("fsr3", "shader profile %s%s (manifest %s)%s%s", backend->profileName,
			  backend->directHistory ? " with direct history output" : "", kFsr3ManifestHash,
			  backend->fallbackReason[0] ? "; unavailable profiles: " : "", backend->fallbackReason);
	AeronSamplerDesc samplerDesc {};
	samplerDesc.min_filter = AERON_FILTER_LINEAR;
	samplerDesc.mag_filter = AERON_FILTER_LINEAR;
	samplerDesc.mip_filter = AERON_FILTER_LINEAR;
	samplerDesc.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE;
	samplerDesc.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE;
	samplerDesc.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE;
	backend->linearSampler = Aeron_CreateSampler(&samplerDesc);
	samplerDesc.min_filter = AERON_FILTER_NEAREST;
	samplerDesc.mag_filter = AERON_FILTER_NEAREST;
	samplerDesc.mip_filter = AERON_FILTER_NEAREST;
	backend->pointSampler  = Aeron_CreateSampler(&samplerDesc);

	AeronComputePipelineDesc pipelineDesc {};
	pipelineDesc.name                            = "compute_fill.comp";
	pipelineDesc.readwrite_storage_texture_count = 1;
	pipelineDesc.uniform_buffer_count            = 1;
	pipelineDesc.thread_count_x                  = 8;
	pipelineDesc.thread_count_y                  = 8;
	pipelineDesc.thread_count_z                  = 1;
	backend->clearTexturePipeline                = Aeron_CreateComputePipeline(&pipelineDesc);
	pipelineDesc.name                            = "compute_fill_buffer.comp";
	pipelineDesc.readwrite_storage_texture_count = 0;
	pipelineDesc.readwrite_storage_buffer_count  = 1;
	pipelineDesc.thread_count_x                  = 64;
	pipelineDesc.thread_count_y                  = 1;
	backend->clearBufferPipeline                 = Aeron_CreateComputePipeline(&pipelineDesc);
	if (!backend->linearSampler || !backend->pointSampler || !backend->clearTexturePipeline ||
		!backend->clearBufferPipeline) {
		AeronFsr3Backend_Destroy(backend);
		return nullptr;
	}
	backend->initializationCommandBuffer = Aeron_AcquireCommandBuffer();
	if (!backend->initializationCommandBuffer) {
		AeronFsr3Backend_Destroy(backend);
		return nullptr;
	}
	return backend;
}

bool AeronFsr3Backend_FinishInitialization(AeronFsr3Backend* backend) {
	if (!backend || !backend->initializationCommandBuffer) {
		return false;
	}
	AeronCommandBuffer* commandBuffer = backend->initializationCommandBuffer;
	backend->initializationCommandBuffer = nullptr;
	return Aeron_SubmitCommandBuffer(commandBuffer) != 0;
}

void AeronFsr3Backend_Destroy(AeronFsr3Backend* backend) {
	if (!backend) {
		return;
	}
	if (backend->initializationCommandBuffer) {
		Aeron_CancelCommandBuffer(backend->initializationCommandBuffer);
		backend->initializationCommandBuffer = nullptr;
	}
	for (PipelineEntry*& pipeline : backend->pipelines) {
		if (pipeline) {
			Aeron_DestroyComputePipeline(pipeline->pipeline);
			delete pipeline;
			pipeline = nullptr;
		}
	}
	for (ResourceEntry& entry : backend->resources) {
		if (entry.occupied && entry.owned) {
			DestroyOwnedResource(entry);
		}
	}
	Aeron_DestroyComputePipeline(backend->clearTexturePipeline);
	Aeron_DestroyComputePipeline(backend->clearBufferPipeline);
	Aeron_DestroySampler(backend->linearSampler);
	Aeron_DestroySampler(backend->pointSampler);
	delete backend;
}

FfxInterface AeronFsr3Backend_GetInterface(AeronFsr3Backend* backend) {
	FfxInterface interfacePtr {};
	interfacePtr.fpGetSDKVersion               = GetSdkVersion;
	interfacePtr.fpGetEffectGpuMemoryUsage     = GetMemoryUsage;
	interfacePtr.fpCreateBackendContext        = CreateBackendContext;
	interfacePtr.fpGetDeviceCapabilities       = GetCapabilities;
	interfacePtr.fpDestroyBackendContext       = DestroyBackendContext;
	interfacePtr.fpCreateResource              = CreateResource;
	interfacePtr.fpRegisterResource            = RegisterResource;
	interfacePtr.fpGetResource                 = GetResource;
	interfacePtr.fpUnregisterResources         = UnregisterResources;
	interfacePtr.fpRegisterStaticResource      = RegisterStaticResource;
	interfacePtr.fpGetResourceDescription      = GetResourceDescription;
	interfacePtr.fpDestroyResource             = DestroyResource;
	interfacePtr.fpMapResource                 = MapResource;
	interfacePtr.fpUnmapResource               = UnmapResource;
	interfacePtr.fpStageConstantBufferDataFunc = StageConstants;
	interfacePtr.fpCreatePipeline              = CreatePipeline;
	interfacePtr.fpDestroyPipeline             = DestroyPipeline;
	interfacePtr.fpScheduleGpuJob              = ScheduleJob;
	interfacePtr.fpExecuteGpuJobs              = ExecuteJobs;
	interfacePtr.scratchBuffer                 = backend;
	interfacePtr.scratchBufferSize             = sizeof(*backend);
	interfacePtr.device                        = backend;
	return interfacePtr;
}

const char* AeronFsr3Backend_ProfileName(const AeronFsr3Backend* backend) {
	return backend ? backend->profileName : "unavailable";
}

uint32_t AeronFsr3Backend_ProfileCount(void) {
	return static_cast<uint32_t>(std::size(kFsr3AvailableProfiles));
}

uint32_t AeronFsr3Backend_ManifestSchema(void) { return kFsr3ManifestSchema; }

const char* AeronFsr3Backend_ManifestHash(void) { return kFsr3ManifestHash; }

const char* AeronFsr3Backend_FallbackReason(const AeronFsr3Backend* backend) {
	return backend ? backend->fallbackReason : "FSR backend unavailable";
}

const char* AeronFsr3Backend_LastFailedPipeline(const AeronFsr3Backend* backend) {
	return backend && backend->lastFailedPipeline[0] ? backend->lastFailedPipeline : "unknown pipeline";
}

void AeronFsr3Backend_AppendFallbackReason(AeronFsr3Backend* backend, const char* reason) {
	if (!backend || !reason || !reason[0]) {
		return;
	}
	const size_t used = std::strlen(backend->fallbackReason);
	if (used < sizeof(backend->fallbackReason) - 1) {
		std::snprintf(backend->fallbackReason + used, sizeof(backend->fallbackReason) - used, "%s%s",
					  used ? "; " : "", reason);
	}
}

bool AeronFsr3Backend_UsesFp16(const AeronFsr3Backend* backend) { return backend && backend->fp16; }

bool AeronFsr3Backend_UsesWaveSpd(const AeronFsr3Backend* backend) { return backend && backend->waveSpd; }

bool AeronFsr3Backend_UsesLanczosLut(const AeronFsr3Backend* backend) {
	return backend && backend->lanczosLut;
}

bool AeronFsr3Backend_UsesDirectHistory(const AeronFsr3Backend* backend) {
	return backend && backend->directHistory;
}

const char* AeronFsr3Backend_AtomicLayout(const AeronFsr3Backend* backend) {
	return backend ? backend->atomicLayout : "unavailable";
}

void AeronFsr3Backend_BeginDispatch(AeronFsr3Backend* backend) {
	if (!backend) {
		return;
	}
	backend->lastExecutionSucceeded = false;
	backend->borrowedOutputTarget   = nullptr;
}

bool AeronFsr3Backend_LastExecutionSucceeded(const AeronFsr3Backend* backend) {
	return backend && backend->lastExecutionSucceeded;
}

AeronTexture* AeronFsr3Backend_OutputAlias(const AeronFsr3Backend* backend) {
	return backend && backend->outputAliasTarget ? Aeron_RenderTargetGetTexture(backend->outputAliasTarget)
												 : nullptr;
}

AeronRenderTarget* AeronFsr3Backend_BorrowedOutputTarget(const AeronFsr3Backend* backend) {
	return backend ? backend->borrowedOutputTarget : nullptr;
}

void AeronFsr3Backend_SetReconstructedDepthInitialized(AeronFsr3Backend* backend, AeronBuffer* buffer) {
	if (!backend || !buffer) {
		return;
	}
	backend->reconstructedDepth      = buffer;
	backend->reconstructedDepthState = ReconstructedDepthState::Ready;
}

FfxResource AeronFsr3Backend_TextureResource(AeronTexture* texture, const wchar_t* name,
											 FfxResourceStates state) {
	FfxResource resource {};
	resource.resource             = texture;
	resource.description.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
	resource.description.format   = ToFfxFormat(Aeron_TextureGetFormat(texture));
	resource.description.width    = static_cast<uint32_t>(Aeron_TextureGetWidth(texture));
	resource.description.height   = static_cast<uint32_t>(Aeron_TextureGetHeight(texture));
	resource.description.depth    = 1;
	resource.description.mipCount = static_cast<uint32_t>(Aeron_TextureGetMipCount(texture));
	resource.description.usage = (Aeron_TextureGetUsage(texture) & AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE)
									 ? FFX_RESOURCE_USAGE_UAV
									 : FFX_RESOURCE_USAGE_READ_ONLY;
	resource.state             = state;
	CopyWideName(resource.name, FFX_RESOURCE_NAME_SIZE, name);
	return resource;
}

FfxResource AeronFsr3Backend_BufferResource(AeronBuffer* buffer, uint32_t stride, const wchar_t* name,
											FfxResourceStates state) {
	FfxResource resource {};
	resource.resource           = buffer;
	resource.description.type   = FFX_RESOURCE_TYPE_BUFFER;
	resource.description.format = FFX_SURFACE_FORMAT_UNKNOWN;
	resource.description.size   = Aeron_BufferSize(buffer);
	resource.description.stride = stride;
	resource.description.usage  = (Aeron_BufferUsageFlags(buffer) & AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE)
									  ? FFX_RESOURCE_USAGE_UAV
									  : FFX_RESOURCE_USAGE_READ_ONLY;
	resource.state              = state;
	CopyWideName(resource.name, FFX_RESOURCE_NAME_SIZE, name);
	return resource;
}
