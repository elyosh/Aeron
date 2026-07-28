#ifndef AERON_RENDER_H
#define AERON_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uniform pushes are reserved for small per-draw/per-dispatch values.
 * Larger or variable-length payloads must use storage buffers. */
#define AERON_MAX_UNIFORM_DATA_SIZE 1024u

/* Per-frame data-transfer counters reset by Aeron_BeginFrame. They describe
 * recorded work, including command buffers that have not yet been submitted. */
typedef struct AeronRenderDataStats {
	uint32_t max_uniform_bytes;
	uint32_t storage_buffer_bind_count;
	uint64_t total_uniform_bytes;
	uint64_t storage_upload_bytes;
	uint64_t upload_staged_bytes;
	uint64_t upload_reserved_bytes;
	uint32_t upload_chunk_count;
	uint32_t buffer_copy_count;
	uint32_t texture_copy_count;
	uint32_t upload_copy_pass_count;
	uint32_t upload_command_buffer_submission_count;
	uint32_t immediate_upload_submission_count;
	uint32_t largest_upload_bytes;
	uint32_t failed_command_buffer_count;
} AeronRenderDataStats;

/* One 8-bit-per-channel palette entry used by indexed pixel frames and surfaces. */
typedef struct AeronPaletteEntry {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} AeronPaletteEntry;

/* CPU-side pixel formats accepted by Aeron pixel-frame and surface APIs.
 * 32-bit formats are byte-order formats in memory: RGBA8888 means R, G, B, A bytes.
 */
typedef enum AeronPixelFormat {
	AERON_PIXEL_FORMAT_UNKNOWN,
	AERON_PIXEL_FORMAT_INDEX8,
	AERON_PIXEL_FORMAT_RGB555,
	AERON_PIXEL_FORMAT_RGB565,
	AERON_PIXEL_FORMAT_XRGB8888,
	AERON_PIXEL_FORMAT_ARGB8888,
	AERON_PIXEL_FORMAT_RGBA8888,
	AERON_PIXEL_FORMAT_BGRA8888
} AeronPixelFormat;

/* Describes whether incoming color values are already linear or sRGB encoded.
 * LINEAR_DISPLAY marks linear values that are the decoded form of
 * display-referred SDR content (sRGB art composed through hardware _SRGB
 * views): presentation may remap their display gamma for HDR output — see
 * Aeron_SetOutputSdrContentGamma — which LINEAR_SRGB (scene-referred light)
 * must never receive. */
typedef enum AeronColorSpace {
	AERON_COLOR_SPACE_SRGB,
	AERON_COLOR_SPACE_LINEAR_SRGB,
	AERON_COLOR_SPACE_LINEAR_DISPLAY
} AeronColorSpace;

/* GPU texture formats used by Aeron-owned textures and render targets.
 * Availability of the non-8-bit and block-compressed formats depends on the
 * backend; query Aeron_TextureFormatSupported before relying on one. */
typedef enum AeronTextureFormat {
	AERON_TEXTURE_FORMAT_UNKNOWN,
	AERON_TEXTURE_FORMAT_RGBA8_UNORM,
	AERON_TEXTURE_FORMAT_RGBA8_SRGB,
	AERON_TEXTURE_FORMAT_BGRA8_UNORM,
	AERON_TEXTURE_FORMAT_BGRA8_SRGB,
	AERON_TEXTURE_FORMAT_D16_UNORM,
	AERON_TEXTURE_FORMAT_D24_UNORM,
	AERON_TEXTURE_FORMAT_D32_FLOAT,
	AERON_TEXTURE_FORMAT_R8_UNORM,
	AERON_TEXTURE_FORMAT_R8G8_UNORM,
	AERON_TEXTURE_FORMAT_R16_SNORM,
	AERON_TEXTURE_FORMAT_R16_FLOAT,
	AERON_TEXTURE_FORMAT_R32_FLOAT,
	AERON_TEXTURE_FORMAT_R32_UINT,
	AERON_TEXTURE_FORMAT_R16G16_FLOAT,
	AERON_TEXTURE_FORMAT_R16G16_SNORM,
	AERON_TEXTURE_FORMAT_R32G32_FLOAT,
	AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT,
	AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
	AERON_TEXTURE_FORMAT_RGBA32_FLOAT,
	AERON_TEXTURE_FORMAT_R10G10B10A2_UNORM,
	AERON_TEXTURE_FORMAT_BC1_RGBA_UNORM,
	AERON_TEXTURE_FORMAT_BC1_RGBA_SRGB,
	AERON_TEXTURE_FORMAT_BC3_RGBA_UNORM,
	AERON_TEXTURE_FORMAT_BC3_RGBA_SRGB,
	AERON_TEXTURE_FORMAT_BC4_R_UNORM,
	AERON_TEXTURE_FORMAT_BC5_RG_UNORM,
	AERON_TEXTURE_FORMAT_BC6H_RGB_FLOAT,
	AERON_TEXTURE_FORMAT_BC6H_RGB_UFLOAT,
	AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM,
	AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB
} AeronTextureFormat;

/* Usage flags for GPU buffers. */
typedef enum AeronBufferUsage {
	AERON_BUFFER_USAGE_VERTEX                = 1u << 0,
	AERON_BUFFER_USAGE_INDEX                 = 1u << 1,
	AERON_BUFFER_USAGE_UNIFORM               = 1u << 2,
	AERON_BUFFER_USAGE_STORAGE               = 1u << 3,
	AERON_BUFFER_USAGE_UPLOAD                = 1u << 4,
	AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ  = 1u << 5,
	AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE = 1u << 6
} AeronBufferUsage;

/* Memory update pattern hint for GPU buffers. */
typedef enum AeronMemoryUsage { AERON_MEMORY_USAGE_GPU_ONLY, AERON_MEMORY_USAGE_DYNAMIC } AeronMemoryUsage;

/* Usage flags for GPU textures. */
typedef enum AeronTextureUsage {
	AERON_TEXTURE_USAGE_SAMPLED                                 = 1u << 0,
	AERON_TEXTURE_USAGE_COLOR_TARGET                            = 1u << 1,
	AERON_TEXTURE_USAGE_DEPTH_TARGET                            = 1u << 2,
	AERON_TEXTURE_USAGE_TRANSFER_DST                            = 1u << 3,
	AERON_TEXTURE_USAGE_TRANSFER_SRC                            = 1u << 4,
	AERON_TEXTURE_USAGE_COMPUTE_STORAGE_READ                    = 1u << 5,
	AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE                   = 1u << 6,
	AERON_TEXTURE_USAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE = 1u << 7
} AeronTextureUsage;

/* Texture filtering mode used by samplers. */
typedef enum AeronFilter { AERON_FILTER_NEAREST, AERON_FILTER_LINEAR } AeronFilter;

/* Texture coordinate behavior outside the 0..1 range. */
typedef enum AeronAddressMode {
	AERON_ADDRESS_CLAMP_TO_EDGE,
	AERON_ADDRESS_REPEAT,
	AERON_ADDRESS_MIRRORED_REPEAT
} AeronAddressMode;

/* Shader stage for resource binding and uniform updates. */
typedef enum AeronShaderStage {
	AERON_SHADER_STAGE_VERTEX,
	AERON_SHADER_STAGE_FRAGMENT,
	AERON_SHADER_STAGE_COMPUTE
} AeronShaderStage;

/* Vertex attribute storage format. */
typedef enum AeronVertexFormat {
	AERON_VERTEX_FORMAT_FLOAT,
	AERON_VERTEX_FORMAT_FLOAT2,
	AERON_VERTEX_FORMAT_FLOAT3,
	AERON_VERTEX_FORMAT_FLOAT4,
	AERON_VERTEX_FORMAT_UBYTE4_NORM,
	AERON_VERTEX_FORMAT_SHORT2_NORM,
	AERON_VERTEX_FORMAT_SHORT4_NORM,
	AERON_VERTEX_FORMAT_UINT
} AeronVertexFormat;

/* Primitive topology consumed by a graphics pipeline. */
typedef enum AeronPrimitiveType {
	AERON_PRIMITIVE_TRIANGLES,
	AERON_PRIMITIVE_LINES,
	AERON_PRIMITIVE_POINTS,
	AERON_PRIMITIVE_TRIANGLE_STRIP
} AeronPrimitiveType;

/* Triangle culling mode. */
typedef enum AeronCullMode { AERON_CULL_NONE, AERON_CULL_FRONT, AERON_CULL_BACK } AeronCullMode;

/* Comparison operation used by depth testing. */
typedef enum AeronCompareOp {
	AERON_COMPARE_ALWAYS,
	AERON_COMPARE_LESS,
	AERON_COMPARE_LESS_EQUAL,
	AERON_COMPARE_EQUAL,
	AERON_COMPARE_GREATER,
	AERON_COMPARE_GREATER_EQUAL
} AeronCompareOp;

/* Blend factor used by color target blend state. */
typedef enum AeronBlendFactor {
	AERON_BLEND_ZERO,
	AERON_BLEND_ONE,
	AERON_BLEND_SRC_COLOR,
	AERON_BLEND_ONE_MINUS_SRC_COLOR,
	AERON_BLEND_DST_COLOR,
	AERON_BLEND_ONE_MINUS_DST_COLOR,
	AERON_BLEND_SRC_ALPHA,
	AERON_BLEND_ONE_MINUS_SRC_ALPHA,
	AERON_BLEND_DST_ALPHA,
	AERON_BLEND_ONE_MINUS_DST_ALPHA
} AeronBlendFactor;

/* Blend equation used by color target blend state. */
typedef enum AeronBlendOp {
	AERON_BLEND_OP_ADD,
	AERON_BLEND_OP_SUBTRACT,
	AERON_BLEND_OP_REVERSE_SUBTRACT,
	AERON_BLEND_OP_MIN,
	AERON_BLEND_OP_MAX
} AeronBlendOp;

/* Index element size for indexed draws. */
typedef enum AeronIndexFormat { AERON_INDEX_FORMAT_UINT16, AERON_INDEX_FORMAT_UINT32 } AeronIndexFormat;

/* Blend mode used when compositing a layer into the final frame.
 * PREMULTIPLIED expects the layer's color already multiplied by alpha
 * (src=ONE, dst=ONE_MINUS_SRC_ALPHA). */
typedef enum AeronLayerBlendMode {
	AERON_LAYER_BLEND_OPAQUE,
	AERON_LAYER_BLEND_ALPHA,
	AERON_LAYER_BLEND_PREMULTIPLIED
} AeronLayerBlendMode;

/* Integer rectangle in the application's logical render coordinate space. */
typedef struct AeronRectI {
	int x;
	int y;
	int width;
	int height;
} AeronRectI;

/* Read-only CPU pixel frame view; generation is used to skip uploads of unchanged frames. */
typedef struct AeronPixelFrameView {
	const void*              pixels;
	int                      width;
	int                      height;
	int                      pitch;
	int                      bpp;
	AeronPixelFormat         format;
	AeronColorSpace          color_space;
	const AeronPaletteEntry* palette;
	uint32_t                 generation;
} AeronPixelFrameView;

/* Opaque Aeron-owned GPU texture. */
typedef struct AeronTexture AeronTexture;

/* Opaque Aeron-owned GPU buffer used for vertices, indices, or storage data. */
typedef struct AeronBuffer AeronBuffer;

/* Opaque Aeron-owned GPU sampler state object. */
typedef struct AeronSampler AeronSampler;

/* Opaque Aeron-owned shader loaded from the compiled shader output directory. */
typedef struct AeronShader AeronShader;

/* Opaque Aeron-owned immutable graphics pipeline. */
typedef struct AeronGraphicsPipeline AeronGraphicsPipeline;

/* Opaque Aeron-owned immutable compute pipeline. */
typedef struct AeronComputePipeline AeronComputePipeline;

/* Opaque Aeron-owned color render target that can also be sampled as a texture. */
typedef struct AeronRenderTarget AeronRenderTarget;

/* Opaque Aeron-owned depth target for 3D render passes. */
typedef struct AeronDepthTarget AeronDepthTarget;

/* Opaque active GPU render pass created by Aeron_BeginRenderPass. */
typedef struct AeronRenderPass AeronRenderPass;

/* Opaque active GPU compute pass created by Aeron_BeginComputePass. */
typedef struct AeronComputePass AeronComputePass;

/* Opaque explicit GPU command buffer. Lets a caller record several render and
 * copy passes (plus texture/buffer uploads) into one submission. */
typedef struct AeronCommandBuffer AeronCommandBuffer;

/* Most recent unexpected renderer failure. The returned string is owned by
 * Aeron and remains valid until shutdown or a later renderer failure. */
const char* Aeron_RenderLastError(void);
/* Builds a diagnostic from Aeron_RenderLastError, shows it once, and requests
 * orderly application shutdown with a nonzero result. */
void Aeron_RequestFatalRendererError(const char* operation);

/* Generation-tagged identifier for one deferred layer in the current host
 * frame. The value 0 is invalid. Tokens expire at the next Aeron_BeginFrame. */
typedef uint32_t AeronRenderSubmission;

/* Records a deferred layer directly into Aeron's active swapchain render pass.
 * All objects passed to the callback are borrowed for the duration of the call:
 * do not end, submit, destroy, or retain them. */
typedef void (*AeronSwapchainRenderCallback)(AeronCommandBuffer* command_buffer, AeronRenderPass* render_pass,
											 AeronRenderTarget* target, int target_width, int target_height,
											 void* userdata);

typedef enum AeronPixelSamplingMode {
	AERON_PIXEL_SAMPLING_LINEAR,
	AERON_PIXEL_SAMPLING_NEAREST,
	AERON_PIXEL_SAMPLING_SHARP_BILINEAR
} AeronPixelSamplingMode;

/* Render-target raster sample count. Zero-initialized descriptions select 1x. */
typedef enum AeronSampleCount {
	AERON_SAMPLE_COUNT_1 = 1,
	AERON_SAMPLE_COUNT_2 = 2,
	AERON_SAMPLE_COUNT_4 = 4,
	AERON_SAMPLE_COUNT_8 = 8
} AeronSampleCount;

/* CPU pixel layer submitted for ordered composition during Aeron_Present. */
typedef struct AeronPixelLayerDesc {
	AeronPixelFrameView frame;
	AeronRectI          logical_rect;
	AeronLayerBlendMode blend_mode;
	/* Zero-initialized descriptors use conventional bilinear sampling. Sharp
	 * bilinear preserves flat source texels while smoothing their boundaries. */
	AeronPixelSamplingMode sampling;
	/* Preserve already-encoded source values when copying into a legacy UNORM target. */
	int preserve_encoded_values;
	/* Optional raw source pixel value that should become transparent during upload. */
	int      color_key_enabled;
	uint32_t color_key;
	/* Optional RGBA multiplier applied at composition time (crossfades, dims).
	 * Ignored when tint_enabled is zero, so zero-initialized descs keep the
	 * untinted behavior. */
	int   tint_enabled;
	float tint_rgba[4];
	/* Optional scissor in logical coordinates; zero width/height disables it. */
	AeronRectI scissor;
} AeronPixelLayerDesc;

/* GPU texture layer submitted for ordered composition during Aeron_Present. */
typedef struct AeronTextureLayerDesc {
	AeronTexture*       texture;
	AeronRectI          logical_rect;
	AeronLayerBlendMode blend_mode;
	/* Color space represented by UNORM texture values; sRGB GPU textures decode in hardware. */
	AeronColorSpace color_space;
	/* Optional RGBA multiplier applied at composition time (crossfades, dims).
	 * Ignored when tint_enabled is zero, so zero-initialized descs keep the
	 * untinted behavior. */
	int   tint_enabled;
	float tint_rgba[4];
	/* Optional additive RGB applied per pixel, weighted by the sample's alpha
	 * so it only paints inside layer coverage:
	 *     out = sample * tint + bias.rgb * sample.a
	 * Used by engine-driven fade-to-color replication. bias_rgba[3] is unused.
	 * Only read when tint_enabled is set. */
	float bias_rgba[4];
	/* Optional scissor in logical coordinates; zero width/height disables it. */
	AeronRectI scissor;
} AeronTextureLayerDesc;

/* Deferred draw layer for clients that can render a complete image directly
 * into the swapchain. The callback runs during Aeron_Present in normal layer
 * order and may only record commands valid inside an active render pass. Exact
 * dimensions protect pixel-authored rendering from resize races. Descriptor
 * pointers must remain valid until Aeron_Present returns. */
typedef struct AeronSwapchainRenderLayerDesc {
	AeronSwapchainRenderCallback callback;
	void*                        userdata;
	int                          required_width;
	int                          required_height;
	/* Optional group name around the callback's GPU commands. The string must
	 * remain valid until Aeron_Present returns. */
	const char* debug_label;
} AeronSwapchainRenderLayerDesc;

/* Description for a GPU buffer allocation. */
typedef struct AeronBufferDesc {
	uint32_t         size;
	uint32_t         usage;
	AeronMemoryUsage memory_usage;
	/* Optional name shown by GPU capture and debugging tools. */
	const char* debug_name;
} AeronBufferDesc;

/* One region in a batched CPU-to-GPU buffer upload. Regions targeting the same
 * dynamic buffer are uploaded into one cycled allocation. */
typedef struct AeronBufferUploadDesc {
	AeronBuffer* buffer;
	uint32_t     offset;
	const void*  data;
	uint32_t     size;
} AeronBufferUploadDesc;

/* Read-only staging usage for one explicit command buffer. */
typedef struct AeronCommandBufferUploadUsage {
	uint64_t staged_bytes;
	uint64_t reserved_bytes;
	uint32_t copy_count;
	uint32_t buffer_copy_count;
	uint32_t texture_copy_count;
	uint32_t chunk_count;
	uint32_t copy_pass_count;
	uint32_t largest_upload_bytes;
} AeronCommandBufferUploadUsage;

/* Description for a game-owned GPU texture allocation. */
typedef struct AeronTextureDesc {
	int                width;
	int                height;
	int                mip_count;
	AeronTextureFormat format;
	uint32_t           usage;
	/* Nonzero creates a cube texture (6 layers); uploads select the face via
	 * AeronTextureUploadDesc.layer. */
	int cube;
	/* Optional name shown by GPU capture and debugging tools. */
	const char* debug_name;
} AeronTextureDesc;

/* CPU-side image upload into an Aeron texture subregion.
 *
 * Two source modes:
 *   - pixels/pitch/pixel_format/palette: CPU pixels converted to the texture's
 *     GPU layout during upload (8-bit UNORM texture formats only).
 *   - raw_data/raw_size: bytes copied verbatim in the texture's own format —
 *     required for block-compressed and float formats. width/height still
 *     describe the destination region in texels.
 * When raw_data is non-NULL it takes precedence over pixels. */
typedef struct AeronTextureUploadDesc {
	AeronTexture*            texture;
	int                      mip_level;
	int                      x;
	int                      y;
	int                      width;
	int                      height;
	const void*              pixels;
	int                      pitch;
	AeronPixelFormat         pixel_format;
	AeronColorSpace          color_space;
	const AeronPaletteEntry* palette;
	/* Nonzero permits backend resource cycling for this upload. Leave zero when
	   updating several regions or mip levels of the same texture in sequence. */
	int cycle;
	/* Raw already-encoded source bytes (see struct comment). */
	const void* raw_data;
	uint32_t    raw_size;
	/* Destination array layer / cube face (0 for plain 2D textures). */
	int layer;
} AeronTextureUploadDesc;

/* Description for sampler filtering and address modes. */
typedef struct AeronSamplerDesc {
	AeronFilter      min_filter;
	AeronFilter      mag_filter;
	AeronFilter      mip_filter;
	AeronAddressMode address_u;
	AeronAddressMode address_v;
	AeronAddressMode address_w;
	/* Bias added to the backend's computed mip level before clamping. */
	float mip_lod_bias;
	/* Lowest mip level the sampler can select. */
	float min_lod;
	/* Highest mip level the sampler can select. Set to 0 for base-level only. */
	float max_lod;
	/* Nonzero enables anisotropic filtering for angled texture footprints. */
	int enable_anisotropy;
	/* Maximum anisotropy clamp used when enable_anisotropy is nonzero. */
	float max_anisotropy;
	/* Comparison sampling for depth textures (shadow maps). */
	int            enable_compare;
	AeronCompareOp compare;
} AeronSamplerDesc;

/* Description for loading a compiled shader artifact by name and stage. */
typedef struct AeronShaderDesc {
	const char*      name;
	AeronShaderStage stage;
	uint32_t         sampler_count;
	uint32_t         uniform_buffer_count;
	uint32_t         storage_buffer_count;
} AeronShaderDesc;

/* One vertex attribute consumed by a graphics pipeline. */
typedef struct AeronVertexAttributeDesc {
	uint32_t          location;
	uint32_t          buffer_slot;
	AeronVertexFormat format;
	uint32_t          offset;
} AeronVertexAttributeDesc;

/* One vertex buffer layout consumed by a graphics pipeline. */
typedef struct AeronVertexBufferLayoutDesc {
	uint32_t slot;
	uint32_t stride;
	int      per_instance;
} AeronVertexBufferLayoutDesc;

/* Depth test/write state for a graphics pipeline. */
typedef struct AeronDepthStateDesc {
	int            depth_test;
	int            depth_write;
	AeronCompareOp compare;
} AeronDepthStateDesc;

/* Optional polygon depth bias. Factors follow the backend rasterizer
 * convention; zero-initialized pipeline descriptions leave it disabled. */
typedef struct AeronRasterizerStateDesc {
	int   depth_bias;
	float depth_bias_constant_factor;
	float depth_bias_clamp;
	float depth_bias_slope_factor;
} AeronRasterizerStateDesc;

/* Color blend state for a graphics pipeline. */
typedef struct AeronBlendStateDesc {
	int              enabled;
	AeronBlendFactor src_color;
	AeronBlendFactor dst_color;
	AeronBlendOp     color_op;
	AeronBlendFactor src_alpha;
	AeronBlendFactor dst_alpha;
	AeronBlendOp     alpha_op;
	/* Channel write mask (bit 0 = R … bit 3 = A), honored only when
	 * color_write_mask_enable is nonzero — masking ALL channels (0) is a
	 * valid choice for declared-but-unwritten MRT slots. Zero-initialized
	 * descs keep full writes. */
	int     color_write_mask_enable;
	uint8_t color_write_mask;
} AeronBlendStateDesc;

/* One color attachment of a multi-target graphics pipeline. */
typedef struct AeronColorTargetStateDesc {
	AeronTextureFormat  format;
	AeronBlendStateDesc blend;
} AeronColorTargetStateDesc;

/* Immutable graphics pipeline description for drawing in render passes.
 * Single-target pipelines use color_format/blend; multi-target pipelines set
 * color_target_count > 0 and describe every attachment in color_targets
 * (color_format/blend are then ignored). */
typedef struct AeronGraphicsPipelineDesc {
	AeronShader*                       vertex_shader;
	AeronShader*                       fragment_shader;
	AeronPrimitiveType                 primitive_type;
	AeronCullMode                      cull_mode;
	const AeronVertexBufferLayoutDesc* vertex_buffers;
	uint32_t                           vertex_buffer_count;
	const AeronVertexAttributeDesc*    attributes;
	uint32_t                           attribute_count;
	AeronTextureFormat                 color_format;
	AeronTextureFormat                 depth_format;
	AeronRasterizerStateDesc           rasterizer;
	AeronDepthStateDesc                depth;
	AeronBlendStateDesc                blend;
	uint32_t                           color_target_count;
	const AeronColorTargetStateDesc*   color_targets;
	AeronSampleCount                   sample_count;
} AeronGraphicsPipelineDesc;

/* Compute shaders are loaded directly into the pipeline on SDL_GPU. Resource
 * counts and thread-group dimensions must match the compiled shader. */
typedef struct AeronComputePipelineDesc {
	const char* name;
	/* Compiled entry point. NULL uses the Aeron shader convention ("main" for
	 * SPIR-V/DXIL and "main0" for MSL). */
	const char* entrypoint;
	uint32_t    sampler_count;
	uint32_t    readonly_storage_texture_count;
	uint32_t    readonly_storage_buffer_count;
	uint32_t    readwrite_storage_texture_count;
	uint32_t    readwrite_storage_buffer_count;
	uint32_t    uniform_buffer_count;
	uint32_t    thread_count_x;
	uint32_t    thread_count_y;
	uint32_t    thread_count_z;
} AeronComputePipelineDesc;

/* One writable texture subresource declared at compute-pass begin. */
typedef struct AeronComputeTextureBinding {
	AeronTexture* texture;
	uint32_t      mip_level;
	uint32_t      layer;
	int           cycle;
} AeronComputeTextureBinding;

/* One writable storage buffer declared at compute-pass begin. */
typedef struct AeronComputeBufferBinding {
	AeronBuffer* buffer;
	int          cycle;
} AeronComputeBufferBinding;

typedef struct AeronComputePassDesc {
	AeronCommandBuffer*               command_buffer;
	const AeronComputeTextureBinding* write_textures;
	uint32_t                          write_texture_count;
	const AeronComputeBufferBinding*  write_buffers;
	uint32_t                          write_buffer_count;
	const char*                       debug_label;
} AeronComputePassDesc;

/* One GPU texture-to-texture copy. Formats must be copy-compatible. */
typedef struct AeronTextureCopyDesc {
	AeronTexture* source;
	uint32_t      source_mip_level;
	uint32_t      source_layer;
	uint32_t      source_x;
	uint32_t      source_y;
	AeronTexture* destination;
	uint32_t      destination_mip_level;
	uint32_t      destination_layer;
	uint32_t      destination_x;
	uint32_t      destination_y;
	uint32_t      width;
	uint32_t      height;
	int           cycle;
} AeronTextureCopyDesc;

/* Description for an Aeron color render target. */
typedef struct AeronRenderTargetDesc {
	int                width;
	int                height;
	AeronTextureFormat format;
	AeronSampleCount   sample_count;
	/* Optional additional AeronTextureUsage bits (for example compute read
	 * on a temporal input or compute write on an upscaler output).
	 * Multisample targets are attachment-only and require zero here. */
	uint32_t usage;
	/* Optional name shown by GPU capture and debugging tools. */
	const char* debug_name;
} AeronRenderTargetDesc;

/* Description for an Aeron depth target. */
typedef struct AeronDepthTargetDesc {
	int                width;
	int                height;
	AeronTextureFormat format;
	AeronSampleCount   sample_count;
	/* Nonzero also makes the depth texture sampleable (SSAO, depth-aware
	   effects) through Aeron_DepthTargetGetTexture. Multisample depth is
	   attachment-only because SDL GPU does not expose depth resolve. */
	int sampled;
	/* Optional name shown by GPU capture and debugging tools. */
	const char* debug_name;
} AeronDepthTargetDesc;

/* Description for an offscreen GPU render pass targeting Aeron render resources. */
typedef struct AeronRenderPassDesc {
	/* May be NULL for a depth-only pass. */
	AeronRenderTarget* color_target;
	/* Optional 1x target receiving color_target's multisample resolve. */
	AeronRenderTarget* color_resolve_target;
	AeronDepthTarget*  depth_target;
	AeronRectI         viewport;
	AeronRectI         scissor;
	/* Discard prior color contents before a pass that fully overwrites every
	 * attachment. Mutually exclusive with clear_color. */
	int   discard_color;
	int   clear_color;
	float clear_color_rgba[4];
	int   clear_depth;
	float clear_depth_value;
	/* Discard transient depth contents at pass end instead of storing them. */
	int discard_depth;
	/* Required command buffer. Aeron_EndRenderPass ends recording; the caller
	 * owns submission through Aeron_SubmitCommandBuffer or its borrowed
	 * presentation command-buffer boundary. */
	AeronCommandBuffer* command_buffer;
	/* Additional color attachments for multi-target passes: color_target is
	   attachment 0, these are attachments 1..N. Every attachment shares the
	   color load settings. */
	uint32_t                  extra_color_target_count;
	AeronRenderTarget* const* extra_color_targets;
	/* Optional group name spanning this complete GPU render pass. */
	const char* debug_label;
} AeronRenderPassDesc;

/* Queues a CPU pixel layer; referenced pixels must remain valid until Aeron_Present uploads them.
 * Returns a current-frame submission token, or 0 on failure. */
AeronRenderSubmission Aeron_SubmitPixelLayer(const AeronPixelLayerDesc* desc);

/* Queues an Aeron texture layer for ordered composition during Aeron_Present.
 * Returns a current-frame submission token, or 0 on failure. */
AeronRenderSubmission Aeron_SubmitTextureLayer(const AeronTextureLayerDesc* desc);

/* Cancels a current-frame deferred layer. Expired or invalid tokens are safe no-ops. */
void Aeron_CancelRenderSubmission(AeronRenderSubmission submission);

/* Queues direct rendering into Aeron's swapchain pass. */
int Aeron_SubmitSwapchainRenderLayer(const AeronSwapchainRenderLayerDesc* desc);

/* Returns nonzero when a target of this exact size currently covers the full
 * drawable and can therefore use a direct swapchain render layer without
 * coordinate projection. Aeron revalidates this when acquiring the drawable. */
int Aeron_CanRenderDirectToSwapchain(int target_width, int target_height);

/* Clears all queued render submissions for the current host frame. */
void Aeron_ClearRenderSubmissions(void);

/* Immediately composites a CPU pixel layer into an Aeron render target. */
int Aeron_ComposePixelLayerToRenderTarget(AeronRenderTarget* target, const AeronPixelLayerDesc* desc,
										  int clear_color, const float clear_color_rgba[4]);

/* Creates an Aeron-owned GPU buffer. */
AeronBuffer* Aeron_CreateBuffer(const AeronBufferDesc* desc);

/* Releases a buffer created by Aeron_CreateBuffer. */
void Aeron_DestroyBuffer(AeronBuffer* buffer);

/* Uploads bytes into an Aeron buffer using an internal SDL GPU transfer buffer. */
int Aeron_UploadBufferData(AeronBuffer* buffer, uint32_t offset, const void* data, uint32_t size);

/* Creates an Aeron-owned sampled or renderable texture. */
AeronTexture* Aeron_CreateTexture(const AeronTextureDesc* desc);

/* Releases a texture created by Aeron_CreateTexture. */
void Aeron_DestroyTexture(AeronTexture* texture);

/* Uploads CPU pixels into an Aeron texture subregion. */
int Aeron_UploadTextureData(const AeronTextureUploadDesc* desc);

/* Stages one texture region into an explicit command buffer. No render or
 * compute pass may be open on the command buffer. */
int Aeron_UploadTextureDataCmd(AeronCommandBuffer* command_buffer, const AeronTextureUploadDesc* desc);

/* Stages texture regions through one packed transfer slice and records them in
 * one copy pass. Source bytes are copied before the function returns. */
int Aeron_UploadTextureBatchCmd(AeronCommandBuffer* command_buffer, const AeronTextureUploadDesc* uploads,
								uint32_t upload_count);

/* Creates an Aeron-owned sampler state object. */
AeronSampler* Aeron_CreateSampler(const AeronSamplerDesc* desc);

/* Copies the immutable description used to create a sampler. */
int Aeron_SamplerGetDesc(const AeronSampler* sampler, AeronSamplerDesc* desc);

/* Releases a sampler created by Aeron_CreateSampler. */
void Aeron_DestroySampler(AeronSampler* sampler);

/* Loads an Aeron shader from the compiled shader output directory. */
AeronShader* Aeron_CreateShader(const AeronShaderDesc* desc);

/* Releases a shader created by Aeron_CreateShader. */
void Aeron_DestroyShader(AeronShader* shader);

/* Creates an immutable graphics pipeline from shaders, vertex layout, and render state. */
AeronGraphicsPipeline* Aeron_CreateGraphicsPipeline(const AeronGraphicsPipelineDesc* desc);

/* Releases a pipeline created by Aeron_CreateGraphicsPipeline. */
void Aeron_DestroyGraphicsPipeline(AeronGraphicsPipeline* pipeline);

/* Creates an immutable compute pipeline from a compiled .comp shader. */
AeronComputePipeline* Aeron_CreateComputePipeline(const AeronComputePipelineDesc* desc);

/* Releases a pipeline created by Aeron_CreateComputePipeline. */
void Aeron_DestroyComputePipeline(AeronComputePipeline* pipeline);

/* Creates an Aeron-owned color target usable as both render target and texture layer. */
AeronRenderTarget* Aeron_CreateRenderTarget(const AeronRenderTargetDesc* desc);

/* Releases a color target created by Aeron_CreateRenderTarget. */
void Aeron_DestroyRenderTarget(AeronRenderTarget* target);

/* Returns the texture view for a color render target; owned by the render target. */
AeronTexture* Aeron_RenderTargetGetTexture(AeronRenderTarget* target);

/* Synchronously reads a color render target back into CPU memory, converting the
 * RGBA8 GPU pixels to the requested 16-bit staging format (RGB565 or RGB555) with
 * the given destination row pitch. Blocks on a GPU fence until the copy completes,
 * so it is meant for the render-target Lock path (Model B overlays), not per-frame
 * use. Returns nonzero on success. */
int Aeron_ReadRenderTargetPixels(AeronRenderTarget* target, void* dst, int pitch, AeronPixelFormat format);

/* Creates an Aeron-owned depth target for GPU render passes. */
AeronDepthTarget* Aeron_CreateDepthTarget(const AeronDepthTargetDesc* desc);

/* Releases a depth target created by Aeron_CreateDepthTarget. */
void Aeron_DestroyDepthTarget(AeronDepthTarget* target);

/* Returns the sampleable texture view of a depth target created with
 * `sampled` set; NULL otherwise. Owned by the depth target. */
AeronTexture* Aeron_DepthTargetGetTexture(AeronDepthTarget* target);

/* Returns nonzero when the backend supports `format` for all the requested
 * AeronTextureUsage bits on 2D textures. */
int Aeron_TextureFormatSupported(AeronTextureFormat format, uint32_t usage);
/* Returns nonzero when an exact render-target sample count is supported for
 * the format. This does not silently downgrade the request. */
int Aeron_TextureFormatSupportsSampleCount(AeronTextureFormat format, AeronSampleCount sample_count);

/* Read-only descriptions of Aeron GPU buffers. */
uint32_t Aeron_BufferSize(const AeronBuffer* buffer);
uint32_t Aeron_BufferUsageFlags(const AeronBuffer* buffer);

/* Acquires an explicit command buffer for multi-pass recording. Submit with
 * Aeron_SubmitCommandBuffer or discard with Aeron_CancelCommandBuffer. */
AeronCommandBuffer* Aeron_AcquireCommandBuffer(void);

/* Submits an explicit command buffer and releases it. Returns nonzero on
 * success. The pointer is invalid afterwards either way. */
int Aeron_SubmitCommandBuffer(AeronCommandBuffer* command_buffer);

/* Discards an explicit command buffer without executing its commands. */
void Aeron_CancelCommandBuffer(AeronCommandBuffer* command_buffer);

/* Copies current staging usage into out. Returns zero for invalid arguments. */
int Aeron_CommandBufferGetUploadUsage(const AeronCommandBuffer* command_buffer,
									  AeronCommandBufferUploadUsage* out);
/* Records a sticky high-level preparation failure. Submission then rejects the
 * command buffer and preserves `message` as the renderer diagnostic. */
void Aeron_CommandBufferSetFailure(AeronCommandBuffer* command_buffer, const char* message);

/* GPU-capture annotations. These are backed by SDL_GPU in debug builds and
 * become no-ops in release builds. Push/pop calls must be balanced on the same
 * command buffer. */
void Aeron_GpuDebugPush(AeronCommandBuffer* command_buffer, const char* name);
void Aeron_GpuDebugPop(AeronCommandBuffer* command_buffer);
void Aeron_GpuDebugMarker(AeronCommandBuffer* command_buffer, const char* name);
void Aeron_GpuDebugNameTexture(AeronTexture* texture, const char* name);
void Aeron_GpuDebugNameBuffer(AeronBuffer* buffer, const char* name);

/* Uploads bytes into an Aeron buffer, recording the copy into an explicit
 * command buffer (no pass may be open on it). The data is staged immediately;
 * the GPU copy executes when the command buffer is submitted. */
int Aeron_UploadBufferDataCmd(AeronCommandBuffer* command_buffer, AeronBuffer* buffer, uint32_t offset,
							  const void* data, uint32_t size);

/* Stages all regions through one packed transfer slice and records them in one
 * copy pass. No pass may be open on command_buffer. */
int Aeron_UploadBufferBatchCmd(AeronCommandBuffer* command_buffer, const AeronBufferUploadDesc* uploads,
							   uint32_t upload_count);

/* ===== Output / swapchain ===== */

/* Why HDR output is or is not running, for logging and settings UI. */
typedef enum AeronHdrOutputStatus {
	/* Extended-linear composition is running. */
	AERON_HDR_OUTPUT_ACTIVE = 0,
	/* HDR output was not requested. */
	AERON_HDR_OUTPUT_DISABLED,
	/* Requested, but the display is not in HDR mode. */
	AERON_HDR_OUTPUT_DISPLAY_SDR,
	/* Requested on an HDR display, but the render backend refused it. */
	AERON_HDR_OUTPUT_UNSUPPORTED
} AeronHdrOutputStatus;

/* Returns nonzero when the display + backend support HDR output
 * (extended-linear swapchain composition). */
int Aeron_OutputSupportsHdr(void);

/* Requests SDR or HDR extended-linear composition. The request is remembered
 * and re-applied when the display's HDR state changes, so asking for HDR on an
 * SDR display is not an error: it stays SDR and switches over if that display
 * later enters HDR mode. Returns nonzero unless the swapchain could not be
 * configured at all, in which case the previous mode stays active. Query
 * Aeron_OutputHdrEnabled / Aeron_OutputHdrStatus for the effective mode. */
int Aeron_SetOutputHdr(int enabled);

/* Returns nonzero while HDR extended-linear output is active. */
int Aeron_OutputHdrEnabled(void);

/* Effective HDR state plus the reason behind it. */
AeronHdrOutputStatus Aeron_OutputHdrStatus(void);

/* Stable lowercase identifier for a status value (never NULL). */
const char* Aeron_OutputHdrStatusName(AeronHdrOutputStatus status);

/* Display HDR headroom: max luminance relative to SDR white (1.0 = SDR).
 * Cached from the display; refreshed on HDR-state and display-change events. */
float Aeron_OutputHdrHeadroom(void);

/* SDR reference white in the active extended-linear swapchain encoding. */
float Aeron_OutputSdrWhiteLevel(void);

/* Decode gamma applied to display-referred (sRGB) layers when compositing
 * into the HDR extended-linear swapchain: 0 selects the exact piecewise sRGB
 * curve, a positive value a pow(rgb, gamma) decode (clamped to [1, 3]).
 * Platform default is 2.2 on Windows/Linux — that art targets ~2.2-power SDR
 * displays and the piecewise toe lifts its darks under HDR — and piecewise on
 * Apple, whose compositor presents SDR content with the piecewise curve. SDR
 * compositions always use the piecewise curve regardless of this setting. */
void  Aeron_SetOutputSdrContentGamma(float gamma);
float Aeron_OutputSdrContentGamma(void);

/* RGB scale required when writing SDR-relative linear colors into this pass.
 * Offscreen and SDR passes return 1.0. */
float Aeron_RenderPassOutputRgbScale(const AeronRenderPass* pass);

/* Current swapchain texture format (changes with Aeron_SetOutputHdr). */
AeronTextureFormat Aeron_SwapchainFormat(void);

/* Stable SDL_GPU driver identifier (for example metal, vulkan, or direct3d12). */
const char* Aeron_RenderDriverName(void);
/* Copies the current frame's uniform/storage transfer counters. */
void Aeron_GetRenderDataStats(AeronRenderDataStats* out_stats);

/* Starts an offscreen GPU render pass; end it with Aeron_EndRenderPass. */
AeronRenderPass* Aeron_BeginRenderPass(const AeronRenderPassDesc* desc);

/* Ends and submits an offscreen GPU render pass started by Aeron_BeginRenderPass. */
void Aeron_EndRenderPass(AeronRenderPass* pass);
/* Returns the raster sample count of an active render pass. */
AeronSampleCount Aeron_RenderPassGetSampleCount(const AeronRenderPass* pass);

/* Starts a compute pass without allocating. Dependent dispatches must use
 * separate passes; nested passes on one command buffer are rejected. */
AeronComputePass* Aeron_BeginComputePass(const AeronComputePassDesc* desc);

/* Ends a compute pass without submitting its caller-owned command buffer. */
void Aeron_EndComputePass(AeronComputePass* pass);

/* Binds the pipeline used by subsequent compute dispatches. */
void Aeron_BindComputePipeline(AeronComputePass* pass, AeronComputePipeline* pipeline);

/* Binds one sampled texture/sampler pair to the compute stage. */
void Aeron_BindComputeTextureSampler(AeronComputePass* pass, uint32_t slot, AeronTexture* texture,
									 AeronSampler* sampler);

/* Binds read-only compute storage textures or buffers. */
void Aeron_BindComputeStorageTexture(AeronComputePass* pass, uint32_t slot, AeronTexture* texture);
void Aeron_BindComputeStorageBuffer(AeronComputePass* pass, uint32_t slot, AeronBuffer* buffer);

/* Pushes one compute uniform block, up to AERON_MAX_UNIFORM_DATA_SIZE bytes.
 * Oversized blocks are rejected; use a storage buffer instead. */
void Aeron_BindComputeUniformData(AeronComputePass* pass, uint32_t slot, const void* data, uint32_t size);

/* Dispatches compute work groups. */
void Aeron_DispatchCompute(AeronComputePass* pass, uint32_t group_count_x, uint32_t group_count_y,
						   uint32_t group_count_z);

/* Records a GPU texture copy into a caller-owned command buffer. */
int Aeron_CopyTextureCmd(AeronCommandBuffer* command_buffer, const AeronTextureCopyDesc* desc);

/* Binds the graphics pipeline used by subsequent draw calls in the render pass. */
void Aeron_BindGraphicsPipeline(AeronRenderPass* pass, AeronGraphicsPipeline* pipeline);

/* Binds one vertex buffer slot for subsequent draw calls in the render pass. */
void Aeron_BindVertexBuffer(AeronRenderPass* pass, uint32_t slot, AeronBuffer* buffer, uint32_t offset);

/* Binds the index buffer used by subsequent indexed draw calls in the render pass. */
void Aeron_BindIndexBuffer(AeronRenderPass* pass, AeronBuffer* buffer, AeronIndexFormat format,
						   uint32_t offset);

/* Binds a texture and sampler pair to a shader stage for subsequent draw calls. */
void Aeron_BindTextureSampler(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot,
							  AeronTexture* texture, AeronSampler* sampler);

/* Pushes small uniform data to a shader stage for subsequent draw calls in
 * this command buffer. Blocks larger than AERON_MAX_UNIFORM_DATA_SIZE are
 * rejected; use a storage buffer instead. */
void Aeron_BindUniformData(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot, const void* data,
						   uint32_t size);

/* Binds a storage buffer (created with AERON_BUFFER_USAGE_STORAGE) to a shader
 * stage for subsequent draw calls in the render pass. */
void Aeron_BindStorageBuffer(AeronRenderPass* pass, AeronShaderStage stage, uint32_t slot,
							 AeronBuffer* buffer);

/* Sets the render-target-pixel viewport for subsequent draw calls. */
void Aeron_SetViewport(AeronRenderPass* pass, const AeronRectI* rect);

/* Sets the render-target-pixel scissor rectangle for subsequent draw calls. */
void Aeron_SetScissor(AeronRenderPass* pass, const AeronRectI* rect);

/* Issues a non-indexed draw with one instance. */
void Aeron_Draw(AeronRenderPass* pass, uint32_t vertex_count, uint32_t first_vertex);

/* Issues an indexed draw with one instance. */
void Aeron_DrawIndexed(AeronRenderPass* pass, uint32_t index_count, uint32_t first_index,
					   int32_t vertex_offset);

/* Issues a non-indexed instanced draw. */
void Aeron_DrawInstanced(AeronRenderPass* pass, uint32_t vertex_count, uint32_t instance_count,
						 uint32_t first_vertex);

/* Issues an indexed instanced draw. */
void Aeron_DrawIndexedInstanced(AeronRenderPass* pass, uint32_t index_count, uint32_t instance_count,
								uint32_t first_index, int32_t vertex_offset);

/* Returns the logical width of an Aeron texture. */
int Aeron_TextureGetWidth(const AeronTexture* texture);

/* Returns the logical height of an Aeron texture. */
int Aeron_TextureGetHeight(const AeronTexture* texture);

/* Returns the format used to create an Aeron texture. */
AeronTextureFormat Aeron_TextureGetFormat(const AeronTexture* texture);

/* Returns the number of mip levels and the creation usage flags. */
int      Aeron_TextureGetMipCount(const AeronTexture* texture);
uint32_t Aeron_TextureGetUsage(const AeronTexture* texture);

#ifdef __cplusplus
}
#endif

#endif
