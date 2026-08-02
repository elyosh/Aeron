#include "gpu_debug.h"
#include "internal.h"

#include <limits.h>
#include <stdarg.h>
#include <string.h>

#if defined(AERON_HAS_VULKAN_FP16_DEVICE_OPTIONS)
#include <vulkan/vulkan.h>
#endif

typedef struct AeronShaderFormatInfo {
	SDL_GPUShaderFormat format;
	const char*         extension;
	const char*         name;
	const char*         entrypoint;
} AeronShaderFormatInfo;

static const AeronShaderFormatInfo g_shaderFormats[] = {
#if defined(AERON_SHADER_FORMAT_MSL)
	{ SDL_GPU_SHADERFORMAT_MSL, "msl", "MSL", "main0" },
#endif
#if defined(AERON_SHADER_FORMAT_SPIRV)
	{ SDL_GPU_SHADERFORMAT_SPIRV, "spv", "SPIR-V", "main" },
#endif
#if defined(AERON_SHADER_FORMAT_DXIL)
	{ SDL_GPU_SHADERFORMAT_DXIL, "dxil", "DXIL", "main" },
#endif
};

static void Aeron_SetRenderError(const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	SDL_vsnprintf(g_aeron.render_error, sizeof(g_aeron.render_error), fmt, args);
	va_end(args);
	Aeron_LogError("aeron", "%s", g_aeron.render_error);
}

const char* Aeron_RenderLastError(void) {
	return g_aeron.render_error[0] ? g_aeron.render_error : "No renderer diagnostic was recorded.";
}

void Aeron_RequestFatalRendererError(const char* operation) {
	char message[1024];

	SDL_snprintf(message, sizeof(message),
				 "An unexpected rendering error occurred during %s.\n\n"
				 "GPU backend: %s\n"
				 "Error: %s\n\n"
				 "The application cannot continue safely. See the log for additional context.",
				 operation && operation[0] ? operation : "GPU work", Aeron_RenderDriverName(),
				 Aeron_RenderLastError());
	Aeron_RequestFatalError("Renderer Error", message);
}

static void Aeron_RecordUniformPush(uint32_t size) {
	g_aeron.render_data_stats.total_uniform_bytes += size;
	if (size > g_aeron.render_data_stats.max_uniform_bytes) {
		g_aeron.render_data_stats.max_uniform_bytes = size;
	}
}

static void Aeron_RecordStorageUpload(const AeronBuffer* buffer, uint32_t size) {
	if (buffer && (buffer->usage & AERON_BUFFER_USAGE_STORAGE) != 0) {
		g_aeron.render_data_stats.storage_upload_bytes += size;
	}
}

static SDL_GPUShaderFormat Aeron_CompiledShaderFormats(void) {
	SDL_GPUShaderFormat formats = 0;

#if defined(AERON_SHADER_FORMAT_MSL)
	formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#if defined(AERON_SHADER_FORMAT_SPIRV)
	formats |= SDL_GPU_SHADERFORMAT_SPIRV;
#endif
#if defined(AERON_SHADER_FORMAT_DXIL)
	formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
	return formats;
}

static int Aeron_BuildShaderPath(char* dst, size_t dst_size, const char* shader_name, const char* extension) {
	int length;

	if (!dst || !dst_size || !shader_name || !extension) {
		return 0;
	}

	length = SDL_snprintf(dst, dst_size, "%s/%s.%s", g_aeron.shader_root, shader_name, extension);
	return length >= 0 && (size_t)length < dst_size;
}

/* Resolves a shader artifact against the configured application shader root.
 * Returns the loaded bytes (SDL_free) or NULL. */
static Uint8* Aeron_LoadShaderFile(const char* shader_name, const char* extension, size_t* out_size,
								   char* path_out, size_t path_out_size) {
	if (!Aeron_BuildShaderPath(path_out, path_out_size, shader_name, extension)) {
		SDL_SetError("shader path is too long");
		return NULL;
	}
	return (Uint8*)SDL_LoadFile(path_out, out_size);
}

static const AeronShaderFormatInfo* Aeron_SelectShaderFormat(void) {
	SDL_GPUShaderFormat supported_formats;
	size_t              i;

	supported_formats = SDL_GetGPUShaderFormats(g_aeron.gpu_device);
	for (i = 0; i < sizeof(g_shaderFormats) / sizeof(g_shaderFormats[0]); i++) {
		if ((supported_formats & g_shaderFormats[i].format) != 0) {
			return &g_shaderFormats[i];
		}
	}

	return NULL;
}

static SDL_GPUShader* Aeron_LoadShader(const char* shader_name, SDL_GPUShaderStage stage,
									   const AeronShaderFormatInfo* format_info, uint32_t num_samplers,
									   uint32_t num_uniform_buffers, uint32_t num_storage_buffers) {
	char                    path[AERON_MAX_PATH];
	size_t                  code_size;
	Uint8*                  code;
	SDL_GPUShaderCreateInfo create_info;
	SDL_GPUShader*          shader;

	if (!format_info) {
		return NULL;
	}

	code = Aeron_LoadShaderFile(shader_name, format_info->extension, &code_size, path, sizeof(path));
	if (!code) {
		Aeron_SetRenderError("Failed to load %s shader '%s': %s", format_info->name, path, SDL_GetError());
		return NULL;
	}

	SDL_zero(create_info);
	create_info.code_size           = code_size;
	create_info.code                = code;
	create_info.entrypoint          = format_info->entrypoint;
	create_info.format              = format_info->format;
	create_info.stage               = stage;
	create_info.num_samplers        = num_samplers;
	create_info.num_uniform_buffers = num_uniform_buffers;
	create_info.num_storage_buffers = num_storage_buffers;

	shader = SDL_CreateGPUShader(g_aeron.gpu_device, &create_info);
	if (!shader) {
		Aeron_SetRenderError("SDL_CreateGPUShader failed for '%s': %s", path, SDL_GetError());
	}

	SDL_free(code);
	return shader;
}

static int Aeron_LoadBuiltinShaders(void) {
	const AeronShaderFormatInfo* format_info;

	format_info = Aeron_SelectShaderFormat();
	if (!format_info) {
		Aeron_LogError("aeron", "GPU backend does not support Aeron compiled shader formats");
		return 0;
	}

	Aeron_LogInfo("aeron", "Loading %s shaders for SDL GPU driver '%s'", format_info->name,
			  SDL_GetGPUDeviceDriver(g_aeron.gpu_device));

	g_aeron.fullscreen_vertex_shader =
		Aeron_LoadShader("fullscreen.vert", SDL_GPU_SHADERSTAGE_VERTEX, format_info, 0, 0, 0);
	g_aeron.fullscreen_fragment_shader =
		Aeron_LoadShader("fullscreen.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, format_info, 1, 1, 0);

	return g_aeron.fullscreen_vertex_shader && g_aeron.fullscreen_fragment_shader;
}

static AeronTextureFormat Aeron_FromSdlTextureFormat(SDL_GPUTextureFormat format);

/* Applies the best available swapchain composition for the requested output
 * mode: HDR extended-linear when hdr is nonzero (no silent SDR fallback — the
 * caller keeps its previous mode on failure), otherwise SDR-linear with a
 * plain-SDR fallback. */
static int Aeron_ConfigureSwapchain(int hdr) {
	SDL_GPUSwapchainComposition composition;

	if (hdr) {
		composition = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
		if (!SDL_WindowSupportsGPUSwapchainComposition(g_aeron.gpu_device, g_aeron.window, composition)) {
			Aeron_SetRenderError("HDR extended-linear swapchain composition is unavailable");
			return 0;
		}
		SDL_ClearError();
		if (!SDL_SetGPUSwapchainParameters(g_aeron.gpu_device, g_aeron.window, composition,
										  SDL_GPU_PRESENTMODE_VSYNC)) {
			const char* error = SDL_GetError();
			Aeron_SetRenderError("SDL_SetGPUSwapchainParameters failed for HDR output: %s",
								 error && error[0] ? error : "<no SDL error provided>");
			return 0;
		}
	} else {
		composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
		if (SDL_WindowSupportsGPUSwapchainComposition(g_aeron.gpu_device, g_aeron.window,
													  SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
			composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
		}

			if (!SDL_SetGPUSwapchainParameters(g_aeron.gpu_device, g_aeron.window, composition,
											   SDL_GPU_PRESENTMODE_VSYNC)) {
				if (composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR ||
					!SDL_SetGPUSwapchainParameters(g_aeron.gpu_device, g_aeron.window,
												   SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) {
					const char* error = SDL_GetError();
					Aeron_SetRenderError("SDL_SetGPUSwapchainParameters failed for SDR output: %s",
										 error && error[0] ? error : "<no SDL error provided>");
					return 0;
				}
			composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
		}
	}

	g_aeron.swapchain_composition = composition;
	g_aeron.swapchain_format      = SDL_GetGPUSwapchainTextureFormat(g_aeron.gpu_device, g_aeron.window);
	if (g_aeron.swapchain_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		Aeron_SetRenderError("SDL returned an invalid swapchain texture format");
		return 0;
	}
	g_aeron.hdr_output_enabled    = hdr;
	/* The debug overlay's ImGui pipeline is keyed on the swapchain
	 * format; rebuild it on composition flips (no-op before its init). */
	Aeron_DebugUiOnSwapchainFormatChanged(g_aeron.swapchain_format);
	return 1;
}

int Aeron_OutputSupportsHdr(void) {
	if (!g_aeron.gpu_device || !g_aeron.window) {
		Aeron_SetRenderError("Cannot reconfigure output without a GPU device and window");
		return 0;
	}

	return SDL_WindowSupportsGPUSwapchainComposition(g_aeron.gpu_device, g_aeron.window,
													 SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR)
			   ? 1
			   : 0;
}

/* Caches the window's HDR display properties. SDL keeps them current and
 * signals SDL_EVENT_WINDOW_HDR_STATE_CHANGED, so the render path reads the
 * cache instead of doing a property lookup per draw. */
static void Aeron_RefreshOutputHdrProperties(void) {
	SDL_PropertiesID props;

	g_aeron.hdr_headroom        = 1.0f;
	g_aeron.hdr_sdr_white_level = 1.0f;
	if (!g_aeron.window) {
		return;
	}

	props = SDL_GetWindowProperties(g_aeron.window);
	if (!props) {
		return;
	}

	/* Both properties are SDR-relative, so 1.0 is the neutral value a display
	 * without HDR reports and the floor for a nonsensical driver value. */
	g_aeron.hdr_headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
	if (g_aeron.hdr_headroom < 1.0f) {
		g_aeron.hdr_headroom = 1.0f;
	}
	g_aeron.hdr_sdr_white_level = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
	if (g_aeron.hdr_sdr_white_level <= 0.0f) {
		g_aeron.hdr_sdr_white_level = 1.0f;
	}
}

/* Applies the desired mode, downgrading to SDR when HDR is unavailable. Logs
 * every effective transition with the reason so an unavailable HDR display or
 * a backend that refuses the composition is visible in the log. */
static int Aeron_ApplyOutputHdr(void) {
	const int previous = g_aeron.hdr_output_enabled;
	const int wanted   = g_aeron.hdr_output_desired && Aeron_OutputSupportsHdr();

	if (wanted != previous) {
		/* Layer-composition pipelines are keyed on target format; the new
		 * format's pipelines are created lazily on the next present. */
		if (!Aeron_ConfigureSwapchain(wanted)) {
			return 0;
		}
		Aeron_LogInfo("aeron", "HDR output %s (%s, headroom %.2f)", wanted ? "enabled" : "disabled",
				  Aeron_OutputHdrStatusName(Aeron_OutputHdrStatus()), (double)g_aeron.hdr_headroom);
	}
	return 1;
}

int Aeron_SetOutputHdr(int enabled) {
	if (!g_aeron.gpu_device || !g_aeron.window) {
		return 0;
	}
	g_aeron.hdr_output_desired  = enabled != 0;
	g_aeron.hdr_reapply_pending = 0;
	return Aeron_ApplyOutputHdr();
}

void Aeron_OnOutputHdrStateChanged(void) {
	Aeron_RefreshOutputHdrProperties();
	g_aeron.hdr_reapply_pending = 1;
}

void Aeron_ApplyPendingOutputHdr(void) {
	if (!g_aeron.hdr_reapply_pending) {
		return;
	}
	g_aeron.hdr_reapply_pending = 0;
	if (!g_aeron.gpu_device || !g_aeron.window) {
		return;
	}
	/* A failed re-evaluation keeps the previous composition, which stays
	 * presentable; the display simply does not follow its new HDR state.
	 * Aeron_SetRenderError already logged the cause. */
	(void)Aeron_ApplyOutputHdr();
}

int Aeron_OutputHdrEnabled(void) { return g_aeron.hdr_output_enabled; }

AeronHdrOutputStatus Aeron_OutputHdrStatus(void) {
	if (g_aeron.hdr_output_enabled) {
		return AERON_HDR_OUTPUT_ACTIVE;
	}
	if (!g_aeron.hdr_output_desired) {
		return AERON_HDR_OUTPUT_DISABLED;
	}
	/* Headroom above SDR white means the display is in HDR mode, so a refusal
	 * at this point comes from the GPU backend rather than the display. */
	return g_aeron.hdr_headroom > 1.0f ? AERON_HDR_OUTPUT_UNSUPPORTED : AERON_HDR_OUTPUT_DISPLAY_SDR;
}

const char* Aeron_OutputHdrStatusName(AeronHdrOutputStatus status) {
	switch (status) {
		case AERON_HDR_OUTPUT_ACTIVE:
			return "active";
		case AERON_HDR_OUTPUT_DISABLED:
			return "disabled";
		case AERON_HDR_OUTPUT_DISPLAY_SDR:
			return "display not in HDR mode";
		case AERON_HDR_OUTPUT_UNSUPPORTED:
			return "unsupported by the render backend";
	}
	return "unknown";
}

float Aeron_OutputHdrHeadroom(void) {
	float headroom;

	if (g_aeron.swapchain_composition != SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR ||
		g_aeron.hdr_paper_white_nits <= 0.0f) {
		return g_aeron.hdr_headroom;
	}
	/* SDL's headroom is relative to the OS SDR white. A paper-white override
	 * moves the game's reference white while the display peak stays fixed,
	 * so re-express that peak relative to the game's white. */
	headroom = g_aeron.hdr_headroom * g_aeron.hdr_sdr_white_level * 80.0f / g_aeron.hdr_paper_white_nits;
	return headroom < 1.0f ? 1.0f : headroom;
}

float Aeron_OutputSdrWhiteLevel(void) {
	if (g_aeron.swapchain_composition != SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) {
		return 1.0f;
	}
	if (g_aeron.hdr_paper_white_nits > 0.0f) {
		return g_aeron.hdr_paper_white_nits / 80.0f;
	}
	return g_aeron.hdr_sdr_white_level;
}

void Aeron_SetOutputPaperWhiteNits(float nits) {
	if (nits <= 0.0f) {
		g_aeron.hdr_paper_white_nits = 0.0f;
		return;
	}
	g_aeron.hdr_paper_white_nits = SDL_clamp(nits, 80.0f, 1000.0f);
}

float Aeron_OutputPaperWhiteNits(void) { return g_aeron.hdr_paper_white_nits; }

void Aeron_SetOutputSdrContentGamma(float gamma) {
	if (gamma <= 0.0f) {
		g_aeron.hdr_sdr_content_gamma = 0.0f;
		return;
	}
	g_aeron.hdr_sdr_content_gamma = SDL_clamp(gamma, 1.0f, 3.0f);
}

float Aeron_OutputSdrContentGamma(void) { return g_aeron.hdr_sdr_content_gamma; }

/* Effective decode gamma for display-referred layers in the current
 * composition. Only the HDR swapchain sees the decoded linear light directly;
 * SDR compositions round-trip through the sRGB hardware encode and must keep
 * the exact piecewise inverse. */
static float Aeron_OutputSdrDecodeGamma(void) {
	if (g_aeron.swapchain_composition != SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) {
		return 0.0f;
	}
	return g_aeron.hdr_sdr_content_gamma;
}

float Aeron_RenderPassOutputRgbScale(const AeronRenderPass* pass) {
	return pass && pass->output_rgb_scale > 0.0f ? pass->output_rgb_scale : 1.0f;
}

AeronTextureFormat Aeron_SwapchainFormat(void) {
	return Aeron_FromSdlTextureFormat(g_aeron.swapchain_format);
}

static int Aeron_CreatePixelFrameSampler(void) {
	SDL_GPUSamplerCreateInfo sampler_info;

	SDL_zero(sampler_info);
	sampler_info.min_filter     = SDL_GPU_FILTER_LINEAR;
	sampler_info.mag_filter     = SDL_GPU_FILTER_LINEAR;
	sampler_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

	g_aeron.pixel_frame_sampler = SDL_CreateGPUSampler(g_aeron.gpu_device, &sampler_info);
	if (!g_aeron.pixel_frame_sampler) {
		Aeron_LogError("aeron", "SDL_CreateGPUSampler failed: %s", SDL_GetError());
		return 0;
	}
	sampler_info.min_filter             = SDL_GPU_FILTER_NEAREST;
	sampler_info.mag_filter             = SDL_GPU_FILTER_NEAREST;
	g_aeron.pixel_frame_nearest_sampler = SDL_CreateGPUSampler(g_aeron.gpu_device, &sampler_info);
	if (!g_aeron.pixel_frame_nearest_sampler) {
		Aeron_LogError("aeron", "SDL_CreateGPUSampler failed: %s", SDL_GetError());
		SDL_ReleaseGPUSampler(g_aeron.gpu_device, g_aeron.pixel_frame_sampler);
		g_aeron.pixel_frame_sampler = NULL;
		return 0;
	}

	return 1;
}

static int Aeron_CreateFullscreenPipeline(SDL_GPUGraphicsPipeline** out_pipeline,
										  SDL_GPUTextureFormat      target_format,
										  AeronLayerBlendMode       blend_mode) {
	SDL_GPUColorTargetDescription     color_target;
	SDL_GPUGraphicsPipelineCreateInfo pipeline_info;

	if (!out_pipeline) {
		return 0;
	}

	SDL_zero(color_target);
	color_target.format = target_format;
	if (blend_mode == AERON_LAYER_BLEND_ALPHA || blend_mode == AERON_LAYER_BLEND_PREMULTIPLIED) {
		color_target.blend_state.src_color_blendfactor = blend_mode == AERON_LAYER_BLEND_PREMULTIPLIED
															 ? SDL_GPU_BLENDFACTOR_ONE
															 : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
		color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
		color_target.blend_state.enable_blend          = true;
	}

	SDL_zero(pipeline_info);
	pipeline_info.vertex_shader                         = g_aeron.fullscreen_vertex_shader;
	pipeline_info.fragment_shader                       = g_aeron.fullscreen_fragment_shader;
	pipeline_info.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipeline_info.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
	pipeline_info.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
	pipeline_info.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	pipeline_info.rasterizer_state.enable_depth_clip    = true;
	pipeline_info.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
	pipeline_info.target_info.color_target_descriptions = &color_target;
	pipeline_info.target_info.num_color_targets         = 1;

	*out_pipeline = SDL_CreateGPUGraphicsPipeline(g_aeron.gpu_device, &pipeline_info);
	if (!*out_pipeline) {
		Aeron_SetRenderError("SDL_CreateGPUGraphicsPipeline failed for fullscreen target format %d: %s",
							 (int)target_format, SDL_GetError());
		return 0;
	}

	return 1;
}

static SDL_GPUTextureFormat Aeron_TextureFormatForPixelLayer(const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView* view;

	view = &desc->frame;
	if (desc->preserve_encoded_values) {
		return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}
	if (view->color_space == AERON_COLOR_SPACE_LINEAR_SRGB) {
		return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}

	return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
}

static SDL_GPUTextureFormat Aeron_ToSdlTextureFormat(AeronTextureFormat format) {
	switch (format) {
		case AERON_TEXTURE_FORMAT_RGBA8_UNORM:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		case AERON_TEXTURE_FORMAT_RGBA8_SRGB:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		case AERON_TEXTURE_FORMAT_BGRA8_UNORM:
			return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
		case AERON_TEXTURE_FORMAT_BGRA8_SRGB:
			return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		case AERON_TEXTURE_FORMAT_D16_UNORM:
			return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
		case AERON_TEXTURE_FORMAT_D24_UNORM:
			return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
		case AERON_TEXTURE_FORMAT_D32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		case AERON_TEXTURE_FORMAT_R8_UNORM:
			return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
		case AERON_TEXTURE_FORMAT_R8G8_UNORM:
			return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
		case AERON_TEXTURE_FORMAT_R16_SNORM:
			return SDL_GPU_TEXTUREFORMAT_R16_SNORM;
		case AERON_TEXTURE_FORMAT_R16_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
		case AERON_TEXTURE_FORMAT_R32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
		case AERON_TEXTURE_FORMAT_R32_UINT:
			return SDL_GPU_TEXTUREFORMAT_R32_UINT;
		case AERON_TEXTURE_FORMAT_R16G16_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
		case AERON_TEXTURE_FORMAT_R16G16_SNORM:
			return SDL_GPU_TEXTUREFORMAT_R16G16_SNORM;
		case AERON_TEXTURE_FORMAT_R32G32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
		case AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT:
			return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
		case AERON_TEXTURE_FORMAT_RGBA16_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		case AERON_TEXTURE_FORMAT_RGBA32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
		case AERON_TEXTURE_FORMAT_R10G10B10A2_UNORM:
			return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
		case AERON_TEXTURE_FORMAT_BC1_RGBA_UNORM:
			return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
		case AERON_TEXTURE_FORMAT_BC1_RGBA_SRGB:
			return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB;
		case AERON_TEXTURE_FORMAT_BC3_RGBA_UNORM:
			return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
		case AERON_TEXTURE_FORMAT_BC3_RGBA_SRGB:
			return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
		case AERON_TEXTURE_FORMAT_BC4_R_UNORM:
			return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
		case AERON_TEXTURE_FORMAT_BC5_RG_UNORM:
			return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
		case AERON_TEXTURE_FORMAT_BC6H_RGB_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT;
		case AERON_TEXTURE_FORMAT_BC6H_RGB_UFLOAT:
			return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT;
		case AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM:
			return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
		case AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB:
			return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
		default:
			return SDL_GPU_TEXTUREFORMAT_INVALID;
	}
}

/* Reverse mapping for the formats a swapchain can take. */
static AeronTextureFormat Aeron_FromSdlTextureFormat(SDL_GPUTextureFormat format) {
	switch (format) {
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
			return AERON_TEXTURE_FORMAT_RGBA8_UNORM;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			return AERON_TEXTURE_FORMAT_RGBA8_SRGB;
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
			return AERON_TEXTURE_FORMAT_BGRA8_UNORM;
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
			return AERON_TEXTURE_FORMAT_BGRA8_SRGB;
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
			return AERON_TEXTURE_FORMAT_RGBA16_FLOAT;
		case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:
			return AERON_TEXTURE_FORMAT_R10G10B10A2_UNORM;
		default:
			return AERON_TEXTURE_FORMAT_UNKNOWN;
	}
}

static SDL_GPUBufferUsageFlags Aeron_ToSdlBufferUsage(uint32_t usage) {
	SDL_GPUBufferUsageFlags sdl_usage;

	sdl_usage = 0;
	if ((usage & AERON_BUFFER_USAGE_VERTEX) != 0) {
		sdl_usage |= SDL_GPU_BUFFERUSAGE_VERTEX;
	}
	if ((usage & AERON_BUFFER_USAGE_INDEX) != 0) {
		sdl_usage |= SDL_GPU_BUFFERUSAGE_INDEX;
	}
	if ((usage & AERON_BUFFER_USAGE_STORAGE) != 0) {
		sdl_usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	}
	if ((usage & AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ) != 0) {
		sdl_usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	}
	if ((usage & AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE) != 0) {
		sdl_usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	}

	return sdl_usage;
}

static SDL_GPUTextureUsageFlags Aeron_ToSdlTextureUsage(uint32_t usage) {
	SDL_GPUTextureUsageFlags sdl_usage;

	sdl_usage = 0;
	if ((usage & AERON_TEXTURE_USAGE_SAMPLED) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
	}
	if ((usage & AERON_TEXTURE_USAGE_COLOR_TARGET) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	}
	if ((usage & AERON_TEXTURE_USAGE_DEPTH_TARGET) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	}
	if ((usage & AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
	}
	if ((usage & AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
	}
	if ((usage & AERON_TEXTURE_USAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE) != 0) {
		sdl_usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
	}

	return sdl_usage;
}

static SDL_GPUFilter Aeron_ToSdlFilter(AeronFilter filter) {
	if (filter == AERON_FILTER_LINEAR) {
		return SDL_GPU_FILTER_LINEAR;
	}

	return SDL_GPU_FILTER_NEAREST;
}

static SDL_GPUSamplerMipmapMode Aeron_ToSdlMipFilter(AeronFilter filter) {
	if (filter == AERON_FILTER_LINEAR) {
		return SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	}

	return SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
}

static SDL_GPUSamplerAddressMode Aeron_ToSdlAddressMode(AeronAddressMode mode) {
	switch (mode) {
		case AERON_ADDRESS_REPEAT:
			return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		case AERON_ADDRESS_MIRRORED_REPEAT:
			return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
		case AERON_ADDRESS_CLAMP_TO_EDGE:
		default:
			return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	}
}

static SDL_GPUShaderStage Aeron_ToSdlShaderStage(AeronShaderStage stage) {
	if (stage == AERON_SHADER_STAGE_FRAGMENT) {
		return SDL_GPU_SHADERSTAGE_FRAGMENT;
	}

	return SDL_GPU_SHADERSTAGE_VERTEX;
}

static SDL_GPUVertexElementFormat Aeron_ToSdlVertexFormat(AeronVertexFormat format) {
	switch (format) {
		case AERON_VERTEX_FORMAT_FLOAT:
			return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
		case AERON_VERTEX_FORMAT_FLOAT2:
			return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
		case AERON_VERTEX_FORMAT_FLOAT3:
			return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
		case AERON_VERTEX_FORMAT_FLOAT4:
			return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
		case AERON_VERTEX_FORMAT_UBYTE4_NORM:
			return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
		case AERON_VERTEX_FORMAT_SHORT2_NORM:
			return SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM;
		case AERON_VERTEX_FORMAT_SHORT4_NORM:
			return SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM;
		case AERON_VERTEX_FORMAT_UINT:
			return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
		default:
			return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	}
}

static SDL_GPUPrimitiveType Aeron_ToSdlPrimitiveType(AeronPrimitiveType type) {
	switch (type) {
		case AERON_PRIMITIVE_LINES:
			return SDL_GPU_PRIMITIVETYPE_LINELIST;
		case AERON_PRIMITIVE_POINTS:
			return SDL_GPU_PRIMITIVETYPE_POINTLIST;
		case AERON_PRIMITIVE_TRIANGLE_STRIP:
			return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
		case AERON_PRIMITIVE_TRIANGLES:
		default:
			return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	}
}

static int Aeron_IsValidSampleCount(AeronSampleCount sample_count) {
	return sample_count == AERON_SAMPLE_COUNT_1 || sample_count == AERON_SAMPLE_COUNT_2 ||
		   sample_count == AERON_SAMPLE_COUNT_4 || sample_count == AERON_SAMPLE_COUNT_8;
}

static AeronSampleCount Aeron_DescSampleCount(AeronSampleCount sample_count) {
	return sample_count == 0 ? AERON_SAMPLE_COUNT_1 : sample_count;
}

static SDL_GPUSampleCount Aeron_ToSdlSampleCount(AeronSampleCount sample_count) {
	switch (sample_count) {
		case AERON_SAMPLE_COUNT_2:
			return SDL_GPU_SAMPLECOUNT_2;
		case AERON_SAMPLE_COUNT_4:
			return SDL_GPU_SAMPLECOUNT_4;
		case AERON_SAMPLE_COUNT_8:
			return SDL_GPU_SAMPLECOUNT_8;
		case AERON_SAMPLE_COUNT_1:
		default:
			return SDL_GPU_SAMPLECOUNT_1;
	}
}

static SDL_GPUCullMode Aeron_ToSdlCullMode(AeronCullMode mode) {
	switch (mode) {
		case AERON_CULL_FRONT:
			return SDL_GPU_CULLMODE_FRONT;
		case AERON_CULL_BACK:
			return SDL_GPU_CULLMODE_BACK;
		case AERON_CULL_NONE:
		default:
			return SDL_GPU_CULLMODE_NONE;
	}
}

static SDL_GPUCompareOp Aeron_ToSdlCompareOp(AeronCompareOp op) {
	switch (op) {
		case AERON_COMPARE_LESS:
			return SDL_GPU_COMPAREOP_LESS;
		case AERON_COMPARE_LESS_EQUAL:
			return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		case AERON_COMPARE_EQUAL:
			return SDL_GPU_COMPAREOP_EQUAL;
		case AERON_COMPARE_GREATER:
			return SDL_GPU_COMPAREOP_GREATER;
		case AERON_COMPARE_GREATER_EQUAL:
			return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
		case AERON_COMPARE_ALWAYS:
		default:
			return SDL_GPU_COMPAREOP_ALWAYS;
	}
}

static SDL_GPUBlendFactor Aeron_ToSdlBlendFactor(AeronBlendFactor factor) {
	switch (factor) {
		case AERON_BLEND_ZERO:
			return SDL_GPU_BLENDFACTOR_ZERO;
		case AERON_BLEND_ONE:
			return SDL_GPU_BLENDFACTOR_ONE;
		case AERON_BLEND_SRC_COLOR:
			return SDL_GPU_BLENDFACTOR_SRC_COLOR;
		case AERON_BLEND_ONE_MINUS_SRC_COLOR:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		case AERON_BLEND_DST_COLOR:
			return SDL_GPU_BLENDFACTOR_DST_COLOR;
		case AERON_BLEND_ONE_MINUS_DST_COLOR:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
		case AERON_BLEND_SRC_ALPHA:
			return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		case AERON_BLEND_ONE_MINUS_SRC_ALPHA:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		case AERON_BLEND_DST_ALPHA:
			return SDL_GPU_BLENDFACTOR_DST_ALPHA;
		case AERON_BLEND_ONE_MINUS_DST_ALPHA:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
		default:
			return SDL_GPU_BLENDFACTOR_ONE;
	}
}

static SDL_GPUBlendOp Aeron_ToSdlBlendOp(AeronBlendOp op) {
	switch (op) {
		case AERON_BLEND_OP_SUBTRACT:
			return SDL_GPU_BLENDOP_SUBTRACT;
		case AERON_BLEND_OP_REVERSE_SUBTRACT:
			return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		case AERON_BLEND_OP_MIN:
			return SDL_GPU_BLENDOP_MIN;
		case AERON_BLEND_OP_MAX:
			return SDL_GPU_BLENDOP_MAX;
		case AERON_BLEND_OP_ADD:
		default:
			return SDL_GPU_BLENDOP_ADD;
	}
}

static int Aeron_IsDepthTextureFormat(AeronTextureFormat format) {
	return format == AERON_TEXTURE_FORMAT_D16_UNORM || format == AERON_TEXTURE_FORMAT_D24_UNORM ||
		   format == AERON_TEXTURE_FORMAT_D32_FLOAT;
}

static int Aeron_IsCompressedTextureFormat(AeronTextureFormat format) {
	return format >= AERON_TEXTURE_FORMAT_BC1_RGBA_UNORM && format <= AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB;
}

/* Renderable color formats: anything that is not depth, compressed, or unknown.
 * Actual driver support is a separate question (Aeron_TextureFormatSupported). */
static int Aeron_IsColorTextureFormat(AeronTextureFormat format) {
	return format != AERON_TEXTURE_FORMAT_UNKNOWN && !Aeron_IsDepthTextureFormat(format) &&
		   !Aeron_IsCompressedTextureFormat(format);
}

static int Aeron_PixelFormatBytesPerPixel(AeronPixelFormat format) {
	switch (format) {
		case AERON_PIXEL_FORMAT_INDEX8:
			return 1;
		case AERON_PIXEL_FORMAT_RGB555:
		case AERON_PIXEL_FORMAT_RGB565:
			return 2;
		case AERON_PIXEL_FORMAT_XRGB8888:
		case AERON_PIXEL_FORMAT_ARGB8888:
		case AERON_PIXEL_FORMAT_RGBA8888:
		case AERON_PIXEL_FORMAT_BGRA8888:
			return 4;
		default:
			return 0;
	}
}

static SDL_PixelFormat Aeron_SdlPixelFormatForPixelFrame(AeronPixelFormat format) {
	switch (format) {
		case AERON_PIXEL_FORMAT_RGB555:
			return SDL_PIXELFORMAT_XRGB1555;
		case AERON_PIXEL_FORMAT_RGB565:
			return SDL_PIXELFORMAT_RGB565;
		case AERON_PIXEL_FORMAT_XRGB8888:
			return SDL_PIXELFORMAT_XRGB32;
		case AERON_PIXEL_FORMAT_ARGB8888:
			return SDL_PIXELFORMAT_ARGB32;
		case AERON_PIXEL_FORMAT_RGBA8888:
			return SDL_PIXELFORMAT_RGBA32;
		case AERON_PIXEL_FORMAT_BGRA8888:
			return SDL_PIXELFORMAT_BGRA32;
		default:
			return SDL_PIXELFORMAT_UNKNOWN;
	}
}

static SDL_Colorspace Aeron_SdlColorspaceForPixelFrame(AeronColorSpace color_space) {
	if (color_space == AERON_COLOR_SPACE_LINEAR_SRGB) {
		return SDL_COLORSPACE_SRGB_LINEAR;
	}

	return SDL_COLORSPACE_SRGB;
}

static void Aeron_ReleasePixelLayerUpload(AeronPixelLayerUpload* upload) {
	if (!upload) {
		return;
	}

	if (upload->transfer) {
		SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, upload->transfer);
		upload->transfer = NULL;
	}

	if (upload->texture) {
		SDL_ReleaseGPUTexture(g_aeron.gpu_device, upload->texture);
		upload->texture = NULL;
	}

	upload->width                      = 0;
	upload->height                     = 0;
	upload->transfer_size              = 0;
	upload->texture_format             = SDL_GPU_TEXTUREFORMAT_INVALID;
	upload->uploaded_generation        = 0;
	upload->uploaded_pixels            = NULL;
	upload->uploaded_color_key_enabled = 0;
	upload->uploaded_color_key         = 0;
}

static void Aeron_ReleasePixelLayerUploads(void) {
	int i;

	for (i = 0; i < AERON_MAX_PIXEL_LAYER_UPLOADS; ++i) {
		Aeron_ReleasePixelLayerUpload(&g_aeron.pixel_layer_uploads[i]);
	}
	Aeron_ReleasePixelLayerUpload(&g_aeron.composition_pixel_upload);
}

static int Aeron_EnsurePixelLayerUpload(AeronPixelLayerUpload* upload, const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView*      view;
	SDL_GPUTextureCreateInfo        texture_info;
	SDL_GPUTransferBufferCreateInfo transfer_info;
	SDL_GPUTextureFormat            texture_format;
	uint64_t                        transfer_size;

	if (!upload || !desc) {
		return 0;
	}

	view           = &desc->frame;
	texture_format = Aeron_TextureFormatForPixelLayer(desc);
	transfer_size  = (uint64_t)view->width * (uint64_t)view->height * 4u;
	if (transfer_size > UINT32_MAX) {
		return 0;
	}

	if (upload->texture && upload->width == view->width && upload->height == view->height &&
		upload->texture_format == texture_format && upload->transfer_size >= (uint32_t)transfer_size) {
		return 1;
	}

	Aeron_ReleasePixelLayerUpload(upload);

	SDL_zero(texture_info);
	texture_info.type                 = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format               = texture_format;
	texture_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	texture_info.width                = (uint32_t)view->width;
	texture_info.height               = (uint32_t)view->height;
	texture_info.layer_count_or_depth = 1;
	texture_info.num_levels           = 1;
	texture_info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	upload->texture = SDL_CreateGPUTexture(g_aeron.gpu_device, &texture_info);
	if (!upload->texture) {
		Aeron_SetRenderError("SDL_CreateGPUTexture failed for pixel frame: %s", SDL_GetError());
		return 0;
	}
	AeronGpuDebug_NameTexture(g_aeron.gpu_device, upload->texture, "aeron.pixel_layer");

	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size  = (uint32_t)transfer_size;

	upload->transfer = SDL_CreateGPUTransferBuffer(g_aeron.gpu_device, &transfer_info);
	if (!upload->transfer) {
		Aeron_SetRenderError("SDL_CreateGPUTransferBuffer failed for pixel frame: %s", SDL_GetError());
		Aeron_ReleasePixelLayerUpload(upload);
		return 0;
	}

	upload->width          = view->width;
	upload->height         = view->height;
	upload->transfer_size  = (uint32_t)transfer_size;
	upload->texture_format = texture_format;
	return 1;
}

static void Aeron_WriteRgba8(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	dst[0] = r;
	dst[1] = g;
	dst[2] = b;
	dst[3] = a;
}

static int Aeron_ConvertIndexed8PixelFrameToRgba8(uint8_t* dst, const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView* view;
	int                        y;

	view = &desc->frame;
	if (!dst || !view || !view->pixels || !view->palette || view->width <= 0 || view->height <= 0) {
		return 0;
	}

	for (y = 0; y < view->height; ++y) {
		const uint8_t* src_row = (const uint8_t*)view->pixels + (size_t)y * (size_t)view->pitch;
		uint8_t*       dst_row = dst + (size_t)y * (size_t)view->width * 4u;
		int            x;

		for (x = 0; x < view->width; ++x) {
			uint8_t* out = dst_row + (size_t)x * 4u;

			const AeronPaletteEntry* entry = &view->palette[src_row[x]];
			Aeron_WriteRgba8(out, entry->r, entry->g, entry->b,
							 desc->color_key_enabled && src_row[x] == (uint8_t)desc->color_key ? 0
																							   : entry->a);
		}
	}

	return 1;
}

static uint32_t Aeron_ReadRawPixelValue(const uint8_t* src, AeronPixelFormat format) {
	switch (format) {
		case AERON_PIXEL_FORMAT_INDEX8:
			return src[0];
		case AERON_PIXEL_FORMAT_RGB555:
		case AERON_PIXEL_FORMAT_RGB565:
			return (uint32_t)src[0] | ((uint32_t)src[1] << 8);
		case AERON_PIXEL_FORMAT_XRGB8888:
		case AERON_PIXEL_FORMAT_ARGB8888:
		case AERON_PIXEL_FORMAT_RGBA8888:
		case AERON_PIXEL_FORMAT_BGRA8888:
			return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
				   ((uint32_t)src[3] << 24);
		default:
			return 0;
	}
}

static void Aeron_ApplyPixelLayerColorKey(uint8_t* dst, const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView* view;
	int                        bytes_per_pixel;
	int                        y;

	if (!dst || !desc || !desc->color_key_enabled) {
		return;
	}

	view            = &desc->frame;
	bytes_per_pixel = Aeron_PixelFormatBytesPerPixel(view->format);
	if (bytes_per_pixel == 0) {
		return;
	}

	for (y = 0; y < view->height; ++y) {
		const uint8_t* src_row = (const uint8_t*)view->pixels + (size_t)y * (size_t)view->pitch;
		uint8_t*       dst_row = dst + (size_t)y * (size_t)view->width * 4u;
		int            x;

		for (x = 0; x < view->width; ++x) {
			if (Aeron_ReadRawPixelValue(src_row + (size_t)x * (size_t)bytes_per_pixel, view->format) ==
				desc->color_key) {
				dst_row[(size_t)x * 4u + 3u] = 0;
			}
		}
	}
}

static int Aeron_ConvertPixelLayerToRgba8(uint8_t* dst, const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView* view;
	SDL_PixelFormat            src_format;
	SDL_Colorspace             colorspace;
	int                        result;

	if (!dst || !desc) {
		return 0;
	}

	view = &desc->frame;
	if (!view->pixels || view->width <= 0 || view->height <= 0) {
		return 0;
	}

	if (view->format == AERON_PIXEL_FORMAT_INDEX8) {
		return Aeron_ConvertIndexed8PixelFrameToRgba8(dst, desc);
	}

	if (view->format == AERON_PIXEL_FORMAT_RGBA8888 && !desc->color_key_enabled) {
		const size_t row_bytes = (size_t)view->width * 4u;
		for (int y = 0; y < view->height; ++y) {
			memcpy(dst + (size_t)y * row_bytes,
				   (const uint8_t*)view->pixels + (size_t)y * (size_t)view->pitch, row_bytes);
		}
		return 1;
	}

	src_format = Aeron_SdlPixelFormatForPixelFrame(view->format);
	if (src_format == SDL_PIXELFORMAT_UNKNOWN) {
		return 0;
	}

	colorspace = Aeron_SdlColorspaceForPixelFrame(desc->preserve_encoded_values ? AERON_COLOR_SPACE_SRGB
																				: view->color_space);
	result = SDL_ConvertPixelsAndColorspace(view->width, view->height, src_format, colorspace, 0,
											view->pixels, view->pitch, SDL_PIXELFORMAT_RGBA32, colorspace, 0,
											dst, view->width * 4);
	if (result) {
		Aeron_ApplyPixelLayerColorKey(dst, desc);
	}
	return result;
}

static SDL_PixelFormat Aeron_SdlPixelFormatForTextureUpload(AeronTextureFormat format) {
	switch (format) {
		case AERON_TEXTURE_FORMAT_RGBA8_UNORM:
		case AERON_TEXTURE_FORMAT_RGBA8_SRGB:
			return SDL_PIXELFORMAT_RGBA32;
		case AERON_TEXTURE_FORMAT_BGRA8_UNORM:
		case AERON_TEXTURE_FORMAT_BGRA8_SRGB:
			return SDL_PIXELFORMAT_BGRA32;
		default:
			return SDL_PIXELFORMAT_UNKNOWN;
	}
}

static int Aeron_ConvertIndexed8ToTextureUpload(uint8_t* dst, const AeronTextureUploadDesc* desc,
												SDL_PixelFormat dst_format) {
	uint32_t packed_palette[256];
	int      y;
	int      palette_index;

	if (!dst || !desc || !desc->pixels || !desc->palette || desc->width <= 0 || desc->height <= 0) {
		return 0;
	}

	for (palette_index = 0; palette_index < 256; ++palette_index) {
		const AeronPaletteEntry* entry = &desc->palette[palette_index];
		uint8_t*                 out   = (uint8_t*)&packed_palette[palette_index];

		if (dst_format == SDL_PIXELFORMAT_BGRA32) {
			out[0] = entry->b;
			out[1] = entry->g;
			out[2] = entry->r;
			out[3] = entry->a;
		} else {
			out[0] = entry->r;
			out[1] = entry->g;
			out[2] = entry->b;
			out[3] = entry->a;
		}
	}

	for (y = 0; y < desc->height; ++y) {
		const uint8_t* src_row = (const uint8_t*)desc->pixels + (size_t)y * (size_t)desc->pitch;
		uint32_t*      dst_row = (uint32_t*)(dst + (size_t)y * (size_t)desc->width * 4u);
		int            x;

		for (x = 0; x < desc->width; ++x) {
			dst_row[x] = packed_palette[src_row[x]];
		}
	}

	return 1;
}

static int Aeron_ConvertTextureUploadToGpuPixels(uint8_t* dst, const AeronTextureUploadDesc* desc,
												 SDL_PixelFormat dst_format) {
	SDL_PixelFormat src_format;
	SDL_Colorspace  colorspace;

	if (!dst || !desc || !desc->texture || !desc->pixels || desc->width <= 0 || desc->height <= 0) {
		return 0;
	}

	if (desc->pixel_format == AERON_PIXEL_FORMAT_INDEX8) {
		return Aeron_ConvertIndexed8ToTextureUpload(dst, desc, dst_format);
	}

	src_format = Aeron_SdlPixelFormatForPixelFrame(desc->pixel_format);
	if (src_format == SDL_PIXELFORMAT_UNKNOWN || dst_format == SDL_PIXELFORMAT_UNKNOWN) {
		return 0;
	}

	colorspace = Aeron_SdlColorspaceForPixelFrame(desc->color_space);
	return SDL_ConvertPixelsAndColorspace(desc->width, desc->height, src_format, colorspace, 0, desc->pixels,
										  desc->pitch, dst_format, colorspace, 0, dst, desc->width * 4);
}

static int Aeron_UploadPixelLayer(SDL_GPUCommandBuffer* command_buffer, AeronPixelLayerUpload* upload,
								  const AeronPixelLayerDesc* desc) {
	const AeronPixelFrameView* view;
	void*                      mapped;
	SDL_GPUCopyPass*           copy_pass;
	SDL_GPUTextureTransferInfo source;
	SDL_GPUTextureRegion       destination;

	if (!desc) {
		return 0;
	}

	view = &desc->frame;
	if (!Aeron_EnsurePixelLayerUpload(upload, desc)) {
		return 0;
	}

	if (upload->uploaded_generation == view->generation && upload->uploaded_pixels == view->pixels &&
		upload->uploaded_preserve_encoded_values == desc->preserve_encoded_values &&
		upload->uploaded_color_key_enabled == desc->color_key_enabled &&
		upload->uploaded_color_key == desc->color_key) {
		return 1;
	}

	mapped = SDL_MapGPUTransferBuffer(g_aeron.gpu_device, upload->transfer, true);
	if (!mapped) {
		Aeron_SetRenderError("SDL_MapGPUTransferBuffer failed for pixel frame: %s", SDL_GetError());
		return 0;
	}

	if (!Aeron_ConvertPixelLayerToRgba8((uint8_t*)mapped, desc)) {
		SDL_UnmapGPUTransferBuffer(g_aeron.gpu_device, upload->transfer);
		Aeron_SetRenderError("Unsupported or invalid Aeron pixel frame format");
		return 0;
	}

	SDL_UnmapGPUTransferBuffer(g_aeron.gpu_device, upload->transfer);

	SDL_zero(source);
	source.transfer_buffer = upload->transfer;
	source.pixels_per_row  = (uint32_t)view->width;
	source.rows_per_layer  = (uint32_t)view->height;

	SDL_zero(destination);
	destination.texture = upload->texture;
	destination.w       = (uint32_t)view->width;
	destination.h       = (uint32_t)view->height;
	destination.d       = 1;

	AeronGpuDebug_Push(command_buffer, "Pixel layer upload");
	SDL_ClearError();
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	if (!copy_pass) {
		Aeron_SetRenderError("SDL_BeginGPUCopyPass failed for pixel frame: %s", SDL_GetError());
		AeronGpuDebug_Pop(command_buffer);
		return 0;
	}

	SDL_UploadToGPUTexture(copy_pass, &source, &destination, true);
	SDL_EndGPUCopyPass(copy_pass);
	AeronGpuDebug_Pop(command_buffer);

	upload->uploaded_generation              = view->generation;
	upload->uploaded_pixels                  = view->pixels;
	upload->uploaded_preserve_encoded_values = desc->preserve_encoded_values;
	upload->uploaded_color_key_enabled       = desc->color_key_enabled;
	upload->uploaded_color_key               = desc->color_key;
	return 1;
}

static void Aeron_ProjectLogicalRect(const SDL_Rect* content_rect, const AeronRectI* logical_rect,
									 SDL_Rect* out_rect) {
	if (!content_rect || !logical_rect || !out_rect || g_aeron.logical_width <= 0 ||
		g_aeron.logical_height <= 0) {
		return;
	}

	out_rect->x = content_rect->x + (logical_rect->x * content_rect->w) / g_aeron.logical_width;
	out_rect->y = content_rect->y + (logical_rect->y * content_rect->h) / g_aeron.logical_height;
	out_rect->w = (logical_rect->width * content_rect->w) / g_aeron.logical_width;
	out_rect->h = (logical_rect->height * content_rect->h) / g_aeron.logical_height;
}

static SDL_GPUGraphicsPipeline* Aeron_PipelineForTarget(SDL_GPUTextureFormat target_format,
														AeronLayerBlendMode  blend_mode) {
	int i;

	for (i = 0; i < g_aeron.fullscreen_pipeline_count; ++i) {
		AeronFullscreenPipeline* cached = &g_aeron.fullscreen_pipelines[i];
		if (cached->format == target_format && cached->blend_mode == blend_mode) {
			return cached->pipeline;
		}
	}

	if (g_aeron.fullscreen_pipeline_count >=
		(int)(sizeof(g_aeron.fullscreen_pipelines) / sizeof(g_aeron.fullscreen_pipelines[0]))) {
		return NULL;
	}

	i                                          = g_aeron.fullscreen_pipeline_count++;
	g_aeron.fullscreen_pipelines[i].format     = target_format;
	g_aeron.fullscreen_pipelines[i].blend_mode = blend_mode;
	if (!Aeron_CreateFullscreenPipeline(&g_aeron.fullscreen_pipelines[i].pipeline, target_format,
										blend_mode)) {
		--g_aeron.fullscreen_pipeline_count;
		SDL_zero(g_aeron.fullscreen_pipelines[i]);
		return NULL;
	}

	return g_aeron.fullscreen_pipelines[i].pipeline;
}

static int Aeron_IsSrgbTextureFormat(AeronTextureFormat format) {
	return format == AERON_TEXTURE_FORMAT_RGBA8_SRGB || format == AERON_TEXTURE_FORMAT_BGRA8_SRGB ||
		   format == AERON_TEXTURE_FORMAT_BC1_RGBA_SRGB || format == AERON_TEXTURE_FORMAT_BC3_RGBA_SRGB ||
		   format == AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB;
}

static int Aeron_IsSrgbSdlTextureFormat(SDL_GPUTextureFormat format) {
	return format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
		   format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
}

static int Aeron_IsValidPixelSamplingMode(AeronPixelSamplingMode sampling) {
	return sampling >= AERON_PIXEL_SAMPLING_LINEAR && sampling <= AERON_PIXEL_SAMPLING_SHARP_BILINEAR;
}

static SDL_GPUSampler* Aeron_PixelSampler(AeronPixelSamplingMode sampling) {
	return sampling == AERON_PIXEL_SAMPLING_NEAREST ? g_aeron.pixel_frame_nearest_sampler
													: g_aeron.pixel_frame_sampler;
}

static int Aeron_DrawTexture(SDL_GPUCommandBuffer* command_buffer, SDL_GPURenderPass* render_pass,
							 SDL_GPUTexture* texture, SDL_GPUSampler* sampler,
							 SDL_GPUTextureFormat target_format, int target_w, int target_h,
							 int texture_is_srgb, AeronColorSpace color_space, int sharp_bilinear,
							 float output_rgb_scale, float decode_gamma, const SDL_Rect* dst_rect,
							 AeronLayerBlendMode blend_mode,
							 const float tint_rgba[4], const float bias_rgba[4],
							 const SDL_Rect* scissor_rect) {
	SDL_GPUViewport              viewport;
	SDL_GPUTextureSamplerBinding binding;
	SDL_GPUGraphicsPipeline*     pipeline;
	SDL_Rect                     scissor;
	float                        fragment_uniform[12];

	pipeline = Aeron_PipelineForTarget(target_format, blend_mode);
	if (!command_buffer || !texture || !sampler || !pipeline || !dst_rect || dst_rect->w <= 0 ||
		dst_rect->h <= 0) {
		return 0;
	}

	binding.texture      = texture;
	binding.sampler      = sampler;
	/* params.x decode mode: 1 = shader-decode an sRGB-encoded source; 2 =
	 * display-gamma remap of display-referred content that reaches the shader
	 * already linear (through a hardware-decoding _SRGB view). Mode 2 only
	 * acts under a positive decode gamma, so SDR compositions keep their
	 * exact piecewise round trip without a special case here. */
	fragment_uniform[0] = 0.0f;
	if (color_space == AERON_COLOR_SPACE_SRGB) {
		fragment_uniform[0] = texture_is_srgb ? 2.0f : 1.0f;
	} else if (color_space == AERON_COLOR_SPACE_LINEAR_DISPLAY) {
		fragment_uniform[0] = 2.0f;
	}
	fragment_uniform[1]  = sharp_bilinear ? 1.0f : 0.0f;
	fragment_uniform[2]  = output_rgb_scale > 0.0f ? output_rgb_scale : 1.0f;
	fragment_uniform[3]  = decode_gamma > 0.0f ? decode_gamma : 0.0f;
	fragment_uniform[4]  = tint_rgba ? tint_rgba[0] : 1.0f;
	fragment_uniform[5]  = tint_rgba ? tint_rgba[1] : 1.0f;
	fragment_uniform[6]  = tint_rgba ? tint_rgba[2] : 1.0f;
	fragment_uniform[7]  = tint_rgba ? tint_rgba[3] : 1.0f;
	fragment_uniform[8]  = bias_rgba ? bias_rgba[0] : 0.0f;
	fragment_uniform[9]  = bias_rgba ? bias_rgba[1] : 0.0f;
	fragment_uniform[10] = bias_rgba ? bias_rgba[2] : 0.0f;
	fragment_uniform[11] = 0.0f;

	scissor = *dst_rect;
	if (scissor_rect && !SDL_GetRectIntersection(scissor_rect, dst_rect, &scissor)) {
		return 1; /* fully clipped away */
	}

	/* The GPU scissor must lie within the render target — a dst rect
	 * hanging past an edge (e.g. a cursor layer whose hotspot-centered
	 * sprite crosses the window's left/top border) would otherwise
	 * yield a negative-origin scissor and the draw gets rejected.
	 * Clamp the scissor to the target; the viewport keeps the full
	 * unclamped dst rect so the quad's mapping is unchanged and the
	 * scissor merely crops it. */
	if (target_w > 0 && target_h > 0) {
		SDL_Rect target_rect = { 0, 0, target_w, target_h };
		if (!SDL_GetRectIntersection(&scissor, &target_rect, &scissor)) {
			return 1; /* fully off-target */
		}
	}

	SDL_BindGPUGraphicsPipeline(render_pass, pipeline);
	SDL_BindGPUFragmentSamplers(render_pass, 0, &binding, 1);
	SDL_PushGPUFragmentUniformData(command_buffer, 0, fragment_uniform, sizeof(fragment_uniform));
	Aeron_RecordUniformPush(sizeof(fragment_uniform));
	viewport.x         = (float)dst_rect->x;
	viewport.y         = (float)dst_rect->y;
	viewport.w         = (float)dst_rect->w;
	viewport.h         = (float)dst_rect->h;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(render_pass, &viewport);
	SDL_SetGPUScissor(render_pass, &scissor);
	SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
	return 1;
}

/* Debug builds request SDL_GPU debug mode: backends only honor
 * SDL_SetGPUTexture/BufferName under it, and it enables backend validation.
 * Keyed to the same gate as the AeronGpuDebug_* label helpers. */
static SDL_GPUDevice* Aeron_CreateGPUDeviceWithOptionalFp16(SDL_GPUShaderFormat shader_formats) {
#if defined(AERON_HAS_VULKAN_FP16_DEVICE_OPTIONS)
	VkPhysicalDeviceShaderFloat16Int8Features float16_features;
	SDL_GPUVulkanOptions                      vulkan_options;
	const char*                               device_extensions[] = {
		VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
	};
	SDL_PropertiesID properties = SDL_CreateProperties();
	SDL_GPUDevice*   device     = NULL;

	memset(&float16_features, 0, sizeof(float16_features));
	float16_features.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
	float16_features.shaderFloat16 = VK_TRUE;
	memset(&vulkan_options, 0, sizeof(vulkan_options));
	vulkan_options.vulkan_api_version     = VK_API_VERSION_1_1;
	vulkan_options.feature_list           = &float16_features;
	vulkan_options.device_extension_count = SDL_arraysize(device_extensions);
	vulkan_options.device_extension_names = device_extensions;

	if (properties) {
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
							   AERON_GPU_DEBUG_LABELS != 0);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,
							   (shader_formats & SDL_GPU_SHADERFORMAT_MSL) != 0);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
							   (shader_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN,
							   (shader_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0);
		SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
							   &vulkan_options);
		device = SDL_CreateGPUDeviceWithProperties(properties);
		SDL_DestroyProperties(properties);
	}
	if (device) {
		return device;
	}
	Aeron_LogWarn("aeron", "GPU creation with optional Vulkan FP16 failed: %s; retrying baseline device",
			  SDL_GetError());
#endif
	return SDL_CreateGPUDevice(shader_formats, AERON_GPU_DEBUG_LABELS != 0, NULL);
}

int Aeron_RenderBackendInit(void) {
	const SDL_GPUShaderFormat shader_formats = Aeron_CompiledShaderFormats();

	/* Classic/frontend art targets ~2.2-power SDR displays, so decoding it
	 * with the piecewise sRGB curve for HDR composition lifts its darks.
	 * Apple's compositor presents SDR content with the piecewise curve
	 * itself, so matching it there keeps SDR and HDR output identical. */
#if defined(SDL_PLATFORM_APPLE)
	g_aeron.hdr_sdr_content_gamma = 0.0f;
#else
	g_aeron.hdr_sdr_content_gamma = 2.2f;
#endif

	g_aeron.gpu_device = Aeron_CreateGPUDeviceWithOptionalFp16(shader_formats);
	if (!g_aeron.gpu_device) {
		Aeron_LogError("aeron", "SDL_CreateGPUDevice failed: %s", SDL_GetError());
		return 0;
	}

	/* Keep FIFO presentation from queueing an additional completed frame.
	 * Aeron samples input before rendering, so a deeper queue directly adds
	 * input-to-display latency. */
	if (!SDL_SetGPUAllowedFramesInFlight(g_aeron.gpu_device, 1)) {
		Aeron_LogError("aeron", "SDL_SetGPUAllowedFramesInFlight failed: %s", SDL_GetError());
		SDL_DestroyGPUDevice(g_aeron.gpu_device);
		g_aeron.gpu_device = NULL;
		return 0;
	}

	if (!SDL_ClaimWindowForGPUDevice(g_aeron.gpu_device, g_aeron.window)) {
		Aeron_LogError("aeron", "SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
		SDL_DestroyGPUDevice(g_aeron.gpu_device);
		g_aeron.gpu_device = NULL;
		return 0;
	}

	/* Seed the HDR cache before the first swapchain so the SDR white level and
	 * headroom are valid from frame zero; events keep it current afterwards. */
	Aeron_RefreshOutputHdrProperties();

	if (!Aeron_ConfigureSwapchain(0)) {
		SDL_ReleaseWindowFromGPUDevice(g_aeron.gpu_device, g_aeron.window);
		SDL_DestroyGPUDevice(g_aeron.gpu_device);
		g_aeron.gpu_device = NULL;
		return 0;
	}

	if (!Aeron_LoadBuiltinShaders()) {
		Aeron_RenderBackendShutdown();
		return 0;
	}

	if (!Aeron_CreatePixelFrameSampler() ||
		!Aeron_PipelineForTarget(g_aeron.swapchain_format, AERON_LAYER_BLEND_OPAQUE) ||
		!Aeron_PipelineForTarget(g_aeron.swapchain_format, AERON_LAYER_BLEND_ALPHA)) {
		Aeron_RenderBackendShutdown();
		return 0;
	}

	return 1;
}

void Aeron_RenderBackendShutdown(void) {
	if (g_aeron.gpu_device) {
		Aeron_ReleasePixelLayerUploads();

		for (int i = 0; i < g_aeron.fullscreen_pipeline_count; ++i) {
			if (g_aeron.fullscreen_pipelines[i].pipeline) {
				SDL_ReleaseGPUGraphicsPipeline(g_aeron.gpu_device, g_aeron.fullscreen_pipelines[i].pipeline);
				g_aeron.fullscreen_pipelines[i].pipeline = NULL;
			}
		}
		SDL_zeroa(g_aeron.fullscreen_pipelines);
		g_aeron.fullscreen_pipeline_count = 0;

		if (g_aeron.pixel_frame_sampler) {
			SDL_ReleaseGPUSampler(g_aeron.gpu_device, g_aeron.pixel_frame_sampler);
			g_aeron.pixel_frame_sampler = NULL;
		}
		if (g_aeron.pixel_frame_nearest_sampler) {
			SDL_ReleaseGPUSampler(g_aeron.gpu_device, g_aeron.pixel_frame_nearest_sampler);
			g_aeron.pixel_frame_nearest_sampler = NULL;
		}

		if (g_aeron.fullscreen_fragment_shader) {
			SDL_ReleaseGPUShader(g_aeron.gpu_device, g_aeron.fullscreen_fragment_shader);
			g_aeron.fullscreen_fragment_shader = NULL;
		}

		if (g_aeron.fullscreen_vertex_shader) {
			SDL_ReleaseGPUShader(g_aeron.gpu_device, g_aeron.fullscreen_vertex_shader);
			g_aeron.fullscreen_vertex_shader = NULL;
		}

		if (g_aeron.window) {
			SDL_ReleaseWindowFromGPUDevice(g_aeron.gpu_device, g_aeron.window);
		}

		SDL_DestroyGPUDevice(g_aeron.gpu_device);
		g_aeron.gpu_device = NULL;
	}
}

int Aeron_Present(void) {
	SDL_GPUCommandBuffer*  command_buffer;
	SDL_GPUTexture*        swapchain_texture;
	SDL_GPURenderPass*     render_pass;
	SDL_GPUColorTargetInfo color_target;
	SDL_Rect               content_rect;
	Uint32                 swapchain_width;
	Uint32                 swapchain_height;
	int                    i;
	int                    present_ok;

	if (!g_aeron.initialized || !g_aeron.gpu_device) {
		Aeron_SetRenderError("Aeron_Present called without an initialized GPU device");
		return 0;
	}

	/* Nothing was submitted this host frame: keep the previously presented
	 * swapchain image on screen instead of acquiring and clearing a fresh one.
	 * This is the keep-last-frame-alive path for ticks that composed no new frame
	 * (app paused / unfocused, quit tick), which the shim present model relies on
	 * in place of a port-side per-tick re-submit. A visible debug overlay still
	 * forces a present so its windows keep rendering over an idle frame. */
	if (g_aeron.render_layer_count == 0 && !Aeron_DebugUiVisible()) {
		return 1;
	}

	/* Build the debug overlay's ImGui frame (menu bar + registered tool
	 * windows). Must precede the command-buffer work below: the draw-data
	 * upload runs a copy pass before the swapchain render pass opens. */
	Aeron_DebugUiBuildFrame();

	SDL_ClearError();
	command_buffer = SDL_AcquireGPUCommandBuffer(g_aeron.gpu_device);
	if (!command_buffer) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_AcquireGPUCommandBuffer failed for presentation: %s",
							 error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}
	AeronGpuDebug_Push(command_buffer, "Aeron frame");
	present_ok = 1;

	for (i = 0; i < g_aeron.render_layer_count; ++i) {
		AeronRenderLayer* layer = &g_aeron.render_layers[i];

		if (layer->kind != AERON_RENDER_LAYER_PIXEL) {
			continue;
		}

		if (layer->pixel_upload_index < 0 || layer->pixel_upload_index >= AERON_MAX_PIXEL_LAYER_UPLOADS ||
			!Aeron_UploadPixelLayer(command_buffer, &g_aeron.pixel_layer_uploads[layer->pixel_upload_index],
									&layer->u.pixel)) {
			Aeron_SetRenderError("Failed to upload pixel layer %d for presentation", i);
			present_ok = 0;
			break;
		}
	}
	if (!present_ok) {
		AeronGpuDebug_Pop(command_buffer);
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}

	Aeron_DebugUiPrepareRender(command_buffer);

	swapchain_texture = NULL;
	swapchain_width   = 0;
	swapchain_height  = 0;
	SDL_ClearError();
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, g_aeron.window, &swapchain_texture,
											   &swapchain_width, &swapchain_height)) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s",
							 error && error[0] ? error : "<no SDL error provided>");
		AeronGpuDebug_Pop(command_buffer);
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}

	if (swapchain_texture) {
		AeronCommandBuffer borrowed_command_buffer;
		AeronRenderPass    borrowed_render_pass;
		AeronRenderTarget  borrowed_render_target;
		const float        output_rgb_scale = Aeron_OutputSdrWhiteLevel();
		const float        sdr_decode_gamma = Aeron_OutputSdrDecodeGamma();

		Aeron_ComputePresentationRect((int)swapchain_width, (int)swapchain_height, &content_rect);

		SDL_zero(color_target);
		color_target.texture = swapchain_texture;
		color_target.clear_color =
			(SDL_FColor) { g_aeron.clear_color_rgba[0] * output_rgb_scale,
						   g_aeron.clear_color_rgba[1] * output_rgb_scale,
						   g_aeron.clear_color_rgba[2] * output_rgb_scale, g_aeron.clear_color_rgba[3] };
		color_target.load_op  = SDL_GPU_LOADOP_CLEAR;
		color_target.store_op = SDL_GPU_STOREOP_STORE;
		color_target.cycle    = false;

		AeronGpuDebug_Push(command_buffer, "Swapchain composition");
		SDL_ClearError();
		render_pass = SDL_BeginGPURenderPass(command_buffer, &color_target, 1, NULL);
		if (!render_pass) {
			const char* error = SDL_GetError();
			Aeron_SetRenderError("SDL_BeginGPURenderPass failed for presentation: %s",
								 error && error[0] ? error : "<no SDL error provided>");
			AeronGpuDebug_Pop(command_buffer);
			AeronGpuDebug_Pop(command_buffer);
			SDL_CancelGPUCommandBuffer(command_buffer);
			return 0;
		}

		SDL_zero(borrowed_command_buffer);
		borrowed_command_buffer.command_buffer = command_buffer;
		borrowed_command_buffer.render_pass_active = 1;
		SDL_zero(borrowed_render_pass);
		borrowed_render_pass.command_buffer   = command_buffer;
		borrowed_render_pass.render_pass      = render_pass;
		borrowed_render_pass.owner            = &borrowed_command_buffer;
		borrowed_render_pass.output_rgb_scale = output_rgb_scale;
		SDL_zero(borrowed_render_target);
		borrowed_render_target.color.texture   = swapchain_texture;
		borrowed_render_target.color.width     = (int)swapchain_width;
		borrowed_render_target.color.height    = (int)swapchain_height;
		borrowed_render_target.color.mip_count = 1;
		borrowed_render_target.color.format    = Aeron_FromSdlTextureFormat(g_aeron.swapchain_format);
		borrowed_render_target.color.usage     = AERON_TEXTURE_USAGE_COLOR_TARGET;

		for (i = 0; i < g_aeron.render_layer_count; ++i) {
			AeronRenderLayer* layer = &g_aeron.render_layers[i];
			SDL_Rect          dst_rect;
			SDL_Rect          scissor_rect;
			const SDL_Rect*   scissor;

			if (layer->kind == AERON_RENDER_LAYER_PIXEL) {
				AeronPixelLayerUpload* upload = &g_aeron.pixel_layer_uploads[layer->pixel_upload_index];
				AeronGpuDebug_Marker(command_buffer, "Pixel layer");

				Aeron_ProjectLogicalRect(&content_rect, &layer->u.pixel.logical_rect, &dst_rect);
				scissor = NULL;
				if (layer->u.pixel.scissor.width > 0 && layer->u.pixel.scissor.height > 0) {
					Aeron_ProjectLogicalRect(&content_rect, &layer->u.pixel.scissor, &scissor_rect);
					scissor = &scissor_rect;
				}
				if (!Aeron_DrawTexture(
						command_buffer, render_pass, upload->texture, Aeron_PixelSampler(layer->u.pixel.sampling),
						g_aeron.swapchain_format, (int)swapchain_width, (int)swapchain_height,
						Aeron_IsSrgbSdlTextureFormat(upload->texture_format),
						layer->u.pixel.preserve_encoded_values ? AERON_COLOR_SPACE_LINEAR_SRGB
															   : layer->u.pixel.frame.color_space,
						layer->u.pixel.sampling == AERON_PIXEL_SAMPLING_SHARP_BILINEAR, output_rgb_scale,
						sdr_decode_gamma, &dst_rect, layer->u.pixel.blend_mode,
						layer->u.pixel.tint_enabled ? layer->u.pixel.tint_rgba : NULL, NULL, scissor)) {
					Aeron_SetRenderError("Failed to record pixel layer %d for presentation", i);
					present_ok = 0;
					break;
				}
			} else if (layer->kind == AERON_RENDER_LAYER_TEXTURE) {
				AeronGpuDebug_Marker(command_buffer, "Texture layer");
				Aeron_ProjectLogicalRect(&content_rect, &layer->u.texture.logical_rect, &dst_rect);
				scissor = NULL;
				if (layer->u.texture.scissor.width > 0 && layer->u.texture.scissor.height > 0) {
					Aeron_ProjectLogicalRect(&content_rect, &layer->u.texture.scissor, &scissor_rect);
					scissor = &scissor_rect;
				}
				if (!Aeron_DrawTexture(
						command_buffer, render_pass, layer->u.texture.texture->texture,
						g_aeron.pixel_frame_sampler, g_aeron.swapchain_format, (int)swapchain_width,
						(int)swapchain_height, Aeron_IsSrgbTextureFormat(layer->u.texture.texture->format),
						layer->u.texture.color_space, 1, output_rgb_scale, sdr_decode_gamma, &dst_rect,
						layer->u.texture.blend_mode,
						layer->u.texture.tint_enabled ? layer->u.texture.tint_rgba : NULL,
						layer->u.texture.tint_enabled ? layer->u.texture.bias_rgba : NULL, scissor)) {
					Aeron_SetRenderError("Failed to record texture layer %d for presentation", i);
					present_ok = 0;
					break;
				}
			} else if (layer->kind == AERON_RENDER_LAYER_SWAPCHAIN_RENDER) {
				const AeronSwapchainRenderLayerDesc* direct = &layer->u.swapchain_render;
				const int full_target                       = content_rect.x == 0 && content_rect.y == 0 &&
															  content_rect.w == (int)swapchain_width &&
															  content_rect.h == (int)swapchain_height;
				if (!direct->callback) {
					continue;
				}
				if (!full_target || direct->required_width != (int)swapchain_width ||
					direct->required_height != (int)swapchain_height) {
					Aeron_LogWarn("aeron", "skipping direct swapchain layer: required %dx%d, acquired %ux%u",
							  direct->required_width, direct->required_height, swapchain_width,
							  swapchain_height);
					continue;
				}
				if (direct->debug_label) {
					AeronGpuDebug_Push(command_buffer, direct->debug_label);
				}
				direct->callback(&borrowed_command_buffer, &borrowed_render_pass, &borrowed_render_target,
								 (int)swapchain_width, (int)swapchain_height, direct->userdata);
				if (direct->debug_label) {
					AeronGpuDebug_Pop(command_buffer);
				}
				if (borrowed_command_buffer.failed) {
					if (borrowed_command_buffer.failure_message[0]) {
						Aeron_SetRenderError("%s", borrowed_command_buffer.failure_message);
					}
					present_ok = 0;
					break;
				}
			}
		}
		/* Debug overlay draws last — over every composed layer. */
		if (Aeron_DebugUiVisible()) {
			AeronGpuDebug_Marker(command_buffer, "Debug UI");
		}
		Aeron_DebugUiRecordDraws(command_buffer, render_pass);
		SDL_EndGPURenderPass(render_pass);
		AeronGpuDebug_Pop(command_buffer);
	}
	AeronGpuDebug_Pop(command_buffer);

	if (!present_ok) {
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}
	SDL_ClearError();
	if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_SubmitGPUCommandBuffer failed for presentation: %s",
							 error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}
	return 1;
}

int Aeron_WaitForPresentationSlot(void) {
	if (!g_aeron.gpu_device || !g_aeron.window) {
		Aeron_SetRenderError("Cannot wait for presentation without a GPU device and window");
		return 0;
	}

	SDL_ClearError();
	if (!SDL_WaitForGPUSwapchain(g_aeron.gpu_device, g_aeron.window)) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_WaitForGPUSwapchain failed: %s",
							 error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}
	return 1;
}

AeronRenderSubmission Aeron_SubmitPixelLayer(const AeronPixelLayerDesc* desc) {
	AeronRenderLayer* layer;
	int               bytes_per_pixel;
	int               layer_index;

	if (!desc || !desc->frame.pixels || desc->frame.width <= 0 || desc->frame.height <= 0 ||
		desc->frame.pitch <= 0 || desc->frame.format == AERON_PIXEL_FORMAT_UNKNOWN ||
		desc->logical_rect.width <= 0 || desc->logical_rect.height <= 0 ||
		!Aeron_IsValidPixelSamplingMode(desc->sampling)) {
		Aeron_SetRenderError("Invalid pixel-layer submission");
		return 0;
	}

	bytes_per_pixel = Aeron_PixelFormatBytesPerPixel(desc->frame.format);
	if (!bytes_per_pixel || desc->frame.pitch < desc->frame.width * bytes_per_pixel) {
		Aeron_SetRenderError("Invalid pixel-layer pitch or pixel format");
		return 0;
	}

	if (g_aeron.render_layer_count >= AERON_MAX_RENDER_LAYERS ||
		g_aeron.pixel_layer_upload_count >= AERON_MAX_PIXEL_LAYER_UPLOADS) {
		Aeron_SetRenderError("Pixel-layer submission capacity exceeded");
		return 0;
	}

	layer_index               = g_aeron.render_layer_count++;
	layer                     = &g_aeron.render_layers[layer_index];
	layer->kind               = AERON_RENDER_LAYER_PIXEL;
	layer->pixel_upload_index = g_aeron.pixel_layer_upload_count++;
	layer->u.pixel            = *desc;
	return ((uint32_t)g_aeron.render_submission_generation << 16) | (uint32_t)(layer_index + 1);
}

AeronRenderSubmission Aeron_SubmitTextureLayer(const AeronTextureLayerDesc* desc) {
	AeronRenderLayer* layer;
	int               layer_index;

	if (!desc || !desc->texture || !desc->texture->texture || desc->logical_rect.width <= 0 ||
		desc->logical_rect.height <= 0) {
		Aeron_SetRenderError("Invalid texture-layer submission");
		return 0;
	}

	if (g_aeron.render_layer_count >= AERON_MAX_RENDER_LAYERS) {
		Aeron_SetRenderError("Texture-layer submission capacity exceeded");
		return 0;
	}

	layer_index               = g_aeron.render_layer_count++;
	layer                     = &g_aeron.render_layers[layer_index];
	layer->kind               = AERON_RENDER_LAYER_TEXTURE;
	layer->pixel_upload_index = -1;
	layer->u.texture          = *desc;
	return ((uint32_t)g_aeron.render_submission_generation << 16) | (uint32_t)(layer_index + 1);
}

void Aeron_CancelRenderSubmission(AeronRenderSubmission submission) {
	const uint16_t generation   = (uint16_t)(submission >> 16);
	const uint16_t packed_index = (uint16_t)(submission & 0xffffu);
	int            layer_index;

	if (submission == 0 || generation != g_aeron.render_submission_generation || packed_index == 0) {
		return;
	}

	layer_index = (int)packed_index - 1;
	if (layer_index < 0 || layer_index >= g_aeron.render_layer_count) {
		return;
	}

	g_aeron.render_layers[layer_index].kind = AERON_RENDER_LAYER_NONE;
}

int Aeron_SubmitSwapchainRenderLayer(const AeronSwapchainRenderLayerDesc* desc) {
	AeronRenderLayer* layer;

	if (!desc || !desc->callback || desc->required_width <= 0 || desc->required_height <= 0 ||
		g_aeron.render_layer_count >= AERON_MAX_RENDER_LAYERS) {
		Aeron_SetRenderError("Invalid or overflowing direct swapchain-layer submission");
		return 0;
	}

	layer                     = &g_aeron.render_layers[g_aeron.render_layer_count++];
	layer->kind               = AERON_RENDER_LAYER_SWAPCHAIN_RENDER;
	layer->pixel_upload_index = -1;
	layer->u.swapchain_render = *desc;
	return 1;
}

int Aeron_CanRenderDirectToSwapchain(int target_width, int target_height) {
	SDL_Rect content_rect;
	int      drawable_width;
	int      drawable_height;

	if (!g_aeron.gpu_device || !g_aeron.window || target_width <= 0 || target_height <= 0 ||
		g_aeron.swapchain_format == SDL_GPU_TEXTUREFORMAT_INVALID ||
		Aeron_FromSdlTextureFormat(g_aeron.swapchain_format) == AERON_TEXTURE_FORMAT_UNKNOWN ||
		!SDL_GetWindowSizeInPixels(g_aeron.window, &drawable_width, &drawable_height)) {
		return 0;
	}
	Aeron_ComputePresentationRect(drawable_width, drawable_height, &content_rect);
	return content_rect.x == 0 && content_rect.y == 0 && content_rect.w == drawable_width &&
		   content_rect.h == drawable_height && target_width == drawable_width &&
		   target_height == drawable_height;
}

int Aeron_ComposePixelLayerToRenderTarget(AeronRenderTarget* target, const AeronPixelLayerDesc* desc,
										  int clear_color, const float clear_color_rgba[4]) {
	SDL_GPUCommandBuffer*  command_buffer;
	SDL_GPUColorTargetInfo color_target;
	SDL_GPURenderPass*     render_pass;
	SDL_GPUTextureFormat   target_format;
	SDL_Rect               dst_rect;

	if (!g_aeron.gpu_device || !target || !target->color.texture || !desc || !desc->frame.pixels ||
		desc->frame.width <= 0 || desc->frame.height <= 0 || desc->logical_rect.width <= 0 ||
		desc->logical_rect.height <= 0 || !Aeron_IsValidPixelSamplingMode(desc->sampling)) {
		Aeron_SetRenderError("Invalid pixel-layer render-target composition description");
		return 0;
	}
	target_format = Aeron_ToSdlTextureFormat(target->color.format);
	if (!Aeron_PipelineForTarget(target_format, desc->blend_mode)) {
		Aeron_SetRenderError("Could not create the pixel-layer composition pipeline");
		return 0;
	}

	SDL_ClearError();
	command_buffer = SDL_AcquireGPUCommandBuffer(g_aeron.gpu_device);
	if (!command_buffer) {
		Aeron_SetRenderError("SDL_AcquireGPUCommandBuffer failed for render-target composition: %s",
							 SDL_GetError());
		return 0;
	}
	AeronGpuDebug_Push(command_buffer, "Pixel layer composition");

	if (!Aeron_UploadPixelLayer(command_buffer, &g_aeron.composition_pixel_upload, desc)) {
		Aeron_SetRenderError("Pixel upload failed during render-target composition: %s", SDL_GetError());
		AeronGpuDebug_Pop(command_buffer);
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}

	SDL_zero(color_target);
	color_target.texture = target->color.texture;
	if (clear_color_rgba) {
		color_target.clear_color = (SDL_FColor) { clear_color_rgba[0], clear_color_rgba[1],
												  clear_color_rgba[2], clear_color_rgba[3] };
	} else {
		color_target.clear_color = (SDL_FColor) { 0.0f, 0.0f, 0.0f, 0.0f };
	}
	color_target.load_op  = clear_color ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	color_target.store_op = SDL_GPU_STOREOP_STORE;
	color_target.cycle    = clear_color ? true : false;

	SDL_ClearError();
	render_pass = SDL_BeginGPURenderPass(command_buffer, &color_target, 1, NULL);
	if (!render_pass) {
		Aeron_SetRenderError("SDL_BeginGPURenderPass failed for render-target composition: %s",
							 SDL_GetError());
		AeronGpuDebug_Pop(command_buffer);
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}

	dst_rect.x = desc->logical_rect.x;
	dst_rect.y = desc->logical_rect.y;
	dst_rect.w = desc->logical_rect.width;
	dst_rect.h = desc->logical_rect.height;
	if (!Aeron_DrawTexture(
			command_buffer, render_pass, g_aeron.composition_pixel_upload.texture,
			Aeron_PixelSampler(desc->sampling), target_format, target->color.width, target->color.height,
			Aeron_IsSrgbSdlTextureFormat(g_aeron.composition_pixel_upload.texture_format),
			desc->preserve_encoded_values ? AERON_COLOR_SPACE_LINEAR_SRGB : desc->frame.color_space,
			/* Offscreen target: the piecewise round trip must stay exact, so no
			 * display-gamma decode here — it applies once, at presentation. */
			desc->sampling == AERON_PIXEL_SAMPLING_SHARP_BILINEAR, 1.0f, 0.0f, &dst_rect, desc->blend_mode,
			desc->tint_enabled ? desc->tint_rgba : NULL, NULL, NULL)) {
		Aeron_SetRenderError("Pixel-layer draw setup failed during render-target composition");
		SDL_EndGPURenderPass(render_pass);
		AeronGpuDebug_Pop(command_buffer);
		SDL_CancelGPUCommandBuffer(command_buffer);
		return 0;
	}
	SDL_EndGPURenderPass(render_pass);
	AeronGpuDebug_Pop(command_buffer);

	SDL_ClearError();
	if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
		Aeron_SetRenderError("SDL_SubmitGPUCommandBuffer failed for render-target composition: %s",
							 SDL_GetError());
		return 0;
	}

	return 1;
}

AeronCommandBuffer* Aeron_AcquireCommandBuffer(void) {
	AeronCommandBuffer* command_buffer;

	if (!g_aeron.gpu_device) {
		return NULL;
	}

	command_buffer = (AeronCommandBuffer*)SDL_calloc(1, sizeof(*command_buffer));
	if (!command_buffer) {
		Aeron_SetRenderError("Could not allocate the Aeron command-buffer wrapper");
		return NULL;
	}

	SDL_ClearError();
	command_buffer->command_buffer = SDL_AcquireGPUCommandBuffer(g_aeron.gpu_device);
	if (!command_buffer->command_buffer) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_AcquireGPUCommandBuffer failed: %s",
							 error && error[0] ? error : "<no SDL error provided>");
		SDL_free(command_buffer);
		return NULL;
	}
	command_buffer->next_upload_chunk_size = AERON_UPLOAD_CHUNK_MIN_BYTES;
	command_buffer->owns_wrapper          = 1;

	return command_buffer;
}

const char* Aeron_RenderDriverName(void) {
	const char* driver = g_aeron.gpu_device ? SDL_GetGPUDeviceDriver(g_aeron.gpu_device) : NULL;
	return driver ? driver : "unavailable";
}

void Aeron_GpuDebugPush(AeronCommandBuffer* command_buffer, const char* name) {
	AeronGpuDebug_Push(command_buffer ? command_buffer->command_buffer : NULL, name);
}

void Aeron_GpuDebugPop(AeronCommandBuffer* command_buffer) {
	AeronGpuDebug_Pop(command_buffer ? command_buffer->command_buffer : NULL);
}

void Aeron_GpuDebugMarker(AeronCommandBuffer* command_buffer, const char* name) {
	AeronGpuDebug_Marker(command_buffer ? command_buffer->command_buffer : NULL, name);
}

void Aeron_GpuDebugNameTexture(AeronTexture* texture, const char* name) {
	AeronGpuDebug_NameTexture(g_aeron.gpu_device, texture ? texture->texture : NULL, name);
}

void Aeron_GpuDebugNameBuffer(AeronBuffer* buffer, const char* name) {
	AeronGpuDebug_NameBuffer(g_aeron.gpu_device, buffer ? buffer->buffer : NULL, name);
}

typedef struct AeronUploadSlice {
	AeronUploadChunk* chunk;
	uint32_t          offset;
	uint32_t          size;
	uint8_t*          mapped;
} AeronUploadSlice;

static void Aeron_CommandBufferFail(AeronCommandBuffer* command_buffer, const char* fmt, ...) {
	va_list args;
	char    message[512];

	if (command_buffer && command_buffer->failed) {
		return;
	}
	va_start(args, fmt);
	SDL_vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	Aeron_SetRenderError("%s", message);
	if (command_buffer) {
		Aeron_CopyString(command_buffer->failure_message, sizeof(command_buffer->failure_message), message);
		command_buffer->failed = 1;
		g_aeron.render_data_stats.failed_command_buffer_count++;
	}
}

void Aeron_CommandBufferSetFailure(AeronCommandBuffer* command_buffer, const char* message) {
	Aeron_CommandBufferFail(command_buffer, "%s",
							message && message[0] ? message : "GPU command-buffer preparation failed");
}

static void Aeron_CommandBufferMarkFailed(AeronCommandBuffer* command_buffer) {
	Aeron_CommandBufferFail(command_buffer, "GPU command buffer recording failed");
}

static void Aeron_ReleaseUploadChunks(AeronCommandBuffer* command_buffer) {
	uint32_t i;

	if (!command_buffer) {
		return;
	}
	for (i = 0; i < command_buffer->upload_chunk_count; ++i) {
		if (command_buffer->upload_chunks[i].transfer) {
			SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, command_buffer->upload_chunks[i].transfer);
		}
	}
	SDL_free(command_buffer->upload_chunks);
	command_buffer->upload_chunks         = NULL;
	command_buffer->upload_chunk_count    = 0;
	command_buffer->upload_chunk_capacity = 0;
	SDL_free(command_buffer->upload_cycle_states);
	command_buffer->upload_cycle_states         = NULL;
	command_buffer->upload_cycle_state_count    = 0;
	command_buffer->upload_cycle_state_capacity = 0;
}

static int Aeron_CommandBufferResolveCycle(AeronCommandBuffer* command_buffer, const void* resource,
										  uint8_t kind, int requested, int* out_cycle) {
	AeronUploadCycleState* states;
	uint32_t               i;
	uint32_t               new_capacity;

	if (!command_buffer || !resource || !out_cycle) {
		return 0;
	}
	for (i = 0; i < command_buffer->upload_cycle_state_count; ++i) {
		AeronUploadCycleState* state = &command_buffer->upload_cycle_states[i];
		if (state->resource != resource || state->kind != kind) {
			continue;
		}
		if (requested && !state->cycled) {
			Aeron_LogError("aeron", "Upload destination was cycled after an earlier non-cycling write");
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		*out_cycle = 0;
		return 1;
	}
	if (command_buffer->upload_cycle_state_count == command_buffer->upload_cycle_state_capacity) {
		new_capacity =
			command_buffer->upload_cycle_state_capacity ? command_buffer->upload_cycle_state_capacity * 2u : 16u;
		if (new_capacity < command_buffer->upload_cycle_state_capacity ||
			new_capacity > UINT32_MAX / (uint32_t)sizeof(*states)) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		states = (AeronUploadCycleState*)SDL_realloc(
			command_buffer->upload_cycle_states, (size_t)new_capacity * sizeof(*states));
		if (!states) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		command_buffer->upload_cycle_states = states;
		command_buffer->upload_cycle_state_capacity = new_capacity;
	}
	AeronUploadCycleState* state =
		&command_buffer->upload_cycle_states[command_buffer->upload_cycle_state_count++];
	state->resource = resource;
	state->kind = kind;
	state->cycled = requested != 0;
	*out_cycle = requested != 0;
	return 1;
}

static int Aeron_AlignUploadOffset(uint32_t value, uint32_t alignment, uint32_t* out) {
	uint32_t mask;

	if (!out || alignment == 0 || (alignment & (alignment - 1u)) != 0) {
		return 0;
	}
	mask = alignment - 1u;
	if (value > UINT32_MAX - mask) {
		return 0;
	}
	*out = (value + mask) & ~mask;
	return 1;
}

static uint32_t Aeron_NextUploadChunkCapacity(AeronCommandBuffer* command_buffer, uint32_t required) {
	uint32_t capacity;

	capacity = command_buffer->next_upload_chunk_size;
	if (capacity < AERON_UPLOAD_CHUNK_MIN_BYTES) {
		capacity = AERON_UPLOAD_CHUNK_MIN_BYTES;
	}
	while (capacity < required && capacity < AERON_UPLOAD_CHUNK_TARGET_BYTES) {
		if (capacity > AERON_UPLOAD_CHUNK_TARGET_BYTES / 2u) {
			capacity = AERON_UPLOAD_CHUNK_TARGET_BYTES;
			break;
		}
		capacity *= 2u;
	}
	if (capacity < required) {
		if (!Aeron_AlignUploadOffset(required, AERON_UPLOAD_CHUNK_MIN_BYTES, &capacity)) {
			return 0;
		}
	}
	if (capacity < AERON_UPLOAD_CHUNK_TARGET_BYTES) {
		uint32_t next = capacity > AERON_UPLOAD_CHUNK_TARGET_BYTES / 2u
							? AERON_UPLOAD_CHUNK_TARGET_BYTES
							: capacity * 2u;
		if (next > command_buffer->next_upload_chunk_size) {
			command_buffer->next_upload_chunk_size = next;
		}
	}
	return capacity;
}

static AeronUploadChunk* Aeron_CommandBufferAddUploadChunk(AeronCommandBuffer* command_buffer,
															uint32_t required) {
	SDL_GPUTransferBufferCreateInfo transfer_info;
	AeronUploadChunk*                chunks;
	AeronUploadChunk*                chunk;
	uint32_t                         capacity;
	uint32_t                         new_capacity;

	if (!command_buffer || command_buffer->failed || !command_buffer->owns_wrapper) {
		return NULL;
	}
	capacity = Aeron_NextUploadChunkCapacity(command_buffer, required);
	if (capacity == 0) {
		Aeron_LogError("aeron", "GPU upload chunk size overflow for %u bytes", required);
		Aeron_CommandBufferMarkFailed(command_buffer);
		return NULL;
	}
	if (command_buffer->upload_chunk_count == command_buffer->upload_chunk_capacity) {
		new_capacity = command_buffer->upload_chunk_capacity ? command_buffer->upload_chunk_capacity * 2u : 4u;
		if (new_capacity < command_buffer->upload_chunk_capacity ||
			new_capacity > UINT32_MAX / (uint32_t)sizeof(*chunks)) {
			Aeron_LogError("aeron", "GPU upload chunk registry overflow");
			Aeron_CommandBufferMarkFailed(command_buffer);
			return NULL;
		}
		chunks = (AeronUploadChunk*)SDL_realloc(
			command_buffer->upload_chunks, (size_t)new_capacity * sizeof(*chunks));
		if (!chunks) {
			Aeron_LogError("aeron", "GPU upload chunk registry allocation failed");
			Aeron_CommandBufferMarkFailed(command_buffer);
			return NULL;
		}
		command_buffer->upload_chunks         = chunks;
		command_buffer->upload_chunk_capacity = new_capacity;
	}

	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size  = capacity;
	SDL_ClearError();
	chunk = &command_buffer->upload_chunks[command_buffer->upload_chunk_count];
	SDL_zero(*chunk);
	chunk->transfer = SDL_CreateGPUTransferBuffer(g_aeron.gpu_device, &transfer_info);
	if (!chunk->transfer) {
		const char* error = SDL_GetError();
		Aeron_CommandBufferFail(command_buffer,
							   "SDL_CreateGPUTransferBuffer failed for %u-byte upload chunk: %s", capacity,
							   error && error[0] ? error : "<no SDL error provided>");
		return NULL;
	}
	chunk->capacity = capacity;
	command_buffer->upload_chunk_count++;
	command_buffer->upload_reserved_bytes += capacity;
	g_aeron.render_data_stats.upload_reserved_bytes += capacity;
	g_aeron.render_data_stats.upload_chunk_count++;
	return chunk;
}

static int Aeron_CommandBufferBeginUploadSlice(AeronCommandBuffer* command_buffer, uint32_t size,
												uint32_t alignment, AeronUploadSlice* out) {
	AeronUploadChunk* chunk;
	uint32_t          offset;
	uint32_t          required;
	void*             mapped;

	if (!command_buffer || !out || !command_buffer->command_buffer || !command_buffer->owns_wrapper ||
		command_buffer->failed || command_buffer->compute_pass_active || command_buffer->render_pass_active ||
		size == 0) {
		return 0;
	}
	SDL_zero(*out);
	chunk = command_buffer->upload_chunk_count
				? &command_buffer->upload_chunks[command_buffer->upload_chunk_count - 1u]
				: NULL;
	if (!chunk || !Aeron_AlignUploadOffset(chunk->used, alignment, &offset) ||
		offset > chunk->capacity || size > chunk->capacity - offset) {
		if (size > UINT32_MAX - (alignment - 1u)) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		required = size + alignment - 1u;
		chunk    = Aeron_CommandBufferAddUploadChunk(command_buffer, required);
		if (!chunk || !Aeron_AlignUploadOffset(chunk->used, alignment, &offset) ||
			offset > chunk->capacity || size > chunk->capacity - offset) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
	}

	SDL_ClearError();
	mapped = SDL_MapGPUTransferBuffer(g_aeron.gpu_device, chunk->transfer, false);
	if (!mapped) {
		const char* error = SDL_GetError();
		Aeron_CommandBufferFail(command_buffer, "SDL_MapGPUTransferBuffer failed for %u-byte upload slice: %s",
							   size, error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}
	out->chunk  = chunk;
	out->offset = offset;
	out->size   = size;
	out->mapped = (uint8_t*)mapped + offset;
	return 1;
}

static void Aeron_CommandBufferEndUploadSlice(AeronCommandBuffer* command_buffer, AeronUploadSlice* slice,
											  int commit) {
	if (!command_buffer || !slice || !slice->chunk) {
		return;
	}
	SDL_UnmapGPUTransferBuffer(g_aeron.gpu_device, slice->chunk->transfer);
	if (commit) {
		slice->chunk->used = slice->offset + slice->size;
		command_buffer->upload_staged_bytes += slice->size;
		g_aeron.render_data_stats.upload_staged_bytes += slice->size;
	}
	slice->mapped = NULL;
}

int Aeron_SubmitCommandBuffer(AeronCommandBuffer* command_buffer) {
	int ok;

	if (!command_buffer) {
		return 0;
	}

	ok = command_buffer->failed == 0;
	if (command_buffer->compute_pass_active) {
		Aeron_LogError("aeron", "Cannot submit a command buffer with an active compute pass");
		Aeron_EndComputePass(&command_buffer->active_compute_pass);
		ok = 0;
	}
	if (command_buffer->render_pass_active) {
		Aeron_CommandBufferFail(command_buffer, "Cannot submit a command buffer with an active render pass");
		ok = 0;
	}
	if (!ok) {
		if (command_buffer->command_buffer) {
			SDL_CancelGPUCommandBuffer(command_buffer->command_buffer);
		}
	} else if (command_buffer->command_buffer) {
		SDL_ClearError();
		if (!SDL_SubmitGPUCommandBuffer(command_buffer->command_buffer)) {
			const char* error = SDL_GetError();
			Aeron_CommandBufferFail(command_buffer, "SDL_SubmitGPUCommandBuffer failed: %s",
								   error && error[0] ? error : "<no SDL error provided>");
			ok = 0;
		}
	}
	if (command_buffer->upload_chunk_count > 0) {
		g_aeron.render_data_stats.upload_command_buffer_submission_count++;
		if (command_buffer->immediate_upload) {
			g_aeron.render_data_stats.immediate_upload_submission_count++;
		}
	}
	Aeron_ReleaseUploadChunks(command_buffer);
	SDL_free(command_buffer);
	return ok;
}

void Aeron_CancelCommandBuffer(AeronCommandBuffer* command_buffer) {
	if (!command_buffer) {
		return;
	}

	if (command_buffer->compute_pass_active) {
		Aeron_EndComputePass(&command_buffer->active_compute_pass);
	}
	if (command_buffer->command_buffer) {
		SDL_CancelGPUCommandBuffer(command_buffer->command_buffer);
	}
	Aeron_ReleaseUploadChunks(command_buffer);
	SDL_free(command_buffer);
}

int Aeron_CommandBufferGetUploadUsage(const AeronCommandBuffer* command_buffer,
									  AeronCommandBufferUploadUsage* out) {
	if (!command_buffer || !out) {
		return 0;
	}
	out->staged_bytes    = command_buffer->upload_staged_bytes;
	out->reserved_bytes  = command_buffer->upload_reserved_bytes;
	out->copy_count      = command_buffer->upload_copy_count;
	out->buffer_copy_count = command_buffer->upload_buffer_copy_count;
	out->texture_copy_count = command_buffer->upload_texture_copy_count;
	out->chunk_count     = command_buffer->upload_chunk_count;
	out->copy_pass_count = command_buffer->upload_copy_pass_count;
	out->largest_upload_bytes = command_buffer->largest_upload_bytes;
	return 1;
}

AeronBuffer* Aeron_CreateBuffer(const AeronBufferDesc* desc) {
	SDL_GPUBufferCreateInfo buffer_info;
	SDL_GPUBufferUsageFlags usage;
	AeronBuffer*            buffer;

	if (!g_aeron.gpu_device || !desc || desc->size == 0 || desc->usage == 0) {
		return NULL;
	}

	usage = Aeron_ToSdlBufferUsage(desc->usage);
	if (usage == 0) {
		Aeron_LogError("aeron", "Unsupported Aeron buffer usage 0x%08x", desc->usage);
		return NULL;
	}

	buffer = (AeronBuffer*)SDL_calloc(1, sizeof(*buffer));
	if (!buffer) {
		Aeron_SetRenderError("Could not allocate the %u-byte Aeron buffer wrapper", desc->size);
		return NULL;
	}

	SDL_zero(buffer_info);
	buffer_info.usage = usage;
	buffer_info.size  = desc->size;

	SDL_ClearError();
	buffer->buffer = SDL_CreateGPUBuffer(g_aeron.gpu_device, &buffer_info);
	if (!buffer->buffer) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_CreateGPUBuffer failed for %u bytes usage 0x%08x: %s", desc->size,
							 desc->usage, error && error[0] ? error : "<no SDL error provided>");
		SDL_free(buffer);
		return NULL;
	}
	AeronGpuDebug_NameBuffer(g_aeron.gpu_device, buffer->buffer, desc->debug_name);

	buffer->size         = desc->size;
	buffer->usage        = desc->usage;
	buffer->memory_usage = desc->memory_usage;
	return buffer;
}

void Aeron_DestroyBuffer(AeronBuffer* buffer) {
	if (!buffer) {
		return;
	}

	if (buffer->buffer) {
		SDL_ReleaseGPUBuffer(g_aeron.gpu_device, buffer->buffer);
		buffer->buffer = NULL;
	}

	SDL_free(buffer);
}

int Aeron_UploadBufferData(AeronBuffer* buffer, uint32_t offset, const void* data, uint32_t size) {
	AeronCommandBuffer* command_buffer;
	int                 staged;

	command_buffer = Aeron_AcquireCommandBuffer();
	if (!command_buffer) {
		return 0;
	}
	command_buffer->immediate_upload = 1;
	staged = Aeron_UploadBufferDataCmd(command_buffer, buffer, offset, data, size);
	if (!staged) {
		Aeron_CancelCommandBuffer(command_buffer);
		return 0;
	}
	return Aeron_SubmitCommandBuffer(command_buffer);
}

int Aeron_UploadBufferDataCmd(AeronCommandBuffer* command_buffer, AeronBuffer* buffer, uint32_t offset,
							  const void* data, uint32_t size) {
	const AeronBufferUploadDesc upload = {
		.buffer = buffer,
		.offset = offset,
		.data   = data,
		.size   = size,
	};
	return Aeron_UploadBufferBatchCmd(command_buffer, &upload, 1);
}

int Aeron_UploadBufferBatchCmd(AeronCommandBuffer* command_buffer, const AeronBufferUploadDesc* uploads,
							   uint32_t upload_count) {
	SDL_GPUCopyPass* copy_pass;
	AeronUploadSlice slice;
	uint32_t         transfer_size;
	uint32_t         transfer_offset;
	uint32_t         i;

	if (!g_aeron.gpu_device || !command_buffer || !command_buffer->command_buffer || command_buffer->failed ||
		!uploads || upload_count == 0) {
		return 0;
	}
	if (!command_buffer->owns_wrapper || command_buffer->compute_pass_active || command_buffer->render_pass_active) {
		Aeron_CommandBufferFail(command_buffer,
							   "Buffer uploads require an owned command buffer with no active render or compute pass");
		return 0;
	}

	transfer_size = 0;
	for (i = 0; i < upload_count; ++i) {
		const AeronBufferUploadDesc* upload = &uploads[i];
		uint32_t                     aligned_offset;

			if (!upload->buffer || !upload->buffer->buffer || !upload->data || upload->size == 0 ||
				upload->offset > upload->buffer->size || upload->size > upload->buffer->size - upload->offset) {
				Aeron_CommandBufferFail(command_buffer, "Invalid buffer upload descriptor at batch index %u", i);
				return 0;
			}
		if (!Aeron_AlignUploadOffset(transfer_size, 16u, &aligned_offset)) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		if (upload->size > UINT32_MAX - aligned_offset) {
			Aeron_CommandBufferMarkFailed(command_buffer);
			return 0;
		}
		transfer_size = aligned_offset + upload->size;
	}

	if (!Aeron_CommandBufferBeginUploadSlice(command_buffer, transfer_size, 16u, &slice)) {
		return 0;
	}
	transfer_offset = 0;
	for (i = 0; i < upload_count; ++i) {
		(void)Aeron_AlignUploadOffset(transfer_offset, 16u, &transfer_offset);
		SDL_memcpy(slice.mapped + transfer_offset, uploads[i].data, uploads[i].size);
		transfer_offset += uploads[i].size;
	}
	Aeron_CommandBufferEndUploadSlice(command_buffer, &slice, 1);

	AeronGpuDebug_Push(command_buffer->command_buffer, "Batched buffer upload");
	SDL_ClearError();
	copy_pass = SDL_BeginGPUCopyPass(command_buffer->command_buffer);
	if (!copy_pass) {
		const char* error = SDL_GetError();
		AeronGpuDebug_Pop(command_buffer->command_buffer);
		Aeron_CommandBufferFail(command_buffer, "SDL_BeginGPUCopyPass failed for batched buffer upload: %s",
							   error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}

	transfer_offset = 0;
	for (i = 0; i < upload_count; ++i) {
		SDL_GPUTransferBufferLocation source;
		SDL_GPUBufferRegion           destination;
		int                           cycle;

		(void)Aeron_AlignUploadOffset(transfer_offset, 16u, &transfer_offset);
		SDL_zero(source);
		source.transfer_buffer = slice.chunk->transfer;
		source.offset          = slice.offset + transfer_offset;
		SDL_zero(destination);
		destination.buffer = uploads[i].buffer->buffer;
		destination.offset = uploads[i].offset;
		destination.size   = uploads[i].size;

		if (!Aeron_CommandBufferResolveCycle(
				command_buffer, uploads[i].buffer, 0,
				uploads[i].buffer->memory_usage == AERON_MEMORY_USAGE_DYNAMIC, &cycle)) {
			SDL_EndGPUCopyPass(copy_pass);
			AeronGpuDebug_Pop(command_buffer->command_buffer);
			return 0;
		}
		SDL_UploadToGPUBuffer(copy_pass, &source, &destination, cycle);
		transfer_offset += uploads[i].size;
	}
	SDL_EndGPUCopyPass(copy_pass);
	AeronGpuDebug_Pop(command_buffer->command_buffer);
	command_buffer->upload_copy_count += upload_count;
	command_buffer->upload_buffer_copy_count += upload_count;
	command_buffer->upload_copy_pass_count++;
	g_aeron.render_data_stats.buffer_copy_count += upload_count;
	g_aeron.render_data_stats.upload_copy_pass_count++;
	for (i = 0; i < upload_count; ++i) {
		if (uploads[i].size > command_buffer->largest_upload_bytes) {
			command_buffer->largest_upload_bytes = uploads[i].size;
		}
		if (uploads[i].size > g_aeron.render_data_stats.largest_upload_bytes) {
			g_aeron.render_data_stats.largest_upload_bytes = uploads[i].size;
		}
		Aeron_RecordStorageUpload(uploads[i].buffer, uploads[i].size);
	}
	return 1;
}

AeronTexture* Aeron_CreateTexture(const AeronTextureDesc* desc) {
	SDL_GPUTextureCreateInfo texture_info;
	SDL_GPUTextureFormat     format;
	SDL_GPUTextureUsageFlags usage;
	AeronTexture*            texture;
	int                      mip_count;

	if (!g_aeron.gpu_device || !desc || desc->width <= 0 || desc->height <= 0 || desc->usage == 0) {
		return NULL;
	}

	format = Aeron_ToSdlTextureFormat(desc->format);
	usage  = Aeron_ToSdlTextureUsage(desc->usage);
	if (format == SDL_GPU_TEXTUREFORMAT_INVALID || usage == 0) {
		Aeron_LogError("aeron", "Unsupported Aeron texture %dx%d format %d usage 0x%08x", desc->width,
				  desc->height, desc->format, desc->usage);
		return NULL;
	}

	if ((desc->usage & AERON_TEXTURE_USAGE_DEPTH_TARGET) != 0 && !Aeron_IsDepthTextureFormat(desc->format)) {
		return NULL;
	}
	if ((desc->usage & AERON_TEXTURE_USAGE_COLOR_TARGET) != 0 && !Aeron_IsColorTextureFormat(desc->format)) {
		return NULL;
	}

	mip_count = desc->mip_count > 0 ? desc->mip_count : 1;
	texture   = (AeronTexture*)SDL_calloc(1, sizeof(*texture));
	if (!texture) {
		Aeron_SetRenderError("Could not allocate the %dx%d Aeron texture wrapper", desc->width, desc->height);
		return NULL;
	}

	SDL_zero(texture_info);
	texture_info.type                 = desc->cube ? SDL_GPU_TEXTURETYPE_CUBE : SDL_GPU_TEXTURETYPE_2D;
	texture_info.format               = format;
	texture_info.usage                = usage;
	texture_info.width                = (uint32_t)desc->width;
	texture_info.height               = (uint32_t)desc->height;
	texture_info.layer_count_or_depth = desc->cube ? 6 : 1;
	texture_info.num_levels           = (uint32_t)mip_count;
	texture_info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	SDL_ClearError();
	texture->texture = SDL_CreateGPUTexture(g_aeron.gpu_device, &texture_info);
	if (!texture->texture) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_CreateGPUTexture failed for texture %dx%d format %d usage 0x%08x: %s",
							 desc->width, desc->height, desc->format, desc->usage,
							 error && error[0] ? error : "<no SDL error provided>");
		SDL_free(texture);
		return NULL;
	}
	AeronGpuDebug_NameTexture(g_aeron.gpu_device, texture->texture, desc->debug_name);

	texture->width        = desc->width;
	texture->height       = desc->height;
	texture->mip_count    = mip_count;
	texture->layer_count  = desc->cube ? 6 : 1;
	texture->format       = desc->format;
	texture->usage        = desc->usage;
	texture->sample_count = AERON_SAMPLE_COUNT_1;
	texture->owned        = 1;
	return texture;
}

void Aeron_DestroyTexture(AeronTexture* texture) {
	int i;

	if (!texture || !texture->owned) {
		return;
	}

	for (i = 0; i < g_aeron.render_layer_count; ++i) {
		AeronRenderLayer* layer = &g_aeron.render_layers[i];
		if (layer->kind == AERON_RENDER_LAYER_TEXTURE && layer->u.texture.texture == texture) {
			layer->kind = AERON_RENDER_LAYER_NONE;
		}
	}

	if (texture->texture) {
		SDL_ReleaseGPUTexture(g_aeron.gpu_device, texture->texture);
		texture->texture = NULL;
	}

	SDL_free(texture);
}

static int Aeron_ValidateTextureUpload(const AeronTextureUploadDesc* desc, uint32_t* out_size,
									   SDL_PixelFormat* out_format) {
	uint64_t        transfer_size;
	SDL_PixelFormat upload_format;
	int             bytes_per_pixel;
	int             mip_width;
	int             mip_height;

	if (!desc || !out_size || !out_format || !desc->texture || !desc->texture->texture ||
		(!desc->pixels && !desc->raw_data) || desc->width <= 0 || desc->height <= 0 || desc->mip_level < 0 ||
		desc->mip_level >= desc->texture->mip_count || desc->layer < 0 ||
		desc->layer >= desc->texture->layer_count || desc->x < 0 || desc->y < 0 ||
		(desc->texture->usage & AERON_TEXTURE_USAGE_TRANSFER_DST) == 0) {
		return 0;
	}
	mip_width  = desc->texture->width >> desc->mip_level;
	mip_height = desc->texture->height >> desc->mip_level;
	if (mip_width < 1) {
		mip_width = 1;
	}
	if (mip_height < 1) {
		mip_height = 1;
	}
	if (desc->x > mip_width || desc->width > mip_width - desc->x || desc->y > mip_height ||
		desc->height > mip_height - desc->y) {
		return 0;
	}

	upload_format = SDL_PIXELFORMAT_UNKNOWN;
	if (desc->raw_data) {
		if (desc->raw_size == 0) {
			return 0;
		}
		transfer_size = desc->raw_size;
	} else {
		if (desc->pitch <= 0) {
			return 0;
		}
		upload_format = Aeron_SdlPixelFormatForTextureUpload(desc->texture->format);
		if (upload_format == SDL_PIXELFORMAT_UNKNOWN) {
			return 0;
		}
		bytes_per_pixel = Aeron_PixelFormatBytesPerPixel(desc->pixel_format);
		if (!bytes_per_pixel || desc->width > INT_MAX / bytes_per_pixel ||
			desc->pitch < desc->width * bytes_per_pixel) {
			return 0;
		}
		transfer_size = (uint64_t)desc->width * (uint64_t)desc->height * 4u;
	}
	if (transfer_size == 0 || transfer_size > UINT32_MAX) {
		return 0;
	}
	*out_size   = (uint32_t)transfer_size;
	*out_format = upload_format;
	return 1;
}

int Aeron_UploadTextureBatchCmd(AeronCommandBuffer* command_buffer, const AeronTextureUploadDesc* uploads,
								uint32_t upload_count) {
	SDL_GPUCopyPass* copy_pass;
	AeronUploadSlice slice;
	uint32_t         transfer_size;
	uint32_t         transfer_offset;
	uint32_t         i;

	if (!g_aeron.gpu_device || !command_buffer || !command_buffer->command_buffer || command_buffer->failed ||
		!uploads || upload_count == 0) {
		return 0;
	}
	if (!command_buffer->owns_wrapper || command_buffer->compute_pass_active || command_buffer->render_pass_active) {
		Aeron_CommandBufferFail(command_buffer,
							   "Texture uploads require an owned command buffer with no active render or compute pass");
		return 0;
	}
	transfer_size = 0;
	for (i = 0; i < upload_count; ++i) {
		uint32_t        upload_size;
		uint32_t        aligned_offset;
		SDL_PixelFormat upload_format;
		if (!Aeron_ValidateTextureUpload(&uploads[i], &upload_size, &upload_format) ||
			!Aeron_AlignUploadOffset(transfer_size, 16u, &aligned_offset) ||
			upload_size > UINT32_MAX - aligned_offset) {
			Aeron_CommandBufferFail(command_buffer, "Invalid texture upload descriptor at batch index %u", i);
			return 0;
		}
		transfer_size = aligned_offset + upload_size;
	}
	if (!Aeron_CommandBufferBeginUploadSlice(command_buffer, transfer_size, 16u, &slice)) {
		return 0;
	}

	transfer_offset = 0;
	for (i = 0; i < upload_count; ++i) {
		const AeronTextureUploadDesc* upload = &uploads[i];
		uint32_t                      upload_size;
		SDL_PixelFormat               upload_format;
		(void)Aeron_ValidateTextureUpload(upload, &upload_size, &upload_format);
		(void)Aeron_AlignUploadOffset(transfer_offset, 16u, &transfer_offset);
		if (upload->raw_data) {
			SDL_memcpy(slice.mapped + transfer_offset, upload->raw_data, upload_size);
			} else if (!Aeron_ConvertTextureUploadToGpuPixels(slice.mapped + transfer_offset, upload,
															 upload_format)) {
				Aeron_CommandBufferEndUploadSlice(command_buffer, &slice, 0);
				Aeron_CommandBufferFail(command_buffer, "Texture pixel conversion failed at batch index %u", i);
				return 0;
			}
		transfer_offset += upload_size;
	}
	Aeron_CommandBufferEndUploadSlice(command_buffer, &slice, 1);

	AeronGpuDebug_Push(command_buffer->command_buffer, "Batched texture upload");
	SDL_ClearError();
	copy_pass = SDL_BeginGPUCopyPass(command_buffer->command_buffer);
	if (!copy_pass) {
		const char* error = SDL_GetError();
		AeronGpuDebug_Pop(command_buffer->command_buffer);
		Aeron_CommandBufferFail(command_buffer, "SDL_BeginGPUCopyPass failed for batched texture upload: %s",
							   error && error[0] ? error : "<no SDL error provided>");
		return 0;
	}

	transfer_offset = 0;
	for (i = 0; i < upload_count; ++i) {
		const AeronTextureUploadDesc* upload = &uploads[i];
		SDL_GPUTextureTransferInfo    source;
		SDL_GPUTextureRegion          destination;
		uint32_t                      upload_size;
		SDL_PixelFormat               upload_format;

		(void)Aeron_ValidateTextureUpload(upload, &upload_size, &upload_format);
		(void)Aeron_AlignUploadOffset(transfer_offset, 16u, &transfer_offset);
		SDL_zero(source);
		source.transfer_buffer = slice.chunk->transfer;
		source.offset          = slice.offset + transfer_offset;
		source.pixels_per_row  = upload->raw_data ? 0u : (uint32_t)upload->width;
		source.rows_per_layer  = upload->raw_data ? 0u : (uint32_t)upload->height;
		SDL_zero(destination);
		destination.texture   = upload->texture->texture;
		destination.mip_level = (uint32_t)upload->mip_level;
		destination.layer     = (uint32_t)upload->layer;
		destination.x         = (uint32_t)upload->x;
		destination.y         = (uint32_t)upload->y;
		destination.w         = (uint32_t)upload->width;
		destination.h         = (uint32_t)upload->height;
		destination.d         = 1;
		int cycle;
		if (!Aeron_CommandBufferResolveCycle(command_buffer, upload->texture, 1, upload->cycle != 0,
											 &cycle)) {
			SDL_EndGPUCopyPass(copy_pass);
			AeronGpuDebug_Pop(command_buffer->command_buffer);
			return 0;
		}
		SDL_UploadToGPUTexture(copy_pass, &source, &destination, cycle);
		transfer_offset += upload_size;
	}
	SDL_EndGPUCopyPass(copy_pass);
	AeronGpuDebug_Pop(command_buffer->command_buffer);
	command_buffer->upload_copy_count += upload_count;
	command_buffer->upload_texture_copy_count += upload_count;
	command_buffer->upload_copy_pass_count++;
	g_aeron.render_data_stats.texture_copy_count += upload_count;
	g_aeron.render_data_stats.upload_copy_pass_count++;
	for (i = 0; i < upload_count; ++i) {
		uint32_t        upload_size;
		SDL_PixelFormat upload_format;
		(void)Aeron_ValidateTextureUpload(&uploads[i], &upload_size, &upload_format);
		if (upload_size > command_buffer->largest_upload_bytes) {
			command_buffer->largest_upload_bytes = upload_size;
		}
		if (upload_size > g_aeron.render_data_stats.largest_upload_bytes) {
			g_aeron.render_data_stats.largest_upload_bytes = upload_size;
		}
	}
	return 1;
}

int Aeron_UploadTextureDataCmd(AeronCommandBuffer* command_buffer, const AeronTextureUploadDesc* desc) {
	return Aeron_UploadTextureBatchCmd(command_buffer, desc, 1);
}

int Aeron_UploadTextureData(const AeronTextureUploadDesc* desc) {
	AeronCommandBuffer* command_buffer;
	int                 staged;

	command_buffer = Aeron_AcquireCommandBuffer();
	if (!command_buffer) {
		return 0;
	}
	command_buffer->immediate_upload = 1;
	staged = Aeron_UploadTextureDataCmd(command_buffer, desc);
	if (!staged) {
		Aeron_CancelCommandBuffer(command_buffer);
		return 0;
	}
	return Aeron_SubmitCommandBuffer(command_buffer);
}

AeronSampler* Aeron_CreateSampler(const AeronSamplerDesc* desc) {
	SDL_GPUSamplerCreateInfo sampler_info;
	AeronSampler*            sampler;

	if (!g_aeron.gpu_device || !desc) {
		return NULL;
	}

	sampler = (AeronSampler*)SDL_calloc(1, sizeof(*sampler));
	if (!sampler) {
		return NULL;
	}
	sampler->desc = *desc;

	SDL_zero(sampler_info);
	sampler_info.min_filter        = Aeron_ToSdlFilter(desc->min_filter);
	sampler_info.mag_filter        = Aeron_ToSdlFilter(desc->mag_filter);
	sampler_info.mipmap_mode       = Aeron_ToSdlMipFilter(desc->mip_filter);
	sampler_info.address_mode_u    = Aeron_ToSdlAddressMode(desc->address_u);
	sampler_info.address_mode_v    = Aeron_ToSdlAddressMode(desc->address_v);
	sampler_info.address_mode_w    = Aeron_ToSdlAddressMode(desc->address_w);
	sampler_info.mip_lod_bias      = desc->mip_lod_bias;
	sampler_info.min_lod           = desc->min_lod;
	sampler_info.max_lod           = desc->max_lod;
	sampler_info.enable_anisotropy = desc->enable_anisotropy != 0;
	sampler_info.max_anisotropy    = desc->enable_anisotropy != 0 ? desc->max_anisotropy : 0.0f;
	sampler_info.enable_compare    = desc->enable_compare != 0;
	sampler_info.compare_op        = Aeron_ToSdlCompareOp(desc->compare);

	sampler->sampler = SDL_CreateGPUSampler(g_aeron.gpu_device, &sampler_info);
	if (!sampler->sampler) {
		Aeron_SetRenderError("SDL_CreateGPUSampler failed: %s", SDL_GetError());
		SDL_free(sampler);
		return NULL;
	}

	return sampler;
}

int Aeron_SamplerGetDesc(const AeronSampler* sampler, AeronSamplerDesc* desc) {
	if (!sampler || !desc) {
		return 0;
	}
	*desc = sampler->desc;
	return 1;
}

void Aeron_DestroySampler(AeronSampler* sampler) {
	if (!sampler) {
		return;
	}

	if (sampler->sampler) {
		SDL_ReleaseGPUSampler(g_aeron.gpu_device, sampler->sampler);
		sampler->sampler = NULL;
	}

	SDL_free(sampler);
}

AeronShader* Aeron_CreateShader(const AeronShaderDesc* desc) {
	const AeronShaderFormatInfo* format_info;
	AeronShader*                 shader;

	if (!g_aeron.gpu_device || !desc || !desc->name || desc->stage == AERON_SHADER_STAGE_COMPUTE) {
		return NULL;
	}

	format_info = Aeron_SelectShaderFormat();
	if (!format_info) {
		Aeron_SetRenderError("GPU backend does not support Aeron compiled shader formats");
		return NULL;
	}

	shader = (AeronShader*)SDL_calloc(1, sizeof(*shader));
	if (!shader) {
		return NULL;
	}

	shader->shader =
		Aeron_LoadShader(desc->name, Aeron_ToSdlShaderStage(desc->stage), format_info, desc->sampler_count,
						 desc->uniform_buffer_count, desc->storage_buffer_count);
	if (!shader->shader) {
		SDL_free(shader);
		return NULL;
	}

	shader->stage                = desc->stage;
	shader->sampler_count        = desc->sampler_count;
	shader->uniform_buffer_count = desc->uniform_buffer_count;
	shader->storage_buffer_count = desc->storage_buffer_count;
	return shader;
}

void Aeron_DestroyShader(AeronShader* shader) {
	if (!shader) {
		return;
	}

	if (shader->shader) {
		SDL_ReleaseGPUShader(g_aeron.gpu_device, shader->shader);
		shader->shader = NULL;
	}

	SDL_free(shader);
}

AeronComputePipeline* Aeron_CreateComputePipeline(const AeronComputePipelineDesc* desc) {
	const AeronShaderFormatInfo*     format_info;
	SDL_GPUComputePipelineCreateInfo create_info;
	AeronComputePipeline*            pipeline;
	char                             path[AERON_MAX_PATH];
	size_t                           code_size;
	Uint8*                           code;

	if (!g_aeron.gpu_device || !desc || !desc->name || desc->thread_count_x == 0 ||
		desc->thread_count_y == 0 || desc->thread_count_z == 0) {
		return NULL;
	}

	format_info = Aeron_SelectShaderFormat();
	if (!format_info) {
		return NULL;
	}
	code = Aeron_LoadShaderFile(desc->name, format_info->extension, &code_size, path, sizeof(path));
	if (!code) {
		Aeron_SetRenderError("Failed to load %s compute shader '%s': %s", format_info->name, path,
							 SDL_GetError());
		return NULL;
	}

	SDL_zero(create_info);
	create_info.code_size                     = code_size;
	create_info.code                          = code;
	create_info.entrypoint                    = desc->entrypoint ? desc->entrypoint : format_info->entrypoint;
	create_info.format                        = format_info->format;
	create_info.num_samplers                  = desc->sampler_count;
	create_info.num_readonly_storage_textures = desc->readonly_storage_texture_count;
	create_info.num_readonly_storage_buffers  = desc->readonly_storage_buffer_count;
	create_info.num_readwrite_storage_textures = desc->readwrite_storage_texture_count;
	create_info.num_readwrite_storage_buffers  = desc->readwrite_storage_buffer_count;
	create_info.num_uniform_buffers            = desc->uniform_buffer_count;
	create_info.threadcount_x                  = desc->thread_count_x;
	create_info.threadcount_y                  = desc->thread_count_y;
	create_info.threadcount_z                  = desc->thread_count_z;

	pipeline = (AeronComputePipeline*)SDL_calloc(1, sizeof(*pipeline));
	if (!pipeline) {
		SDL_free(code);
		return NULL;
	}
	pipeline->pipeline = SDL_CreateGPUComputePipeline(g_aeron.gpu_device, &create_info);
	SDL_free(code);
	if (!pipeline->pipeline) {
		Aeron_SetRenderError("SDL_CreateGPUComputePipeline failed for '%s': %s", path, SDL_GetError());
		SDL_free(pipeline);
		return NULL;
	}
	return pipeline;
}

void Aeron_DestroyComputePipeline(AeronComputePipeline* pipeline) {
	if (!pipeline) {
		return;
	}
	if (pipeline->pipeline) {
		SDL_ReleaseGPUComputePipeline(g_aeron.gpu_device, pipeline->pipeline);
	}
	SDL_free(pipeline);
}

/* Fills one SDL color-target description from an Aeron format + blend pair. */
static int Aeron_FillColorTargetDesc(SDL_GPUColorTargetDescription* out, AeronTextureFormat format,
									 const AeronBlendStateDesc* blend) {
	SDL_GPUTextureFormat sdl_format;

	sdl_format = Aeron_ToSdlTextureFormat(format);
	if (sdl_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return 0;
	}

	SDL_zero(*out);
	out->format = sdl_format;
	if (blend->enabled) {
		out->blend_state.src_color_blendfactor = Aeron_ToSdlBlendFactor(blend->src_color);
		out->blend_state.dst_color_blendfactor = Aeron_ToSdlBlendFactor(blend->dst_color);
		out->blend_state.color_blend_op        = Aeron_ToSdlBlendOp(blend->color_op);
		out->blend_state.src_alpha_blendfactor = Aeron_ToSdlBlendFactor(blend->src_alpha);
		out->blend_state.dst_alpha_blendfactor = Aeron_ToSdlBlendFactor(blend->dst_alpha);
		out->blend_state.alpha_blend_op        = Aeron_ToSdlBlendOp(blend->alpha_op);
		out->blend_state.enable_blend          = true;
	}
	if (blend->color_write_mask_enable) {
		out->blend_state.color_write_mask        = blend->color_write_mask;
		out->blend_state.enable_color_write_mask = true;
	}
	return 1;
}

#define AERON_MAX_COLOR_TARGETS 4

AeronGraphicsPipeline* Aeron_CreateGraphicsPipeline(const AeronGraphicsPipelineDesc* desc) {
	SDL_GPUVertexBufferDescription*   vertex_buffers;
	SDL_GPUVertexAttribute*           attributes;
	SDL_GPUColorTargetDescription     color_targets[AERON_MAX_COLOR_TARGETS];
	SDL_GPUGraphicsPipelineCreateInfo pipeline_info;
	SDL_GPUTextureFormat              depth_format;
	AeronGraphicsPipeline*            pipeline;
	AeronSampleCount                  sample_count;
	uint32_t                          color_target_count;
	uint32_t                          i;

	if (!g_aeron.gpu_device || !desc || !desc->vertex_shader || !desc->fragment_shader ||
		!desc->vertex_shader->shader || !desc->fragment_shader->shader ||
		desc->vertex_shader->stage != AERON_SHADER_STAGE_VERTEX ||
		desc->fragment_shader->stage != AERON_SHADER_STAGE_FRAGMENT) {
		return NULL;
	}
	sample_count = Aeron_DescSampleCount(desc->sample_count);
	if (!Aeron_IsValidSampleCount(sample_count)) {
		return NULL;
	}

	if (desc->color_target_count > 0) {
		if (!desc->color_targets || desc->color_target_count > AERON_MAX_COLOR_TARGETS) {
			return NULL;
		}
		color_target_count = desc->color_target_count;
		for (i = 0; i < color_target_count; ++i) {
			if (!Aeron_FillColorTargetDesc(&color_targets[i], desc->color_targets[i].format,
										   &desc->color_targets[i].blend)) {
				return NULL;
			}
		}
	} else if (desc->color_format != AERON_TEXTURE_FORMAT_UNKNOWN) {
		if (!Aeron_FillColorTargetDesc(&color_targets[0], desc->color_format, &desc->blend)) {
			return NULL;
		}
		color_target_count = 1;
	} else {
		color_target_count = 0;
	}

	depth_format = Aeron_ToSdlTextureFormat(desc->depth_format);
	if (desc->depth_format != AERON_TEXTURE_FORMAT_UNKNOWN && depth_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return NULL;
	}

	vertex_buffers = NULL;
	attributes     = NULL;
	if (desc->vertex_buffer_count > 0) {
		if (!desc->vertex_buffers) {
			return NULL;
		}
		vertex_buffers =
			(SDL_GPUVertexBufferDescription*)SDL_calloc(desc->vertex_buffer_count, sizeof(*vertex_buffers));
		if (!vertex_buffers) {
			return NULL;
		}
		for (i = 0; i < desc->vertex_buffer_count; ++i) {
			vertex_buffers[i].slot       = desc->vertex_buffers[i].slot;
			vertex_buffers[i].pitch      = desc->vertex_buffers[i].stride;
			vertex_buffers[i].input_rate = desc->vertex_buffers[i].per_instance
											   ? SDL_GPU_VERTEXINPUTRATE_INSTANCE
											   : SDL_GPU_VERTEXINPUTRATE_VERTEX;
		}
	}

	if (desc->attribute_count > 0) {
		if (!desc->attributes) {
			SDL_free(vertex_buffers);
			return NULL;
		}
		attributes = (SDL_GPUVertexAttribute*)SDL_calloc(desc->attribute_count, sizeof(*attributes));
		if (!attributes) {
			SDL_free(vertex_buffers);
			return NULL;
		}
		for (i = 0; i < desc->attribute_count; ++i) {
			attributes[i].location    = desc->attributes[i].location;
			attributes[i].buffer_slot = desc->attributes[i].buffer_slot;
			attributes[i].format      = Aeron_ToSdlVertexFormat(desc->attributes[i].format);
			attributes[i].offset      = desc->attributes[i].offset;
			if (attributes[i].format == SDL_GPU_VERTEXELEMENTFORMAT_INVALID) {
				SDL_free(attributes);
				SDL_free(vertex_buffers);
				return NULL;
			}
		}
	}

	pipeline = (AeronGraphicsPipeline*)SDL_calloc(1, sizeof(*pipeline));
	if (!pipeline) {
		SDL_free(attributes);
		SDL_free(vertex_buffers);
		return NULL;
	}

	SDL_zero(pipeline_info);
	pipeline_info.vertex_shader                                 = desc->vertex_shader->shader;
	pipeline_info.fragment_shader                               = desc->fragment_shader->shader;
	pipeline_info.vertex_input_state.vertex_buffer_descriptions = vertex_buffers;
	pipeline_info.vertex_input_state.num_vertex_buffers         = desc->vertex_buffer_count;
	pipeline_info.vertex_input_state.vertex_attributes          = attributes;
	pipeline_info.vertex_input_state.num_vertex_attributes      = desc->attribute_count;
	pipeline_info.primitive_type                     = Aeron_ToSdlPrimitiveType(desc->primitive_type);
	pipeline_info.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
	pipeline_info.rasterizer_state.cull_mode         = Aeron_ToSdlCullMode(desc->cull_mode);
	pipeline_info.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	pipeline_info.rasterizer_state.enable_depth_bias = desc->rasterizer.depth_bias != 0;
	pipeline_info.rasterizer_state.depth_bias_constant_factor = desc->rasterizer.depth_bias_constant_factor;
	pipeline_info.rasterizer_state.depth_bias_clamp           = desc->rasterizer.depth_bias_clamp;
	pipeline_info.rasterizer_state.depth_bias_slope_factor    = desc->rasterizer.depth_bias_slope_factor;
	pipeline_info.rasterizer_state.enable_depth_clip          = true;
	pipeline_info.multisample_state.sample_count              = Aeron_ToSdlSampleCount(sample_count);
	pipeline_info.depth_stencil_state.enable_depth_test       = desc->depth.depth_test != 0;
	pipeline_info.depth_stencil_state.enable_depth_write      = desc->depth.depth_write != 0;
	pipeline_info.depth_stencil_state.compare_op              = Aeron_ToSdlCompareOp(desc->depth.compare);
	pipeline_info.target_info.color_target_descriptions       = color_target_count ? color_targets : NULL;
	pipeline_info.target_info.num_color_targets               = color_target_count;
	if (desc->depth_format != AERON_TEXTURE_FORMAT_UNKNOWN) {
		pipeline_info.target_info.has_depth_stencil_target = true;
		pipeline_info.target_info.depth_stencil_format     = depth_format;
	}

	pipeline->pipeline = SDL_CreateGPUGraphicsPipeline(g_aeron.gpu_device, &pipeline_info);
	SDL_free(attributes);
	SDL_free(vertex_buffers);
	if (!pipeline->pipeline) {
		Aeron_SetRenderError("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
		SDL_free(pipeline);
		return NULL;
	}

	return pipeline;
}

void Aeron_DestroyGraphicsPipeline(AeronGraphicsPipeline* pipeline) {
	if (!pipeline) {
		return;
	}

	if (pipeline->pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(g_aeron.gpu_device, pipeline->pipeline);
		pipeline->pipeline = NULL;
	}

	SDL_free(pipeline);
}

AeronRenderTarget* Aeron_CreateRenderTarget(const AeronRenderTargetDesc* desc) {
	SDL_GPUTextureCreateInfo texture_info;
	SDL_GPUTextureFormat     format;
	AeronRenderTarget*       target;
	AeronSampleCount         sample_count;

	if (!g_aeron.gpu_device || !desc || desc->width <= 0 || desc->height <= 0 ||
		!Aeron_IsColorTextureFormat(desc->format)) {
		Aeron_SetRenderError("Invalid render-target description");
		return NULL;
	}
	sample_count = Aeron_DescSampleCount(desc->sample_count);
	if (!Aeron_IsValidSampleCount(sample_count) ||
		(sample_count != AERON_SAMPLE_COUNT_1 && desc->usage != 0)) {
		Aeron_SetRenderError("Invalid render-target sample count or multisample usage");
		return NULL;
	}

	format = Aeron_ToSdlTextureFormat(desc->format);
	if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		Aeron_SetRenderError("Unsupported render-target texture format %d", desc->format);
		return NULL;
	}

	target = (AeronRenderTarget*)SDL_calloc(1, sizeof(*target));
	if (!target) {
		Aeron_SetRenderError("Could not allocate the %dx%d render-target wrapper", desc->width, desc->height);
		return NULL;
	}

	SDL_zero(texture_info);
	texture_info.type   = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format = format;
	texture_info.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
						  (sample_count == AERON_SAMPLE_COUNT_1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0) |
						  Aeron_ToSdlTextureUsage(desc->usage);
	texture_info.width  = (uint32_t)desc->width;
	texture_info.height = (uint32_t)desc->height;
	texture_info.layer_count_or_depth = 1;
	texture_info.num_levels           = 1;
	texture_info.sample_count         = Aeron_ToSdlSampleCount(sample_count);

	SDL_ClearError();
	target->color.texture = SDL_CreateGPUTexture(g_aeron.gpu_device, &texture_info);
	if (!target->color.texture) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_CreateGPUTexture failed for %dx%d render target '%s': %s", desc->width,
							 desc->height, desc->debug_name ? desc->debug_name : "<unnamed>",
							 error && error[0] ? error : "<no SDL error provided>");
		SDL_free(target);
		return NULL;
	}
	AeronGpuDebug_NameTexture(g_aeron.gpu_device, target->color.texture, desc->debug_name);

	target->color.width        = desc->width;
	target->color.height       = desc->height;
	target->color.mip_count    = 1;
	target->color.format       = desc->format;
	target->color.usage        = AERON_TEXTURE_USAGE_COLOR_TARGET | desc->usage |
								 (sample_count == AERON_SAMPLE_COUNT_1 ? AERON_TEXTURE_USAGE_SAMPLED : 0);
	target->color.sample_count = sample_count;
	return target;
}

void Aeron_DestroyRenderTarget(AeronRenderTarget* target) {
	int i;

	if (!target) {
		return;
	}

	/* Drop any pending present submission that references this target's texture, so a
	 * deferred Aeron_Present issued after the target is destroyed does not read freed
	 * memory. This happens on the frontend->flight handoff: the frontend submits its
	 * render-surface texture layer during the tick, then releases the surface (freeing
	 * this target) before the shell's Aeron_Present runs. */
	for (i = 0; i < g_aeron.render_layer_count; ++i) {
		AeronRenderLayer* layer = &g_aeron.render_layers[i];
		if (layer->kind == AERON_RENDER_LAYER_TEXTURE && layer->u.texture.texture == &target->color) {
			layer->kind = AERON_RENDER_LAYER_NONE;
		}
	}

	if (target->color.texture) {
		SDL_ReleaseGPUTexture(g_aeron.gpu_device, target->color.texture);
		target->color.texture = NULL;
	}

	SDL_free(target);
}

AeronTexture* Aeron_RenderTargetGetTexture(AeronRenderTarget* target) {
	if (!target) {
		return NULL;
	}

	return &target->color;
}

int Aeron_ReadRenderTargetPixels(AeronRenderTarget* target, void* dst, int pitch, AeronPixelFormat format) {
	SDL_GPUTransferBufferCreateInfo transfer_info;
	SDL_GPUTransferBuffer*          transfer;
	SDL_GPUCommandBuffer*           command_buffer;
	SDL_GPUCopyPass*                copy_pass;
	SDL_GPUTextureRegion            source;
	SDL_GPUTextureTransferInfo      destination;
	SDL_GPUFence*                   fence;
	const uint8_t*                  mapped;
	uint64_t                        transfer_size;
	int                             width;
	int                             height;
	int                             ri;
	int                             gi;
	int                             bi;
	int                             y;

	if (!g_aeron.gpu_device || !target || !target->color.texture ||
		target->color.sample_count != AERON_SAMPLE_COUNT_1 || !dst || pitch <= 0) {
		Aeron_SetRenderError("Invalid render-target readback description");
		return 0;
	}
	/* Only the 16-bit flight/frontend staging formats are supported; the recovered
	   surfaces the render target backs are all 16bpp (RGB565/RGB555). */
	if (format != AERON_PIXEL_FORMAT_RGB565 && format != AERON_PIXEL_FORMAT_RGB555) {
		Aeron_SetRenderError("Unsupported destination format for render-target readback");
		return 0;
	}
	width  = target->color.width;
	height = target->color.height;
	if (width <= 0 || height <= 0 || pitch < width * 2) {
		Aeron_SetRenderError("Invalid dimensions or pitch for render-target readback");
		return 0;
	}

	/* Byte offsets of the R/G/B channels in the downloaded RGBA8 rows. */
	switch (target->color.format) {
		case AERON_TEXTURE_FORMAT_RGBA8_UNORM:
		case AERON_TEXTURE_FORMAT_RGBA8_SRGB:
			ri = 0;
			gi = 1;
			bi = 2;
			break;
		case AERON_TEXTURE_FORMAT_BGRA8_UNORM:
		case AERON_TEXTURE_FORMAT_BGRA8_SRGB:
			ri = 2;
			gi = 1;
			bi = 0;
			break;
		default:
			Aeron_SetRenderError("Unsupported source format for render-target readback");
			return 0;
	}

	transfer_size = (uint64_t)width * (uint64_t)height * 4u;
	if (transfer_size > UINT32_MAX) {
		Aeron_SetRenderError("Render-target readback exceeds the transfer-buffer size limit");
		return 0;
	}

	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size  = (uint32_t)transfer_size;

	transfer = SDL_CreateGPUTransferBuffer(g_aeron.gpu_device, &transfer_info);
	if (!transfer) {
		Aeron_SetRenderError("SDL_CreateGPUTransferBuffer failed for render-target readback: %s",
							 SDL_GetError());
		return 0;
	}

	SDL_ClearError();
	command_buffer = SDL_AcquireGPUCommandBuffer(g_aeron.gpu_device);
	if (!command_buffer) {
		Aeron_SetRenderError("SDL_AcquireGPUCommandBuffer failed for render-target readback: %s",
							 SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, transfer);
		return 0;
	}

	SDL_ClearError();
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	if (!copy_pass) {
		Aeron_SetRenderError("SDL_BeginGPUCopyPass failed for render-target readback: %s",
							 SDL_GetError());
		SDL_CancelGPUCommandBuffer(command_buffer);
		SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, transfer);
		return 0;
	}

	SDL_zero(source);
	source.texture = target->color.texture;
	source.w       = (uint32_t)width;
	source.h       = (uint32_t)height;
	source.d       = 1;

	SDL_zero(destination);
	destination.transfer_buffer = transfer;
	destination.pixels_per_row  = (uint32_t)width;
	destination.rows_per_layer  = (uint32_t)height;

	SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
	SDL_EndGPUCopyPass(copy_pass);

	/* The CPU reads the pixels immediately, so wait for the GPU to finish the copy. */
	SDL_ClearError();
	fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
	if (!fence) {
		Aeron_SetRenderError("SDL_SubmitGPUCommandBufferAndAcquireFence failed for readback: %s",
							 SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, transfer);
		return 0;
	}
	SDL_WaitForGPUFences(g_aeron.gpu_device, true, &fence, 1);
	SDL_ReleaseGPUFence(g_aeron.gpu_device, fence);

	mapped = (const uint8_t*)SDL_MapGPUTransferBuffer(g_aeron.gpu_device, transfer, false);
	if (!mapped) {
		Aeron_SetRenderError("SDL_MapGPUTransferBuffer failed for render-target readback: %s",
							 SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, transfer);
		return 0;
	}

	for (y = 0; y < height; ++y) {
		const uint8_t* src_row = mapped + (size_t)y * (size_t)width * 4u;
		uint16_t*      dst_row = (uint16_t*)((uint8_t*)dst + (size_t)y * (size_t)pitch);
		int            x;

		if (format == AERON_PIXEL_FORMAT_RGB565) {
			for (x = 0; x < width; ++x) {
				const uint8_t* p = src_row + (size_t)x * 4u;
				dst_row[x]       = (uint16_t)(((p[ri] >> 3) << 11) | ((p[gi] >> 2) << 5) | (p[bi] >> 3));
			}
		} else { /* RGB555 */
			for (x = 0; x < width; ++x) {
				const uint8_t* p = src_row + (size_t)x * 4u;
				dst_row[x]       = (uint16_t)(((p[ri] >> 3) << 10) | ((p[gi] >> 3) << 5) | (p[bi] >> 3));
			}
		}
	}

	SDL_UnmapGPUTransferBuffer(g_aeron.gpu_device, transfer);
	SDL_ReleaseGPUTransferBuffer(g_aeron.gpu_device, transfer);
	return 1;
}

AeronDepthTarget* Aeron_CreateDepthTarget(const AeronDepthTargetDesc* desc) {
	SDL_GPUTextureCreateInfo texture_info;
	SDL_GPUTextureFormat     format;
	AeronDepthTarget*        target;
	AeronSampleCount         sample_count;

	if (!g_aeron.gpu_device || !desc || desc->width <= 0 || desc->height <= 0 ||
		!Aeron_IsDepthTextureFormat(desc->format)) {
		Aeron_SetRenderError("Invalid depth-target description");
		return NULL;
	}
	sample_count = Aeron_DescSampleCount(desc->sample_count);
	if (!Aeron_IsValidSampleCount(sample_count) || (sample_count != AERON_SAMPLE_COUNT_1 && desc->sampled)) {
		Aeron_SetRenderError("Invalid depth-target sample count or sampled multisample request");
		return NULL;
	}

	format = Aeron_ToSdlTextureFormat(desc->format);
	if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		Aeron_SetRenderError("Unsupported depth-target texture format %d", desc->format);
		return NULL;
	}

	target = (AeronDepthTarget*)SDL_calloc(1, sizeof(*target));
	if (!target) {
		Aeron_SetRenderError("Could not allocate the %dx%d depth-target wrapper", desc->width, desc->height);
		return NULL;
	}

	SDL_zero(texture_info);
	texture_info.type   = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format = format;
	texture_info.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	if (desc->sampled) {
		texture_info.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
	}
	texture_info.width                = (uint32_t)desc->width;
	texture_info.height               = (uint32_t)desc->height;
	texture_info.layer_count_or_depth = 1;
	texture_info.num_levels           = 1;
	texture_info.sample_count         = Aeron_ToSdlSampleCount(sample_count);

	SDL_ClearError();
	target->depth.texture = SDL_CreateGPUTexture(g_aeron.gpu_device, &texture_info);
	if (!target->depth.texture) {
		const char* error = SDL_GetError();
		Aeron_SetRenderError("SDL_CreateGPUTexture failed for %dx%d depth target '%s': %s", desc->width,
							 desc->height, desc->debug_name ? desc->debug_name : "<unnamed>",
							 error && error[0] ? error : "<no SDL error provided>");
		SDL_free(target);
		return NULL;
	}
	AeronGpuDebug_NameTexture(g_aeron.gpu_device, target->depth.texture, desc->debug_name);

	target->depth.width     = desc->width;
	target->depth.height    = desc->height;
	target->depth.mip_count = 1;
	target->depth.format    = desc->format;
	target->depth.usage =
		AERON_TEXTURE_USAGE_DEPTH_TARGET | (desc->sampled ? AERON_TEXTURE_USAGE_SAMPLED : 0u);
	target->depth.sample_count = sample_count;
	return target;
}

void Aeron_DestroyDepthTarget(AeronDepthTarget* target) {
	if (!target) {
		return;
	}

	if (target->depth.texture) {
		SDL_ReleaseGPUTexture(g_aeron.gpu_device, target->depth.texture);
		target->depth.texture = NULL;
	}

	SDL_free(target);
}

AeronTexture* Aeron_DepthTargetGetTexture(AeronDepthTarget* target) {
	if (!target || (target->depth.usage & AERON_TEXTURE_USAGE_SAMPLED) == 0) {
		return NULL;
	}

	return &target->depth;
}

AeronRenderPass* Aeron_BeginRenderPass(const AeronRenderPassDesc* desc) {
	SDL_GPUColorTargetInfo               color_targets[AERON_MAX_COLOR_TARGETS];
	SDL_GPUDepthStencilTargetInfo        depth_target;
	SDL_GPUViewport                      viewport;
	SDL_Rect                             scissor;
	AeronRenderPass*                     pass;
	const SDL_GPUDepthStencilTargetInfo* depth_target_ptr;
	uint32_t                             color_target_count;
	uint32_t                             t;
	AeronSampleCount                     pass_sample_count;

	const int has_color = desc && desc->color_target && desc->color_target->color.texture;
	const int has_depth = desc && desc->depth_target && desc->depth_target->depth.texture;
	if (!g_aeron.gpu_device || !desc || !desc->command_buffer || (!has_color && !has_depth)) {
		if (desc && desc->command_buffer) {
			Aeron_CommandBufferFail(desc->command_buffer, "Invalid render-pass description or attachments");
		} else {
			Aeron_SetRenderError("Invalid render-pass description or attachments");
		}
		return NULL;
	}
	if (desc->discard_color && desc->clear_color) {
		Aeron_CommandBufferFail(desc->command_buffer,
							   "Render pass cannot clear and discard its color attachment");
		return NULL;
	}
	if ((!has_color && desc->extra_color_target_count > 0) ||
			(desc->extra_color_target_count > 0 &&
			 (!desc->extra_color_targets || desc->extra_color_target_count >= AERON_MAX_COLOR_TARGETS))) {
		Aeron_CommandBufferFail(desc->command_buffer, "Invalid extra color attachments for render pass");
		return NULL;
	}
	if (!has_color && desc->color_resolve_target) {
		Aeron_CommandBufferFail(desc->command_buffer, "Depth-only render pass cannot have a color resolve target");
		return NULL;
	}

	pass_sample_count = AERON_SAMPLE_COUNT_1;
	if (has_color) {
		pass_sample_count = desc->color_target->color.sample_count;
	} else if (has_depth) {
		pass_sample_count = desc->depth_target->depth.sample_count;
	}
	if (!Aeron_IsValidSampleCount(pass_sample_count)) {
		Aeron_CommandBufferFail(desc->command_buffer, "Invalid render-pass sample count");
		return NULL;
	}
	if (has_depth && desc->depth_target->depth.sample_count != pass_sample_count) {
		Aeron_CommandBufferFail(desc->command_buffer, "Render-pass color/depth sample counts do not match");
		return NULL;
	}

	pass = (AeronRenderPass*)SDL_calloc(1, sizeof(*pass));
	if (!pass) {
		Aeron_CommandBufferFail(desc->command_buffer, "Could not allocate the render-pass wrapper");
		return NULL;
	}
	pass->output_rgb_scale = 1.0f;
	pass->sample_count     = pass_sample_count;
	pass->depth_format     = has_depth ? desc->depth_target->depth.format : AERON_TEXTURE_FORMAT_UNKNOWN;

	if (!desc->command_buffer->command_buffer || desc->command_buffer->failed ||
		desc->command_buffer->compute_pass_active || desc->command_buffer->render_pass_active) {
		Aeron_CommandBufferFail(desc->command_buffer,
							   "Command buffer cannot begin the requested render pass");
		SDL_free(pass);
		return NULL;
	}
	pass->owner          = desc->command_buffer;
	pass->command_buffer = desc->command_buffer->command_buffer;

	color_target_count = has_color ? 1 + desc->extra_color_target_count : 0;
	for (t = 0; t < color_target_count; ++t) {
		AeronRenderTarget* rt         = t == 0 ? desc->color_target : desc->extra_color_targets[t - 1];
		AeronRenderTarget* resolve_rt = t == 0 ? desc->color_resolve_target : NULL;

		if (!rt || !rt->color.texture || rt->color.sample_count != pass_sample_count ||
			(resolve_rt &&
			 (!resolve_rt->color.texture || pass_sample_count == AERON_SAMPLE_COUNT_1 ||
			  resolve_rt->color.sample_count != AERON_SAMPLE_COUNT_1 ||
			  resolve_rt->color.width != rt->color.width || resolve_rt->color.height != rt->color.height ||
			  resolve_rt->color.format != rt->color.format))) {
			Aeron_CommandBufferFail(pass->owner, "Invalid render-pass attachment or resolve target");
			SDL_free(pass);
			return NULL;
		}
		SDL_zero(color_targets[t]);
		color_targets[t].texture     = rt->color.texture;
		color_targets[t].clear_color = (SDL_FColor) { desc->clear_color_rgba[0], desc->clear_color_rgba[1],
													  desc->clear_color_rgba[2], desc->clear_color_rgba[3] };
		color_targets[t].load_op     = desc->clear_color     ? SDL_GPU_LOADOP_CLEAR
									   : desc->discard_color ? SDL_GPU_LOADOP_DONT_CARE
															 : SDL_GPU_LOADOP_LOAD;
		color_targets[t].store_op    = resolve_rt ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
		color_targets[t].cycle       = desc->clear_color || desc->discard_color;
		color_targets[t].resolve_texture       = resolve_rt ? resolve_rt->color.texture : NULL;
		color_targets[t].cycle_resolve_texture = resolve_rt != NULL;
	}

	depth_target_ptr = NULL;
	if (desc->depth_target && desc->depth_target->depth.texture) {
		SDL_zero(depth_target);
		depth_target.texture     = desc->depth_target->depth.texture;
		depth_target.clear_depth = desc->clear_depth_value;
		depth_target.load_op     = desc->clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		depth_target.store_op    = desc->discard_depth ? SDL_GPU_STOREOP_DONT_CARE : SDL_GPU_STOREOP_STORE;
		depth_target.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
		depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth_target.cycle            = desc->clear_depth || desc->discard_depth;
		depth_target_ptr              = &depth_target;
	}

	AeronGpuDebug_Push(pass->command_buffer, desc->debug_label);
	pass->debug_group_open = desc->debug_label != NULL;
	pass->render_pass =
		SDL_BeginGPURenderPass(pass->command_buffer, color_target_count ? color_targets : NULL,
							   color_target_count, depth_target_ptr);
	if (!pass->render_pass) {
		const char* error = SDL_GetError();
		if (pass->debug_group_open) {
			AeronGpuDebug_Pop(pass->command_buffer);
		}
		Aeron_CommandBufferFail(pass->owner, "SDL_BeginGPURenderPass failed: %s",
							   error && error[0] ? error : "<no SDL error provided>");
		SDL_free(pass);
		return NULL;
	}
	pass->owner->render_pass_active = 1;

	if (desc->viewport.width > 0 && desc->viewport.height > 0) {
		viewport.x         = (float)desc->viewport.x;
		viewport.y         = (float)desc->viewport.y;
		viewport.w         = (float)desc->viewport.width;
		viewport.h         = (float)desc->viewport.height;
		viewport.min_depth = 0.0f;
		viewport.max_depth = 1.0f;
		SDL_SetGPUViewport(pass->render_pass, &viewport);
	}

	if (desc->scissor.width > 0 && desc->scissor.height > 0) {
		scissor.x = desc->scissor.x;
		scissor.y = desc->scissor.y;
		scissor.w = desc->scissor.width;
		scissor.h = desc->scissor.height;
		SDL_SetGPUScissor(pass->render_pass, &scissor);
	}

	return pass;
}

void Aeron_EndRenderPass(AeronRenderPass* pass) {
	if (!pass) {
		return;
	}

	if (pass->render_pass) {
		SDL_EndGPURenderPass(pass->render_pass);
		pass->render_pass = NULL;
	}
	if (pass->debug_group_open) {
		AeronGpuDebug_Pop(pass->command_buffer);
		pass->debug_group_open = 0;
	}
	pass->owner->render_pass_active = 0;

	SDL_free(pass);
}

AeronSampleCount Aeron_RenderPassGetSampleCount(const AeronRenderPass* pass) {
	return pass && Aeron_IsValidSampleCount(pass->sample_count) ? pass->sample_count : AERON_SAMPLE_COUNT_1;
}

AeronTextureFormat Aeron_RenderPassGetDepthFormat(const AeronRenderPass* pass) {
	return pass ? pass->depth_format : AERON_TEXTURE_FORMAT_UNKNOWN;
}

AeronComputePass* Aeron_BeginComputePass(const AeronComputePassDesc* desc) {
	SDL_GPUStorageTextureReadWriteBinding textures[AERON_MAX_COMPUTE_WRITE_TEXTURES];
	SDL_GPUStorageBufferReadWriteBinding  buffers[AERON_MAX_COMPUTE_WRITE_BUFFERS];
	AeronCommandBuffer*                   command_buffer;
	AeronComputePass*                     pass;
	uint32_t                              i;

	if (!g_aeron.gpu_device || !desc || !desc->command_buffer || !desc->command_buffer->command_buffer ||
		desc->command_buffer->failed || desc->command_buffer->render_pass_active ||
		(desc->write_texture_count > 0 && !desc->write_textures) ||
		(desc->write_buffer_count > 0 && !desc->write_buffers) ||
		desc->write_texture_count > AERON_MAX_COMPUTE_WRITE_TEXTURES ||
		desc->write_buffer_count > AERON_MAX_COMPUTE_WRITE_BUFFERS) {
		return NULL;
	}
	command_buffer = desc->command_buffer;
	if (command_buffer->compute_pass_active) {
		Aeron_LogError("aeron", "Nested compute passes are not supported on one command buffer");
		return NULL;
	}

	for (i = 0; i < desc->write_texture_count; ++i) {
		AeronTexture* texture = desc->write_textures[i].texture;
		if (!texture || !texture->texture ||
			(texture->usage & (AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE |
							   AERON_TEXTURE_USAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE)) == 0 ||
			desc->write_textures[i].mip_level >= (uint32_t)texture->mip_count) {
			return NULL;
		}
		textures[i].texture   = texture->texture;
		textures[i].mip_level = desc->write_textures[i].mip_level;
		textures[i].layer     = desc->write_textures[i].layer;
		textures[i].cycle     = desc->write_textures[i].cycle != 0;
	}
	for (i = 0; i < desc->write_buffer_count; ++i) {
		AeronBuffer* buffer = desc->write_buffers[i].buffer;
		if (!buffer || !buffer->buffer || (buffer->usage & AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE) == 0) {
			return NULL;
		}
		buffers[i].buffer = buffer->buffer;
		buffers[i].cycle  = desc->write_buffers[i].cycle != 0;
	}

	pass = &command_buffer->active_compute_pass;
	SDL_zero(*pass);
	pass->external       = command_buffer;
	pass->command_buffer = command_buffer->command_buffer;
	AeronGpuDebug_Push(pass->command_buffer, desc->debug_label);
	pass->debug_group_open = desc->debug_label != NULL;
	pass->compute_pass     = SDL_BeginGPUComputePass(
		pass->command_buffer, desc->write_texture_count ? textures : NULL, desc->write_texture_count,
		desc->write_buffer_count ? buffers : NULL, desc->write_buffer_count);
	if (!pass->compute_pass) {
		Aeron_LogError("aeron", "SDL_BeginGPUComputePass failed: %s", SDL_GetError());
		if (pass->debug_group_open) {
			AeronGpuDebug_Pop(pass->command_buffer);
		}
		SDL_zero(*pass);
		Aeron_CommandBufferMarkFailed(command_buffer);
		return NULL;
	}
	command_buffer->compute_pass_active = 1;
	return pass;
}

void Aeron_EndComputePass(AeronComputePass* pass) {
	AeronCommandBuffer* command_buffer;

	if (!pass) {
		return;
	}
	command_buffer = pass->external;
	if (!command_buffer || !command_buffer->compute_pass_active ||
		pass != &command_buffer->active_compute_pass) {
		return;
	}
	if (pass->compute_pass) {
		SDL_EndGPUComputePass(pass->compute_pass);
	}
	if (pass->debug_group_open) {
		AeronGpuDebug_Pop(pass->command_buffer);
	}
	SDL_zero(*pass);
	command_buffer->compute_pass_active = 0;
}

void Aeron_BindComputePipeline(AeronComputePass* pass, AeronComputePipeline* pipeline) {
	if (!pass || !pass->compute_pass || !pipeline || !pipeline->pipeline) {
		return;
	}
	SDL_BindGPUComputePipeline(pass->compute_pass, pipeline->pipeline);
}

void Aeron_BindComputeTextureSampler(AeronComputePass* pass, uint32_t slot, AeronTexture* texture,
									 AeronSampler* sampler) {
	SDL_GPUTextureSamplerBinding binding;

	if (!pass || !pass->compute_pass || !texture || !texture->texture || !sampler || !sampler->sampler ||
		(texture->usage & AERON_TEXTURE_USAGE_SAMPLED) == 0) {
		return;
	}
	binding.texture = texture->texture;
	binding.sampler = sampler->sampler;
	SDL_BindGPUComputeSamplers(pass->compute_pass, slot, &binding, 1);
}

void Aeron_BindComputeStorageTexture(AeronComputePass* pass, uint32_t slot, AeronTexture* texture) {
	if (!pass || !pass->compute_pass || !texture || !texture->texture ||
		(texture->usage & AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ) == 0) {
		return;
	}
	SDL_BindGPUComputeStorageTextures(pass->compute_pass, slot, &texture->texture, 1);
}

void Aeron_BindComputeStorageBuffer(AeronComputePass* pass, uint32_t slot, AeronBuffer* buffer) {
	if (!pass || !pass->compute_pass || !buffer || !buffer->buffer ||
		(buffer->usage & AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ) == 0) {
		return;
	}
	SDL_BindGPUComputeStorageBuffers(pass->compute_pass, slot, &buffer->buffer, 1);
	g_aeron.render_data_stats.storage_buffer_bind_count++;
}

void Aeron_BindComputeUniformData(AeronComputePass* pass, uint32_t slot, const void* data, uint32_t size) {
	if (!pass || !pass->command_buffer || !data || size == 0) {
		return;
	}
	if (size > AERON_MAX_UNIFORM_DATA_SIZE) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER,
					 "Aeron_BindComputeUniformData rejected slot %u: %u bytes exceeds the %u-byte "
					 "uniform limit; use a storage buffer",
					 slot, size, AERON_MAX_UNIFORM_DATA_SIZE);
		return;
	}
	SDL_PushGPUComputeUniformData(pass->command_buffer, slot, data, size);
	Aeron_RecordUniformPush(size);
}

void Aeron_DispatchCompute(AeronComputePass* pass, uint32_t group_count_x, uint32_t group_count_y,
						   uint32_t group_count_z) {
	if (!pass || !pass->compute_pass || group_count_x == 0 || group_count_y == 0 || group_count_z == 0) {
		return;
	}
	SDL_DispatchGPUCompute(pass->compute_pass, group_count_x, group_count_y, group_count_z);
}

int Aeron_CopyTextureCmd(AeronCommandBuffer* command_buffer, const AeronTextureCopyDesc* desc) {
	SDL_GPUTextureLocation source;
	SDL_GPUTextureLocation destination;
	SDL_GPUCopyPass*       copy_pass;

	if (!command_buffer || !command_buffer->command_buffer || command_buffer->failed ||
		command_buffer->compute_pass_active || command_buffer->render_pass_active || !desc || !desc->source ||
		!desc->source->texture || !desc->destination || !desc->destination->texture ||
		desc->source_mip_level >= (uint32_t)desc->source->mip_count ||
		desc->destination_mip_level >= (uint32_t)desc->destination->mip_count || desc->width == 0 ||
		desc->height == 0) {
		return 0;
	}

	SDL_zero(source);
	source.texture   = desc->source->texture;
	source.mip_level = desc->source_mip_level;
	source.layer     = desc->source_layer;
	source.x         = desc->source_x;
	source.y         = desc->source_y;
	SDL_zero(destination);
	destination.texture   = desc->destination->texture;
	destination.mip_level = desc->destination_mip_level;
	destination.layer     = desc->destination_layer;
	destination.x         = desc->destination_x;
	destination.y         = desc->destination_y;

	copy_pass = SDL_BeginGPUCopyPass(command_buffer->command_buffer);
	if (!copy_pass) {
		Aeron_LogError("aeron", "SDL_BeginGPUCopyPass failed for texture copy: %s", SDL_GetError());
		Aeron_CommandBufferMarkFailed(command_buffer);
		return 0;
	}
	SDL_CopyGPUTextureToTexture(copy_pass, &source, &destination, desc->width, desc->height, 1,
								desc->cycle != 0);
	SDL_EndGPUCopyPass(copy_pass);
	return 1;
}

void Aeron_BindGraphicsPipeline(AeronRenderPass* pass, AeronGraphicsPipeline* pipeline) {
	if (!pass || !pass->render_pass || !pipeline || !pipeline->pipeline) {
		return;
	}

	SDL_BindGPUGraphicsPipeline(pass->render_pass, pipeline->pipeline);
}

void Aeron_BindVertexBuffer(AeronRenderPass* pass, uint32_t slot, AeronBuffer* buffer, uint32_t offset) {
	SDL_GPUBufferBinding binding;

	if (!pass || !pass->render_pass || !buffer || !buffer->buffer || offset >= buffer->size ||
		(buffer->usage & AERON_BUFFER_USAGE_VERTEX) == 0) {
		return;
	}

	SDL_zero(binding);
	binding.buffer = buffer->buffer;
	binding.offset = offset;
	SDL_BindGPUVertexBuffers(pass->render_pass, slot, &binding, 1);
}

void Aeron_BindIndexBuffer(AeronRenderPass* pass, AeronBuffer* buffer, AeronIndexFormat format,
						   uint32_t offset) {
	SDL_GPUBufferBinding    binding;
	SDL_GPUIndexElementSize element_size;

	if (!pass || !pass->render_pass || !buffer || !buffer->buffer || offset >= buffer->size ||
		(buffer->usage & AERON_BUFFER_USAGE_INDEX) == 0) {
		return;
	}

	element_size =
		format == AERON_INDEX_FORMAT_UINT32 ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT;

	SDL_zero(binding);
	binding.buffer = buffer->buffer;
	binding.offset = offset;
	SDL_BindGPUIndexBuffer(pass->render_pass, &binding, element_size);
}

void Aeron_BindTextureSampler(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot,
							  AeronTexture* texture, AeronSampler* sampler) {
	SDL_GPUTextureSamplerBinding binding;

	if (!pass || !pass->render_pass || !texture || !texture->texture || !sampler || !sampler->sampler ||
		(texture->usage & AERON_TEXTURE_USAGE_SAMPLED) == 0) {
		return;
	}

	SDL_zero(binding);
	binding.texture = texture->texture;
	binding.sampler = sampler->sampler;

	if (stage == AERON_SHADER_STAGE_VERTEX) {
		SDL_BindGPUVertexSamplers(pass->render_pass, slot, &binding, 1);
	} else {
		SDL_BindGPUFragmentSamplers(pass->render_pass, slot, &binding, 1);
	}
}

void Aeron_BindStorageBuffer(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot,
							 AeronBuffer* buffer) {
	if (!pass || !pass->render_pass || !buffer || !buffer->buffer ||
		(buffer->usage & AERON_BUFFER_USAGE_STORAGE) == 0) {
		return;
	}

	if (stage == AERON_SHADER_STAGE_VERTEX) {
		SDL_BindGPUVertexStorageBuffers(pass->render_pass, slot, &buffer->buffer, 1);
	} else {
		SDL_BindGPUFragmentStorageBuffers(pass->render_pass, slot, &buffer->buffer, 1);
	}
	g_aeron.render_data_stats.storage_buffer_bind_count++;
}

int Aeron_TextureFormatSupported(AeronTextureFormat format, uint32_t usage) {
	SDL_GPUTextureFormat     sdl_format;
	SDL_GPUTextureUsageFlags sdl_usage;

	if (!g_aeron.gpu_device) {
		return 0;
	}

	sdl_format = Aeron_ToSdlTextureFormat(format);
	sdl_usage  = Aeron_ToSdlTextureUsage(usage);
	if (sdl_format == SDL_GPU_TEXTUREFORMAT_INVALID || sdl_usage == 0) {
		return 0;
	}

	return SDL_GPUTextureSupportsFormat(g_aeron.gpu_device, sdl_format, SDL_GPU_TEXTURETYPE_2D, sdl_usage)
			   ? 1
			   : 0;
}

int Aeron_TextureFormatSupportsSampleCount(AeronTextureFormat format, AeronSampleCount sample_count) {
	SDL_GPUTextureFormat sdl_format;

	if (!g_aeron.gpu_device || !Aeron_IsValidSampleCount(sample_count)) {
		return 0;
	}
	sdl_format = Aeron_ToSdlTextureFormat(format);
	if (sdl_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return 0;
	}
	return SDL_GPUTextureSupportsSampleCount(g_aeron.gpu_device, sdl_format,
											 Aeron_ToSdlSampleCount(sample_count))
			   ? 1
			   : 0;
}

uint32_t Aeron_BufferSize(const AeronBuffer* buffer) { return buffer ? buffer->size : 0; }

uint32_t Aeron_BufferUsageFlags(const AeronBuffer* buffer) { return buffer ? buffer->usage : 0; }

void Aeron_BindUniformData(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot, const void* data,
						   uint32_t size) {
	if (!pass || !pass->command_buffer || !data || size == 0) {
		return;
	}
	if (stage != AERON_SHADER_STAGE_VERTEX && stage != AERON_SHADER_STAGE_FRAGMENT) {
		return;
	}
	if (size > AERON_MAX_UNIFORM_DATA_SIZE) {
		const char* stage_name = stage == AERON_SHADER_STAGE_VERTEX ? "vertex" : "fragment";
		SDL_LogError(SDL_LOG_CATEGORY_RENDER,
					 "Aeron_BindUniformData rejected %s slot %u: %u bytes exceeds the %u-byte "
					 "uniform limit; use a storage buffer",
					 stage_name, slot, size, AERON_MAX_UNIFORM_DATA_SIZE);
		return;
	}

	if (stage == AERON_SHADER_STAGE_VERTEX) {
		SDL_PushGPUVertexUniformData(pass->command_buffer, slot, data, size);
	} else {
		SDL_PushGPUFragmentUniformData(pass->command_buffer, slot, data, size);
	}
	Aeron_RecordUniformPush(size);
}

void Aeron_GetRenderDataStats(AeronRenderDataStats* out_stats) {
	if (out_stats) {
		*out_stats = g_aeron.render_data_stats;
	}
}

void Aeron_SetViewport(AeronRenderPass* pass, const AeronRectI* rect) {
	SDL_GPUViewport viewport;

	if (!pass || !pass->render_pass || !rect || rect->width <= 0 || rect->height <= 0) {
		return;
	}

	viewport.x         = (float)rect->x;
	viewport.y         = (float)rect->y;
	viewport.w         = (float)rect->width;
	viewport.h         = (float)rect->height;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(pass->render_pass, &viewport);
}

void Aeron_SetScissor(AeronRenderPass* pass, const AeronRectI* rect) {
	SDL_Rect scissor;

	if (!pass || !pass->render_pass || !rect || rect->width <= 0 || rect->height <= 0) {
		return;
	}

	scissor.x = rect->x;
	scissor.y = rect->y;
	scissor.w = rect->width;
	scissor.h = rect->height;
	SDL_SetGPUScissor(pass->render_pass, &scissor);
}

void Aeron_Draw(AeronRenderPass* pass, uint32_t vertex_count, uint32_t first_vertex) {
	if (!pass || !pass->render_pass || vertex_count == 0) {
		return;
	}

	SDL_DrawGPUPrimitives(pass->render_pass, vertex_count, 1, first_vertex, 0);
}

void Aeron_DrawIndexed(AeronRenderPass* pass, uint32_t index_count, uint32_t first_index,
					   int32_t vertex_offset) {
	if (!pass || !pass->render_pass || index_count == 0) {
		return;
	}

	SDL_DrawGPUIndexedPrimitives(pass->render_pass, index_count, 1, first_index, vertex_offset, 0);
}

void Aeron_DrawInstanced(AeronRenderPass* pass, uint32_t vertex_count, uint32_t instance_count,
						 uint32_t first_vertex) {
	if (!pass || !pass->render_pass || vertex_count == 0 || instance_count == 0) {
		return;
	}

	SDL_DrawGPUPrimitives(pass->render_pass, vertex_count, instance_count, first_vertex, 0);
}

void Aeron_DrawIndexedInstanced(AeronRenderPass* pass, uint32_t index_count, uint32_t instance_count,
								uint32_t first_index, int32_t vertex_offset) {
	if (!pass || !pass->render_pass || index_count == 0 || instance_count == 0) {
		return;
	}

	SDL_DrawGPUIndexedPrimitives(pass->render_pass, index_count, instance_count, first_index, vertex_offset,
								 0);
}

int Aeron_TextureGetWidth(const AeronTexture* texture) {
	if (!texture) {
		return 0;
	}

	return texture->width;
}

int Aeron_TextureGetHeight(const AeronTexture* texture) {
	if (!texture) {
		return 0;
	}

	return texture->height;
}

AeronTextureFormat Aeron_TextureGetFormat(const AeronTexture* texture) {
	if (!texture) {
		return AERON_TEXTURE_FORMAT_UNKNOWN;
	}

	return texture->format;
}

int Aeron_TextureGetMipCount(const AeronTexture* texture) { return texture ? texture->mip_count : 0; }

uint32_t Aeron_TextureGetUsage(const AeronTexture* texture) { return texture ? texture->usage : 0; }

void Aeron_ClearRenderSubmissions(void) {
	g_aeron.render_submission_generation = g_aeron.render_submission_generation == UINT16_MAX
											   ? 1
											   : (uint16_t)(g_aeron.render_submission_generation + 1u);
	g_aeron.render_layer_count           = 0;
	g_aeron.pixel_layer_upload_count     = 0;
}
