#ifndef AERON_INTERNAL_H
#define AERON_INTERNAL_H

#include "aeron/aeron.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdint.h>

#define AERON_MAX_PATH 1024
#ifdef _WIN32
#define AERON_PATH_SEPARATOR "\\"
#else
#define AERON_PATH_SEPARATOR "/"
#endif
#define AERON_MAX_DELTA_US 250000
/* The virtual screen of a VR session when the application sets nothing. */
#define AERON_XR_SCREEN_DEFAULT_WIDTH 1920
#define AERON_XR_SCREEN_DEFAULT_HEIGHT 1080
#define AERON_XR_SCREEN_DEFAULT_DISTANCE_M 2.0f
#define AERON_XR_SCREEN_DEFAULT_WIDTH_M 2.4f
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

#define AERON_MAX_RENDER_SHUTDOWN_HOOKS 8

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
	/* The SDL texture belongs to someone else, such as an OpenXR swapchain.
	 * Aeron draws into it, and never cycles, samples, reads back or releases
	 * it. */
	int external;
	/* The list of textures that are not destroyed yet, which the shutdown
	 * logs, and the name the texture was created with. */
	AeronTexture* live_prev;
	AeronTexture* live_next;
	char          debug_name[48];
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
	AeronComputePass          active_compute_pass;
	int                       compute_pass_active;
	int                       render_pass_active;
	int                       owns_wrapper;
	int                       immediate_upload;
	int                       failed;
	char                      failure_message[512];
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
	int initialized;
	int quit_requested;
	int fatal_error_requested;
	/* From the application's startup options. */
	AeronXrMode         xr_mode;
	AeronStartupOptions xr_options;
	/* Why VR did not start, or empty. */
	char                        xr_start_error[512];
	SDL_Window*                 window;
	SDL_GPUDevice*              gpu_device;
	SDL_GPUShader*              fullscreen_vertex_shader;
	SDL_GPUShader*              fullscreen_fragment_shader;
	AeronFullscreenPipeline     fullscreen_pipelines[16];
	int                         fullscreen_pipeline_count;
	SDL_GPUSampler*             pixel_frame_sampler;
	SDL_GPUSampler*             pixel_frame_nearest_sampler;
	SDL_GPUTextureFormat        swapchain_format;
	SDL_GPUSwapchainComposition swapchain_composition;
	/* HDR output: the mode the app asked for, the mode actually running, and
	 * the display properties cached from the last HDR-state event. The desired
	 * flag is kept so the composition can be re-applied when a display becomes
	 * HDR-capable (or stops being one) while the app runs. */
	int hdr_output_desired;
	int hdr_output_enabled;
	/* The window's present mode: vsync, except while a VR session runs. */
	SDL_GPUPresentMode present_mode;
	/* A VR session runs and the window can't present without waiting for
	 * its display, so the window isn't drawn. */
	int   window_draw_disabled;
	/* When the window draws its next frame in VR (Aeron_XrWindowFrameDue),
	 * or 0 for the next frame of the headset. */
	uint64_t xr_window_next_us;
	/* The virtual screen: with VR, the layers are composed into this
	 * texture instead of the window. The headset shows it as a flat screen
	 * and the window shows a copy. NULL without VR. */
	SDL_GPUTexture*      xr_screen_texture;
	SDL_GPUTextureFormat xr_screen_format;
	int                  xr_screen_width;
	int                  xr_screen_height;
	/* The screen holds a picture, and its empty parts are transparent. */
	int xr_screen_has_content;
	int xr_screen_transparent;
	/* Aeron_XrSetScreenScale; 0 means 1. */
	float xr_screen_scale;
	/* The pointer drawn on the virtual screen in the headset. */
	SDL_GPUTexture* xr_pointer_texture;
	/* The eye mirror of this frame (Aeron_XrSubmitMirror). */
	int                           xr_mirror_submitted;
	AeronSwapchainRenderLayerDesc xr_mirror;
	/* The pointer is confined to this rectangle of the window. */
	int      mouse_confined;
	SDL_Rect mouse_confine_rect;
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
	AeronUploadChunk upload_chunk_pool[AERON_UPLOAD_CHUNK_POOL_MAX];
	uint32_t         upload_chunk_pool_count;
	uint64_t         upload_chunk_pool_bytes;
	int              logical_width;
	int              logical_height;
	int              window_aspect_width;
	int              window_aspect_height;
	int              window_aspect_pending;
	/* The window mode last asked for, and the window's position and size
	 * before it spanned the displays. */
	int                   window_mode;
	int                   window_spanned;
	SDL_Rect              window_rect_before_span;
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
	uint64_t presented_frames;
	/* The pipelines built since the frame began: how many, the time they
	 * took and the names of the first ones (Aeron_LogFramePipelines). */
	uint32_t frame_pipeline_count;
	uint64_t frame_pipeline_ns;
	char     frame_pipeline_names[192];
	int      frames_in_flight;
	/* SDL_QueryGPUFence gives the right answer. */
	int fence_query_reliable;
	/* Block-compressed uploads need regions in whole 4x4 blocks. */
	int upload_whole_blocks;
	/* The Vulkan device has the shaderFloat16 and shaderInt16 features. */
	int vulkan_shader_fp16;
	/* Textures, render targets and depth targets that are not destroyed yet. */
	AeronTexture* live_textures;
	void (*render_shutdown_hooks[AERON_MAX_RENDER_SHUTDOWN_HOOKS])(void);
	int          render_shutdown_hook_count;
	SDL_SpinLock live_textures_lock;
	/* The fence of the newest command buffer submitted since the last
	 * present, which the present queries when its swapchain image arrives. */
	AeronSharedFence* pacing_fence;
	/* Fences that Aeron no longer needs and the GPU hasn't signaled yet. They
	 * are released once signaled. */
	SDL_GPUFence**   retired_fences;
	uint32_t         retired_fence_count;
	uint32_t         retired_fence_capacity;
	AeronPacingState pacing;
} AeronRuntime;

extern AeronRuntime g_aeron;

/* Logs the pipelines built since the last call, if any, in one line, and
 * starts counting again. Building a pipeline compiles its shaders in the
 * driver, which can take long, so a pipeline built during play can make a
 * frame miss its time. Aeron_BeginFrame calls it, so that each frame that
 * built pipelines gets its own line. */
void Aeron_LogFramePipelines(void);

void Aeron_CopyString(char* dst, size_t dst_size, const char* src);
void AeronVfs_DeinitInternal(AeronVfs* vfs);

void Aeron_RefreshPresentationTiming(void);
/* Latency pacing: the present reports how long it waited for its swapchain
 * image and whether the GPU work before it was done by then. */
void Aeron_ResetPacing(void);
int  Aeron_PacingActive(void);
void Aeron_NotePacingPresent(uint64_t acquire_wait_us, int gpu_done, uint64_t present_us);
/* In VR, whether the window draws this frame of the headset. The window
 * draws no more frames than its display shows. */
int Aeron_XrWindowFrameDue(void);

int  Aeron_WindowInit(const AeronConfig* config);
void Aeron_WindowShutdown(void);
void Aeron_ApplyPendingWindowAspectRatio(void);
void Aeron_UpdatePresentationPixelSize(void);
/* Applies the span window mode again after a display was added, removed or
 * moved. */
void Aeron_OnDisplaysChanged(void);
void Aeron_ComputePresentationRect(int container_width, int container_height, SDL_Rect* rect);

int  Aeron_RenderBackendInit(void);
void Aeron_RenderBackendShutdown(void);

/* VR through OpenXR (src/xr). The render backend calls these around the GPU
 * device, in this order. In a build without AERON_XR, AeronXr_PrepareDevice
 * fails and the others do nothing.
 *
 * AeronXr_PrepareDevice finds the OpenXR runtime. It returns 0, with the
 * reason in `error`, when there is none.
 * AeronXr_SetDeviceProperties makes SDL create the device for OpenXR.
 * AeronXr_DeviceCreated creates the session on the new device. It returns 0,
 * with the reason in `error`, when the device can't be used for VR.
 * AeronXr_DeviceCreateFailed is called instead when SDL created no device. */
int  AeronXr_PrepareDevice(char* error, size_t error_size);
int  AeronXr_SetDeviceProperties(SDL_PropertiesID properties);
int  AeronXr_DeviceCreated(SDL_GPUDevice* device, char* error, size_t error_size);
void AeronXr_DeviceCreateFailed(void);
/* Reads the runtime's events and runs the session: it begins the session
 * when the headset is ready and ends it when the runtime stops it.
 * Aeron_BeginFrame calls it. */
void AeronXr_PollEvents(void);
/* While the session runs: waits for the headset's next frame, then ends it.
 * Aeron_XrBeginRendering begins it in between, and AeronXr_EndFrame begins
 * it first when nothing did. These replace the swapchain wait of the window:
 * OpenXR paces the application. They do nothing when no session runs. They
 * return 0 when the session failed; the application is then asked to
 * stop. */
int AeronXr_BeginFrame(void);
int AeronXr_EndFrame(void);
/* The render backend's part of a session start or stop: the window stops
 * waiting for its display's vsync while OpenXR paces the application. */
void Aeron_OnXrSessionRunningChanged(int running);
/* Before SDL_DestroyGPUDevice, with the GPU idle. Also destroys the
 * swapchains. */
void AeronXr_DestroySession(SDL_GPUDevice* device);
/* Whether this frame's eye images are complete (Aeron_XrSubmitViews). */
int AeronXr_ViewsSubmitted(void);
/* The time between two frames of the headset, in microseconds, or 0 when
 * it isn't known. */
uint64_t AeronXr_FramePeriodUs(void);
/* Acquires this frame's image of the virtual screen, which the headset
 * shows as a flat screen. Returns NULL when the headset shows nothing this
 * frame. The image is released when the frame ends. */
SDL_GPUTexture* AeronXr_AcquireScreenImage(SDL_GPUTextureFormat* format, int* width, int* height);
/* Says whether the headset shows the virtual screen this frame, and
 * whether the eye images show through its empty parts. */
void AeronXr_SetScreenLayer(int visible, int transparent);

/* On a runtime that draws flat layers over the eye images whatever their
 * order, the virtual screen and the panel are drawn into the eye images
 * (xr_internal.h, `composite`). Whether this frame needs it: */
int AeronXr_CompositeWanted(void);
/* The size and format of the headset's image of the virtual screen. */
int AeronXr_ScreenImageInfo(SDL_GPUTextureFormat* format, int* width, int* height);

/* One eye's image and where the eye is: its pose in the rig's space and the
 * tangents of its field of view's four edges. Returns 0 when the eye has no
 * image in this frame. */
typedef struct AeronXrCompositeView {
	SDL_GPUTexture*      texture;
	SDL_GPUTextureFormat format;
	int                  width, height;
	float                position[3];
	float                orientation[4];
	float                tan_left, tan_right, tan_up, tan_down;
} AeronXrCompositeView;

int AeronXr_CompositeView(int eye, AeronXrCompositeView* out);
/* A flat surface to draw into the eye images: its centre in the rig's
 * space, and the vectors from the centre to its right edge and to its top
 * edge. `which` is 0 for the panel and 1 for the virtual screen. `texture`
 * gets the panel's picture; the screen's is the caller's. Returns 0 when
 * the surface isn't shown in this frame. */
int AeronXr_CompositeSurface(int which, float center[3], float right[3], float up[3],
							 SDL_GPUTexture** texture);
/* The eye images of this frame hold the screen and the panel. */
void AeronXr_SetComposited(void);
/* Copies the panel's picture into its swapchain image, for a runtime that
 * shows it as a layer. Call it once in each frame, outside any pass. */
void AeronXr_PresentPanel(SDL_GPUCommandBuffer* command_buffer);
/* After SDL_DestroyGPUDevice. Also unloads the OpenXR loader. */
void AeronXr_DestroyInstance(void);
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
/* SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED: rereads the device's buffer length
 * when `device_id` is the mixer's device. SDL keeps converting the mixer's
 * stream, so only the cursors' latency changes. */
void Aeron_AudioHandleDeviceFormatChanged(uint32_t device_id);
/* Writes what the mixer recorded since the last call to the recording's file
 * (Aeron_AudioRecordStart). Aeron_BeginFrame calls it. */
void Aeron_AudioRecordFlush(void);
/* The frames by which the output limiter delays everything mixed. */
int Aeron_AudioOutputDelayFrames(void);

/* The mixer without an output device, for tests and benchmarks.
 * Aeron_AudioInitHeadless sets up the mixer as Aeron_AudioInit does, but
 * opens no device. Every mixed frame then counts as played as soon as it
 * leaves the limiter's delay. Shut it down with Aeron_AudioShutdown. Fails if
 * audio is already initialized. */
int Aeron_AudioInitHeadless(void);
/* Mixes `frames` frames into `out`, interleaved with Aeron_AudioOutputChannels
 * channels, in steps of `chunk_frames` (at most the mixer's own chunk size),
 * as the device callback does. The samples are the F32 ones a device would
 * receive, after the limiter, so they come out Aeron_AudioOutputDelayFrames
 * frames after they are mixed. Returns 0 unless the mixer was set up by
 * Aeron_AudioInitHeadless. */
int Aeron_AudioRenderHeadless(float* out, int frames, int chunk_frames);

/* For tests: the mixer mixes positional voices as jobs, outside its mutex.
 * In each chunk, `hook` runs just before the jobs mix (stage
 * AERON_AUDIO_JOB_HOOK_BEFORE) and just after, before their results go back
 * to the voices (AERON_AUDIO_JOB_HOOK_AFTER). It runs on the thread that
 * mixes, without the mutex, so it can call the audio API. NULL removes it. */
enum { AERON_AUDIO_JOB_HOOK_BEFORE = 0, AERON_AUDIO_JOB_HOOK_AFTER = 1 };

typedef void (*AeronAudioJobHook)(int stage, void* user);
void Aeron_AudioSetJobHook(AeronAudioJobHook hook, void* user);
/* For tests: how many Aeron_AudioClipWrite calls are waiting for a job to
 * end. */
int Aeron_AudioClipWritersWaiting(void);
/* For tests: turns the skipping of silent voices on or off (it is on by
 * default), and counts the voice chunks that were skipped. The output is the
 * same either way. */
void     Aeron_AudioSetSilentSkip(int enabled);
uint64_t Aeron_AudioSilentSkips(void);
/* Loads an HRTF data set from memory, as Aeron_AudioLoadHrtf does from a
 * file. NULL loads the built-in data set. */
int Aeron_AudioLoadHrtfData(const void* data, size_t size, char* error, size_t error_size);

void Aeron_BeginInputFrame(AeronInputSnapshot* input);
void Aeron_HandleEvent(const SDL_Event* event);
void Aeron_ControllersInit(void);
void Aeron_ControllersShutdown(void);
void Aeron_HandleControllerEvent(const SDL_Event* event);
void Aeron_UpdateControllers(AeronInputSnapshot* input);

/* The two VR hands (src/xr/xr_input.c). They use the last two controller
 * slots, which SDL's devices never get. */
#define AERON_CONTROLLER_SDL_MAX (AERON_CONTROLLER_MAX - 2)
/* Reads the hands from the headset, or the fake hands when there is no VR
 * session, and fills their two entries of the input snapshot. It runs once
 * in each frame, after AeronXr_BeginFrame. */
void AeronXr_UpdateHands(AeronInputSnapshot* input);
/* Reads the head tracker (head_tracker.c). It runs once in each frame, after
 * AeronXr_BeginFrame, and gives no pose while a VR session runs. */
void AeronHeadTracker_Update(void);
/* Stops reading the head tracker and chooses none. */
void AeronHeadTracker_Shutdown(void);
/* The hand that has this instance ID: 0 for the left one, 1 for the right
 * one, or -1 when the ID belongs to no hand. */
int AeronXr_HandFromInstance(uint32_t instance_id);
/* Vibrates a hand's controller, as Aeron_RumbleController does for a
 * joystick. The stronger of the two motor values gives the strength. */
int AeronXr_RumbleHand(int hand, uint16_t low_frequency_rumble, uint16_t high_frequency_rumble,
					   uint32_t duration_ms);
int AeronXr_HandHasRumble(int hand);
/* Moves the snapshot's mouse to where a hand points on the virtual screen,
 * and makes the hand's trigger and squeeze its buttons. It runs after the
 * real mouse was read. */
void AeronXr_UpdatePointer(AeronInputSnapshot* input);
/* Nonzero while a hand moves the mouse. */
int AeronXr_PointerActive(void);
/* Remembers the panel for Aeron_XrPanelHit; NULL forgets it. */
struct AeronXrPanelDesc;
void AeronXr_SetPanel(const struct AeronXrPanelDesc* desc);

void Aeron_InitVfs(const AeronConfig* config);

/* Starts the session log file and crash handler once per process, both writing to the
 * user root's diagnostics/ directory. Safe before SDL_Init. */
void Aeron_DiagnosticsInit(const AeronConfig* config);
/* Deletes all but the newest keep files in directory matching pattern; names must sort by age. */
void Aeron_DiagnosticsPrune(const char* directory, const char* pattern, int keep);
/* Mirrors SDL log output into directory/<app_name>-<date>-<time>.log, pruning old sessions. */
void Aeron_LogFileInit(const char* directory, const char* app_name);
/* Writes the name and version of the operating system to `out`. */
void Aeron_SystemDescribeOs(char* out, size_t out_size);
/* Logs the processor, memory and preferred locales. */
void Aeron_LogSystemInfo(void);
/* Native HANDLE of the open log file on Windows, else NULL. Used by the crash handler. */
void* Aeron_LogFileNativeHandle(void);
/* File descriptor of the open log file on POSIX, else -1. Used by the crash handler. */
int Aeron_LogFileDescriptor(void);
#if defined(_WIN32) || defined(__linux__)
#define AERON_HAS_CRASH_HANDLER 1
/* Installs process-wide crash reporting into the diagnostics directory: minidumps
 * on Windows, a signal handler appending a backtrace to the session log on Linux. */
void Aeron_CrashHandlerInit(const char* directory, const char* app_name, const char* app_version);
#endif
void Aeron_ClearRenderSubmissions(void);
/* The relaunch of Aeron_RequestRelaunch (src/relaunch.c). Aeron_Shutdown calls
 * the first one before it closes the window, and the second one after. */
void Aeron_RelaunchBeforeWindowClose(void);
void Aeron_RelaunchRun(void);
void Aeron_RelaunchCancel(void);

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
