#ifndef AERON_TEMPORAL_H
#define AERON_TEMPORAL_H

#include "aeron/render.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed FSR 3.1.4 modes supported by Aeron. Native AA keeps render and
 * output resolution equal. Dynamic resolution and Ultra Performance are
 * intentionally outside the public integration contract. */
typedef enum AeronTemporalMode {
	AERON_TEMPORAL_OFF = 0,
	AERON_TEMPORAL_NATIVE_AA,
	AERON_TEMPORAL_QUALITY,
	AERON_TEMPORAL_BALANCED,
	AERON_TEMPORAL_PERFORMANCE
} AeronTemporalMode;

typedef struct AeronTemporalUpscaler AeronTemporalUpscaler;

typedef struct AeronTemporalUpscalerDesc {
	uint32_t max_render_width;
	uint32_t max_render_height;
	uint32_t max_output_width;
	uint32_t max_output_height;
	/* Intended scene output format. RGBA16F contexts may expose FSR's
	 * internal history directly when sharpening and debug view are disabled. */
	AeronTextureFormat output_format;
	/* Allocate a second shared motion target so game-motion frames can be
	 * retained while FSR continues to process presentation-only frames. */
	int retain_motion_vectors;
	/* Enables AMD's argument validation and warning callback. */
	int debug_checking;
} AeronTemporalUpscalerDesc;

typedef struct AeronTemporalDispatchDesc {
	AeronCommandBuffer* command_buffer;

	/* HDR scene color at render resolution. It must support sampled and
	 * read-only compute access. */
	AeronTexture* color;
	/* Reversed-Z infinite depth copied/exported into an R32_FLOAT color texture
	 * at render resolution. Native depth-target formats are not storage textures
	 * on every SDL_GPU backend. */
	AeronTexture* depth;
	/* Unjittered, low-resolution motion vectors in pixel units. */
	AeronTexture* motion_vectors;
	/* Optional R8 reactive and transparency/composition masks. */
	AeronTexture* reactive;
	AeronTexture* transparency_and_composition;
	/* Optional 1x1 R32G32_FLOAT exposure texture. */
	AeronTexture* exposure;
	/* HDR destination at output resolution with compute-write usage. */
	AeronTexture* output;

	uint32_t render_width;
	uint32_t render_height;
	uint32_t output_width;
	uint32_t output_height;

	/* Exact pixel-space jitter applied to this frame's projection. */
	float jitter_x;
	float jitter_y;
	/* Converts the supplied motion-vector values to render-resolution pixels. */
	float motion_vector_scale_x;
	float motion_vector_scale_y;

	float frame_time_delta_ms;
	float pre_exposure;
	/* Physical near-plane distance for the reversed infinite projection. */
	float camera_near;
	float camera_vertical_fov_radians;
	float view_space_to_meters;
	float sharpness;
	int   enable_sharpening;
	int   reset_history;
	int   debug_view;
	/* Selects the retained shared-motion target for this dispatch. */
	int update_retained_motion_vectors;
} AeronTemporalDispatchDesc;

/* Read-only diagnostics for the automatically selected FSR shader profile. */
typedef struct AeronTemporalProfileInfo {
	uint32_t    manifest_schema;
	const char* manifest_hash;
	const char* profile_name;
	const char* backend_driver;
	const char* fallback_reason;
	const char* atomic_layout;
	uint32_t    pipeline_recreation_count;
	int         fp16;
	int         wave_spd;
	int         lanczos_lut;
	int         direct_history_output;
} AeronTemporalProfileInfo;

/* Creates the reusable FSR 3.1.4 context and its persistent history resources. */
AeronTemporalUpscaler* AeronTemporalUpscaler_Create(const AeronTemporalUpscalerDesc* desc);

/* The caller must ensure GPU work using the context has completed first. */
void AeronTemporalUpscaler_Destroy(AeronTemporalUpscaler* upscaler);

/* Records all FSR passes into desc->command_buffer. */
int AeronTemporalUpscaler_Dispatch(AeronTemporalUpscaler* upscaler, const AeronTemporalDispatchDesc* desc);

/* Borrowed render-resolution resources produced by the most recent successful
 * dispatch. Both return NULL before the first success or after a failed
 * dispatch, and remain owned by the upscaler. */
AeronTexture* AeronTemporalUpscaler_DilatedDepth(const AeronTemporalUpscaler* upscaler);
AeronTexture* AeronTemporalUpscaler_DilatedMotionVectors(const AeronTemporalUpscaler* upscaler);
/* Borrowed output-resolution history from the most recent eligible successful
 * dispatch. Its alpha channel is FSR lock state; downstream consumers must use
 * RGB only and must not modify the target. */
AeronRenderTarget* AeronTemporalUpscaler_OutputTarget(const AeronTemporalUpscaler* upscaler);
int                AeronTemporalUpscaler_UsesDirectHistory(const AeronTemporalUpscaler* upscaler);
/* Last successfully updated game-motion field. It survives successful scratch
 * dispatches and returns NULL until a retained update succeeds. */
AeronTexture* AeronTemporalUpscaler_RetainedDilatedMotionVectors(const AeronTemporalUpscaler* upscaler);
void          AeronTemporalUpscaler_InvalidateRetainedMotionVectors(AeronTemporalUpscaler* upscaler);

/* Last validation/backend failure for this context. Empty after success. */
const char* AeronTemporalUpscaler_LastError(const AeronTemporalUpscaler* upscaler);

/* Returns nonzero and fills profile diagnostics for a valid context. Strings
 * remain owned by Aeron and are valid for the context lifetime. */
int AeronTemporalUpscaler_GetProfileInfo(const AeronTemporalUpscaler* upscaler,
										 AeronTemporalProfileInfo*    info);

/* Shared YAML spelling and fixed-resolution helpers. */
const char* AeronTemporal_ModeName(AeronTemporalMode mode);
int         AeronTemporal_ParseMode(const char* text, AeronTemporalMode* mode);

/* Resolution and Halton(2,3) jitter helpers supplied by FSR 3.1.4. */
int AeronTemporal_GetRenderResolution(AeronTemporalMode mode, uint32_t output_width, uint32_t output_height,
									  uint32_t* render_width, uint32_t* render_height);
/* AMD's recommended world-texture bias. OFF returns zero because FSR is not
 * consuming the image; active modes use log2(render/output) - 1. */
float AeronTemporal_GetMipLodBias(AeronTemporalMode mode, uint32_t render_width, uint32_t output_width);
int   AeronTemporal_GetJitter(uint64_t frame_index, uint32_t render_width, uint32_t output_width,
							  float* jitter_x, float* jitter_y);

#ifdef __cplusplus
}
#endif

#endif
