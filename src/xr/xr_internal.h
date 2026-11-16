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
	X(xrEnumerateViewConfigurationViews)

typedef struct AeronXrFunctions {
#define AERON_XR_FUNCTION_FIELD(name) PFN_##name name;
	AERON_XR_FUNCTIONS(AERON_XR_FUNCTION_FIELD)
#undef AERON_XR_FUNCTION_FIELD
} AeronXrFunctions;

/* Fills `out` with the functions of `instance`. Returns 0, with the missing
 * function named in `error`, when the loader doesn't have one of them. */
int AeronXr_LoadFunctions(XrInstance instance, AeronXrFunctions* out, char* error, size_t error_size);

#endif /* AERON_XR */

#endif
