#ifndef AERON_INTERNAL_H
#define AERON_INTERNAL_H

#include "aeron/aeron.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdint.h>

#define AERON_MAX_PATH 1024
#define AERON_MAX_DELTA_US 250000
#define AERON_MAX_RENDER_LAYERS 64
#define AERON_MAX_PIXEL_LAYER_UPLOADS 8
#define AERON_MAX_COMPUTE_WRITE_TEXTURES 64
#define AERON_MAX_COMPUTE_WRITE_BUFFERS 64
#define AERON_UPLOAD_CHUNK_MIN_BYTES (64u * 1024u)
#define AERON_UPLOAD_CHUNK_TARGET_BYTES (16u * 1024u * 1024u)
/* Limits of the pool of upload chunks kept between command buffers. Larger
 * chunks, usually from asset loading, are released after use. */
#define AERON_UPLOAD_CHUNK_POOL_MAX 16
#define AERON_UPLOAD_CHUNK_POOL_MAX_CHUNK_BYTES (4u * 1024u * 1024u)
#define AERON_UPLOAD_CHUNK_POOL_MAX_BYTES (16u * 1024u * 1024u)

typedef struct AeronVfsCaseDirectory AeronVfsCaseDirectory;
struct AeronIso9660;

struct AeronVfs {
	char                   asset_root[AERON_MAX_PATH];
	char                   resource_root[AERON_MAX_PATH];
	char                   user_root[AERON_MAX_PATH];
	char                   temp_root[AERON_MAX_PATH];
	char                   mod_root[AERON_MAX_PATH];
	uint32_t               root_options[AERON_VFS_ROOT_COUNT];
	AeronVfsCaseDirectory* case_directories[AERON_VFS_ROOT_COUNT];
	struct AeronIso9660*   iso_roots[AERON_VFS_ROOT_COUNT];
};

struct AeronFile {
	SDL_IOStream* stream;
	int           eof;
	int           error;
};

struct AeronTexture {
	SDL_GPUTexture*    texture;
	int                width;
	int                height;
	int                mip_count;
	int                layer_count; /* cube arrays: 6 per cube */
	int                depth;       /* 3D textures; 0 otherwise */
	AeronTextureFormat format;
	uint32_t           usage;
	AeronSampleCount   sample_count;
	int                owned;
};

struct AeronBuffer {
	SDL_GPUBuffer*   buffer;
	uint32_t         size;
	uint32_t         usage;
	AeronMemoryUsage memory_usage;
};

struct AeronSampler {
	SDL_GPUSampler*  sampler;
	AeronSamplerDesc desc;
};

/* Shaders and pipelines are shared. Creating one that already exists
 * returns the existing one and counts one more user; destroying it removes
 * a user, and the last one releases it. Each kind is kept in a list. */
struct AeronShader {
	SDL_GPUShader*   shader;
	AeronShaderStage stage;
	uint32_t         sampler_count;
	uint32_t         uniform_buffer_count;
	uint32_t         storage_buffer_count;
	char*            name;
	/* Never reused, so that a pipeline's key can't match a later shader
	 * at the same address. */
	uint64_t     serial;
	uint32_t     users;
	AeronShader* next;
};

struct AeronGraphicsPipeline {
	SDL_GPUGraphicsPipeline* pipeline;
	/* The creation values as bytes, with shader serials for the shaders. */
	uint8_t*               key;
	size_t                 key_size;
	uint64_t               key_hash;
	uint32_t               users;
	AeronGraphicsPipeline* next;
};

struct AeronComputePipeline {
	SDL_GPUComputePipeline* pipeline;
	uint8_t*                key;
	size_t                  key_size;
	uint64_t                key_hash;
	uint32_t                users;
	AeronComputePipeline*   next;
};

struct AeronRenderTarget {
	AeronTexture color;
};

struct AeronDepthTarget {
	/* Embedded texture view; .texture/.usage are valid even when the target was
	 * created without `sampled` (usage then lacks AERON_TEXTURE_USAGE_SAMPLED). */
	AeronTexture depth;
};

struct AeronComputePass {
	SDL_GPUCommandBuffer* command_buffer;
	SDL_GPUComputePass*   compute_pass;
	AeronCommandBuffer*   external;
	int                   debug_group_open;
};

typedef struct AeronUploadChunk {
	SDL_GPUTransferBuffer* transfer;
	uint32_t               capacity;
	uint32_t               used;
} AeronUploadChunk;

typedef struct AeronUploadCycleState {
	const void* resource;
	uint8_t     kind;
	uint8_t     cycled;
} AeronUploadCycleState;

/* The most non-blocking readbacks (AeronBufferReadback) one command buffer
 * can record. */
#define AERON_COMMAND_READBACK_MAX 8

/* The debug groups a command buffer keeps open across a flush. */
#define AERON_COMMAND_DEBUG_DEPTH 16

struct AeronReadbackSlot;

/* Explicit command buffer wrapper. The command buffer owns its upload chunks
 * until it is submitted or canceled, then returns them to the pool in
 * g_aeron. Individual uploads suballocate the chunks instead of creating one
 * transfer buffer per region. Compute-pass state is embedded so pass encoding
 * does not allocate in steady state. */
struct AeronCommandBuffer {
	SDL_GPUCommandBuffer*  command_buffer;
	AeronUploadChunk*      upload_chunks;
	uint32_t               upload_chunk_count;
	uint32_t               upload_chunk_capacity;
	uint64_t               upload_staged_bytes;
	uint64_t               upload_reserved_bytes;
	uint32_t               upload_copy_count;
	uint32_t               upload_buffer_copy_count;
	uint32_t               upload_texture_copy_count;
	uint32_t               upload_copy_pass_count;
	uint32_t               largest_upload_bytes;
	uint32_t               next_upload_chunk_size;
	AeronUploadCycleState* upload_cycle_states;
	uint32_t               upload_cycle_state_count;
	uint32_t               upload_cycle_state_capacity;
	/* The readback copies recorded on this command buffer, which wait for
	 * its fence once it is submitted. */
	struct AeronReadbackSlot* readback_slots[AERON_COMMAND_READBACK_MAX];
	uint32_t                  readback_slot_count;
	AeronComputePass       active_compute_pass;
	int                    compute_pass_active;
	int                    render_pass_active;
	int                    owns_wrapper;
	int                    immediate_upload;
	int                    failed;
	char                   failure_message[512];
	/* The open debug groups, outermost first. */
	char     debug_labels[AERON_COMMAND_DEBUG_DEPTH][64];
	uint32_t debug_depth;
};

struct AeronRenderPass {
	SDL_GPUCommandBuffer* command_buffer;
	SDL_GPURenderPass*    render_pass;
	AeronCommandBuffer*   owner;
	float                 output_rgb_scale;
	AeronSampleCount      sample_count;
	AeronTextureFormat    depth_format;
	int                   debug_group_open;
	/* Attachment size, which every scissor is clamped to. A scissor that
	 * falls entirely outside it skips draws until the next one. */
	int width;
	int height;
	int scissor_empty;
};

typedef struct AeronPixelLayerUpload {
	SDL_GPUTexture*        texture;
	SDL_GPUTransferBuffer* transfer;
	SDL_GPUTextureFormat   texture_format;
	uint32_t               uploaded_generation;
	const void*            uploaded_pixels;
	int                    uploaded_preserve_encoded_values;
	int                    uploaded_color_key_enabled;
	uint32_t               uploaded_color_key;
	const uint8_t*         uploaded_coverage;
	uint32_t               transfer_size;
	int                    width;
	int                    height;
} AeronPixelLayerUpload;

typedef struct AeronFullscreenPipeline {
	SDL_GPUTextureFormat     format;
	AeronLayerBlendMode      blend_mode;
	SDL_GPUGraphicsPipeline* pipeline;
} AeronFullscreenPipeline;

typedef enum AeronRenderLayerKind {
	AERON_RENDER_LAYER_NONE,
	AERON_RENDER_LAYER_PIXEL,
	AERON_RENDER_LAYER_TEXTURE,
	AERON_RENDER_LAYER_SWAPCHAIN_RENDER
} AeronRenderLayerKind;

typedef struct AeronRenderLayer {
	AeronRenderLayerKind kind;
	int                  pixel_upload_index;

	union {
		AeronPixelLayerDesc           pixel;
		AeronTextureLayerDesc         texture;
		AeronSwapchainRenderLayerDesc swapchain_render;
	} u;
} AeronRenderLayer;

typedef struct AeronControllerDevice {
	SDL_Gamepad*   gamepad;
	SDL_Joystick*  joystick;
	SDL_JoystickID instance_id;
	int            owns_joystick;
	int            raw_axis_count;
	int            raw_button_count;
	int            raw_hat_count;
	int            has_rumble;
} AeronControllerDevice;

typedef struct AeronSharedFence AeronSharedFence;

#define AERON_PACING_HISTORY 120

/* Latency pacing: the delay before the next frame reads input, and the
 * last presents it was decided from. */
typedef struct AeronPacingState {
	int      enabled;
	uint32_t delay_us;
	uint32_t hold_frames;
	/* The app asked to wait before the frame being drawn. */
	int      app_delayed;
	uint64_t last_present_us;
	uint32_t history_count;
	uint32_t history_next;
	uint64_t present_us[AERON_PACING_HISTORY];
	uint32_t acquire_wait_us[AERON_PACING_HISTORY];
	uint8_t  late[AERON_PACING_HISTORY];
	uint8_t  busy[AERON_PACING_HISTORY];
} AeronPacingState;

typedef struct AeronRuntime {
	int                         initialized;
	int                         quit_requested;
	int                         fatal_error_requested;
	SDL_Window*                 window;
	SDL_GPUDevice*              gpu_device;
	SDL_GPUShader*              fullscreen_vertex_shader;
	SDL_GPUShader*              fullscreen_fragment_shader;
	AeronFullscreenPipeline     fullscreen_pipelines[8];
	int                         fullscreen_pipeline_count;
	SDL_GPUSampler*             pixel_frame_sampler;
	SDL_GPUSampler*             pixel_frame_nearest_sampler;
	SDL_GPUTextureFormat        swapchain_format;
	SDL_GPUSwapchainComposition swapchain_composition;
	/* HDR output: the mode the app asked for, the mode actually running, and
	 * the display properties cached from the last HDR-state event. The desired
	 * flag is kept so the composition can be re-applied when a display becomes
	 * HDR-capable (or stops being one) while the app runs. */
	int   hdr_output_desired;
	int   hdr_output_enabled;
	int   hdr_reapply_pending;
	float hdr_headroom;
	float hdr_sdr_white_level;
	/* Decode gamma for display-referred sRGB layers composited into the HDR
	 * swapchain: 0 = piecewise sRGB curve, >0 = pow(rgb, gamma). SDR
	 * compositions always use the piecewise curve (exact encode inverse). */
	float hdr_sdr_content_gamma;
	/* Paper white override in nits (scRGB: 80 nits == 1.0); 0 follows the
	 * OS SDR white level. */
	float                 hdr_paper_white_nits;
	AeronRenderLayer      render_layers[AERON_MAX_RENDER_LAYERS];
	uint16_t              render_submission_generation;
	AeronPixelLayerUpload pixel_layer_uploads[AERON_MAX_PIXEL_LAYER_UPLOADS];
	AeronPixelLayerUpload composition_pixel_upload;
	int                   render_layer_count;
	int                   pixel_layer_upload_count;
	/* Upload chunks that no command buffer owns. They are mapped with cycling
	 * when reused, so SDL picks a copy the GPU no longer reads. */
	AeronUploadChunk      upload_chunk_pool[AERON_UPLOAD_CHUNK_POOL_MAX];
	uint32_t              upload_chunk_pool_count;
	uint64_t              upload_chunk_pool_bytes;
	int                   logical_width;
	int                   logical_height;
	int                   window_aspect_width;
	int                   window_aspect_height;
	int                   window_aspect_pending;
	int                   presentation_pixel_width;
	int                   presentation_pixel_height;
	int                   relative_mouse_enabled;
	AeronPresentationMode presentation_mode;
	int                   presentation_vsync_divisor;
	SDL_DisplayID         presentation_display_id;
	double                display_refresh_hz;
	uint64_t              presentation_next_frame_us;
	float                 clear_color_rgba[4];
	AeronInputSnapshot    input;
	AeronControllerDevice controllers[AERON_CONTROLLER_MAX];
	uint64_t              last_frame_us;
	AeronVfs              vfs;
	char                  org_name[128];
	char                  app_name[128];
	char                  shader_root[AERON_MAX_PATH];
	char                  render_error[512];
	AeronRenderDataStats  render_data_stats;
	/* Frames submitted for presentation. The buffer readbacks count them to
	 * know when the GPU has finished an older command buffer. */
	uint64_t              presented_frames;
	int                   frames_in_flight;
	/* SDL_QueryGPUFence gives the right answer. */
	int fence_query_reliable;
	/* The fence of the newest command buffer submitted since the last
	 * present, which the present queries when its swapchain image arrives. */
	AeronSharedFence* pacing_fence;
	AeronPacingState  pacing;
} AeronRuntime;

extern AeronRuntime g_aeron;

void Aeron_CopyString(char* dst, size_t dst_size, const char* src);
void AeronVfs_DeinitInternal(AeronVfs* vfs);

void Aeron_RefreshPresentationTiming(void);
/* Latency pacing: the present reports how long it waited for its swapchain
 * image and whether the GPU work before it was done by then. */
void Aeron_ResetPacing(void);
int  Aeron_PacingActive(void);
void Aeron_NotePacingPresent(uint64_t acquire_wait_us, int gpu_done, uint64_t present_us);

int  Aeron_WindowInit(const AeronConfig* config);
void Aeron_WindowShutdown(void);
void Aeron_ApplyPendingWindowAspectRatio(void);
void Aeron_UpdatePresentationPixelSize(void);
void Aeron_ComputePresentationRect(int container_width, int container_height, SDL_Rect* rect);

int  Aeron_RenderBackendInit(void);
void Aeron_RenderBackendShutdown(void);
int  Aeron_WaitForPresentationSlot(void);

/* Re-reads the window's HDR headroom / SDR white level and queues a
 * composition re-evaluation. Called from the event handler when the display's
 * HDR state changes or the window moves to another display. */
void Aeron_OnOutputHdrStateChanged(void);
/* Applies a queued composition re-evaluation. Called from Aeron_BeginFrame
 * after the event pump, so the swapchain never flips mid-frame. */
void Aeron_ApplyPendingOutputHdr(void);

int  Aeron_AudioInit(void);
void Aeron_AudioShutdown(void);

void Aeron_BeginInputFrame(AeronInputSnapshot* input);
void Aeron_HandleEvent(const SDL_Event* event);
void Aeron_ControllersInit(void);
void Aeron_ControllersShutdown(void);
void Aeron_HandleControllerEvent(const SDL_Event* event);
void Aeron_UpdateControllers(AeronInputSnapshot* input);

void Aeron_InitVfs(const AeronConfig* config);
void Aeron_ClearRenderSubmissions(void);

/* ---- Debug UI internal hooks (src/debug_ui.cpp) ----
 * Real implementations only when built with AERON_DEBUG_UI; the macros
 * below keep the call sites unconditional. */
#ifdef AERON_DEBUG_UI
#ifdef __cplusplus
extern "C" {
#endif
void Aeron_DebugUiInitInternal(void);
void Aeron_DebugUiShutdownInternal(void);
/* Feed one SDL event; returns nonzero when the overlay consumed it and
 * the engine input layer should skip it. */
int Aeron_DebugUiHandleEvent(const SDL_Event* event);
/* Withhold captured input from the snapshot (mouse always while
 * visible; keyboard while an ImGui text field wants it). Called at the
 * end of Aeron_BeginFrame's input update. */
void Aeron_DebugUiFilterInput(AeronInputSnapshot* input);
/* Build the frame's UI (NewFrame -> menu bar + tools -> ImGui::Render).
 * Called from Aeron_Present before the command buffer is acquired. */
void Aeron_DebugUiBuildFrame(void);
/* Upload ImGui draw data (own copy pass; call before any render pass). */
void Aeron_DebugUiPrepareRender(SDL_GPUCommandBuffer* command_buffer);
/* Record ImGui draws into the open swapchain render pass. */
void Aeron_DebugUiRecordDraws(SDL_GPUCommandBuffer* command_buffer, SDL_GPURenderPass* render_pass);
/* Rebuild the ImGui GPU-backend pipeline against a new swapchain format. */
void Aeron_DebugUiOnSwapchainFormatChanged(SDL_GPUTextureFormat format);
#ifdef __cplusplus
}
#endif
#else
#define Aeron_DebugUiInitInternal() ((void)0)
#define Aeron_DebugUiShutdownInternal() ((void)0)
#define Aeron_DebugUiHandleEvent(event) 0
#define Aeron_DebugUiFilterInput(input) ((void)0)
#define Aeron_DebugUiBuildFrame() ((void)0)
#define Aeron_DebugUiPrepareRender(command_buffer) ((void)0)
#define Aeron_DebugUiRecordDraws(command_buffer, render_pass) ((void)0)
#define Aeron_DebugUiOnSwapchainFormatChanged(format) ((void)0)
#endif

#endif
