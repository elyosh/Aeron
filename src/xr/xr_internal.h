#ifndef AERON_XR_INTERNAL_H
#define AERON_XR_INTERNAL_H

/* Shared by the files in src/xr. Only these files include OpenXR headers
 * and call SDL's OpenXR functions. */

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

#endif /* AERON_XR */

#endif
