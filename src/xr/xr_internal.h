#ifndef AERON_XR_INTERNAL_H
#define AERON_XR_INTERNAL_H

/* Shared by the files in src/xr. Only these files include OpenXR headers
 * and call SDL's OpenXR functions. The rest of aeron uses the AeronXr_*
 * functions declared in src/internal.h. */

#include "aeron/xr.h"

#if AERON_XR

/* The vendored OpenXR headers come first: SDL_openxr.h then uses their
 * types and leaves its own short copies out. */
#include <openxr/openxr.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_openxr.h>

/* What the installed OpenXR runtime offers, read before an instance exists. */
typedef struct AeronXrRuntimeInfo {
	uint32_t extension_count;
	int      has_visibility_mask; /* XR_KHR_visibility_mask */
} AeronXrRuntimeInfo;

/* Loads the OpenXR loader through SDL and lists the runtime's extensions.
 * On success the loader stays loaded, and the caller balances this call
 * with AeronXr_UnloadRuntime. Returns 0, with the reason in `error`, when
 * the loader or the runtime is missing. */
int  AeronXr_LoadRuntime(AeronXrRuntimeInfo* out, char* error, size_t error_size);
void AeronXr_UnloadRuntime(void);

/* The OpenXR functions that aeron calls. SDL creates the instance, so aeron
 * gets them from the loader with xrGetInstanceProcAddr. */
#define AERON_XR_FUNCTIONS(X)            \
	X(xrDestroyInstance)                 \
	X(xrGetInstanceProperties)           \
	X(xrGetSystemProperties)             \
	X(xrResultToString)                  \
	X(xrDestroySession)                  \
	X(xrCreateReferenceSpace)            \
	X(xrDestroySpace)                    \
	X(xrEnumerateViewConfigurations)     \
	X(xrEnumerateViewConfigurationViews) \
	X(xrEnumerateEnvironmentBlendModes)  \
	X(xrPollEvent)                       \
	X(xrBeginSession)                    \
	X(xrEndSession)                      \
	X(xrWaitFrame)                       \
	X(xrBeginFrame)                      \
	X(xrEndFrame)                        \
	X(xrLocateSpace)

typedef struct AeronXrFunctions {
#define AERON_XR_FUNCTION_FIELD(name) PFN_##name name;
	AERON_XR_FUNCTIONS(AERON_XR_FUNCTION_FIELD)
#undef AERON_XR_FUNCTION_FIELD
} AeronXrFunctions;

/* Fills `out` with the functions of `instance`. Returns 0, with the missing
 * function named in `error`, when the loader doesn't have one of them. */
int AeronXr_LoadFunctions(XrInstance instance, AeronXrFunctions* out, char* error, size_t error_size);

/* The life of a session, as the runtime's events drive it (xr_state.c).
 * It uses no global state, so a test can run it with fake functions. */
typedef struct AeronXrSessionMachine {
	XrSession      session;
	XrSessionState state;
	/* xrBeginSession succeeded and xrEndSession was not called since. Frames
	 * are waited for and submitted only while this is set. */
	int running;
	/* The session has the user's input focus. */
	int focused;
	/* The runtime asked the application to quit. */
	int exit_requested;
	/* Changes when the views' history must be reset: after a recentre or
	 * when the runtime moves the reference space. */
	uint64_t generation;
} AeronXrSessionMachine;

/* Handles one event from xrPollEvent. It begins the session when the
 * runtime is ready and ends it when the runtime stops it. Returns 0, with
 * the reason in `error`, when the session or the instance is lost, or when
 * a call fails: the application can't continue in VR. */
int AeronXrSession_HandleEvent(AeronXrSessionMachine* machine, const AeronXrFunctions* fn,
							   const XrEventDataBuffer* event, char* error, size_t error_size);

/* Computes the origin that a recentre gives the rig, from the pose of the
 * head: the head's position, and its yaw alone. The origin stays level, so
 * the head keeps its pitch and roll relative to it. */
void AeronXr_LevelOrigin(const XrPosef* head, XrPosef* origin);

/* The OpenXR state of the GPU device, shared by xr_session.c and xr_frame.c. */
typedef struct AeronXrState {
	/* The loader is loaded and `runtime` is valid. */
	int                runtime_loaded;
	AeronXrRuntimeInfo runtime;
	/* The extensions asked from the runtime. SDL reads the list while it
	 * creates the device. */
	const char* extensions[1];
	uint32_t    extension_count;

	/* SDL writes these two while it creates the device. */
	XrInstance instance;
	XrSystemId system_id;

	AeronXrFunctions      fn;
	AeronXrSessionMachine machine;
	/* The runtime's fixed space, the head, and the space that the views are
	 * located in. The last one is the fixed space moved to the rig's origin;
	 * a recentre creates it again. */
	XrSpace local_space;
	XrSpace view_space;
	XrSpace rig_space;
	int     recenter_pending;
	/* The session failed. No OpenXR call is made after that, and the
	 * application was asked to stop with an error. */
	int failed;
	/* How the headset shows the eye images over the real world. */
	XrEnvironmentBlendMode blend_mode;

	/* A frame is open between xrBeginFrame and xrEndFrame. */
	int    frame_open;
	XrTime frame_display_time;
	int    frame_should_render;

	/* The size the runtime recommends for one eye image, and the largest
	 * size it accepts. Both eyes use the same size. */
	int view_width, view_height;
	int view_max_width, view_max_height;
	/* The format of the eye images. It is an sRGB format: the tone map writes
	 * linear values and the format encodes them. */
	SDL_GPUTextureFormat color_format;
} AeronXrState;

extern AeronXrState g_xr;

/* Writes "<what>: <the result's name>" into `error`. */
void AeronXr_ResultError(char* error, size_t error_size, const char* what, XrResult result);
/* Stops using the session and asks the application to stop with this error. */
void AeronXr_Fail(const char* message);

#endif /* AERON_XR */

#endif
