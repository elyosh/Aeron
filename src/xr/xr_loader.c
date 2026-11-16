/* Finds the OpenXR runtime. SDL loads the OpenXR loader library by name, so
 * aeron does not link it. */

#include "xr_internal.h"

#include "aeron/log.h"

#if AERON_XR

#include <stdio.h>
#include <string.h>

int AeronXr_LoadRuntime(AeronXrRuntimeInfo* out, char* error, size_t error_size) {
	memset(out, 0, sizeof *out);
	if (!SDL_OpenXR_LoadLibrary()) {
		snprintf(error, error_size, "the OpenXR loader was not found (%s)", SDL_GetError());
		return 0;
	}

	/* The loader answers these two calls without an instance. */
	PFN_xrGetInstanceProcAddr get_proc = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	PFN_xrVoidFunction        function = NULL;
	XrResult                  result   = XR_ERROR_FUNCTION_UNSUPPORTED;
	if (get_proc)
		result = get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function);
	if (XR_FAILED(result) || !function) {
		snprintf(error, error_size, "the OpenXR loader has no xrEnumerateInstanceExtensionProperties");
		SDL_OpenXR_UnloadLibrary();
		return 0;
	}
	PFN_xrEnumerateInstanceExtensionProperties enumerate = (PFN_xrEnumerateInstanceExtensionProperties)function;

	uint32_t count = 0;
	result         = enumerate(NULL, 0, &count, NULL);
	if (XR_FAILED(result)) {
		snprintf(error, error_size, "no OpenXR runtime answered (XrResult %d)", (int)result);
		SDL_OpenXR_UnloadLibrary();
		return 0;
	}
	XrExtensionProperties* properties = count ? SDL_calloc(count, sizeof *properties) : NULL;
	if (count && !properties) {
		snprintf(error, error_size, "out of memory");
		SDL_OpenXR_UnloadLibrary();
		return 0;
	}
	for (uint32_t i = 0; i < count; i++)
		properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	if (count)
		result = enumerate(NULL, count, &count, properties);
	if (XR_FAILED(result)) {
		snprintf(error, error_size, "the OpenXR runtime did not list its extensions (XrResult %d)", (int)result);
		SDL_free(properties);
		SDL_OpenXR_UnloadLibrary();
		return 0;
	}

	out->extension_count = count;
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(properties[i].extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) == 0)
			out->has_visibility_mask = 1;
	}
	SDL_free(properties);
	return 1;
}

void AeronXr_UnloadRuntime(void) {
	SDL_OpenXR_UnloadLibrary();
}

int Aeron_XrProbeRuntime(void) {
	AeronXrRuntimeInfo info;
	char               error[256];
	if (!AeronXr_LoadRuntime(&info, error, sizeof error)) {
		Aeron_LogInfo("xr", "OpenXR is not available: %s", error);
		return 0;
	}
	Aeron_LogInfo("xr", "OpenXR runtime found: %u extensions, visibility mask %s", info.extension_count,
				  info.has_visibility_mask ? "yes" : "no");
	AeronXr_UnloadRuntime();
	return 1;
}

#else /* !AERON_XR */

int Aeron_XrProbeRuntime(void) {
	Aeron_LogInfo("xr", "OpenXR is not available: this build has no VR support");
	return 0;
}

#endif
