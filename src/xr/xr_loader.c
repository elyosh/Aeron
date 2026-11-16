/* Finds the OpenXR runtime. SDL loads the OpenXR loader library by name, so
 * aeron does not link it. */

#include "xr_internal.h"

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
	if (result == XR_ERROR_RUNTIME_UNAVAILABLE) {
		snprintf(error, error_size, "no OpenXR runtime is installed or active");
		SDL_OpenXR_UnloadLibrary();
		return 0;
	}
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

int AeronXr_LoadFunctions(XrInstance instance, AeronXrFunctions* out, char* error, size_t error_size) {
	PFN_xrGetInstanceProcAddr get_proc = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	memset(out, 0, sizeof *out);
	if (!get_proc) {
		snprintf(error, error_size, "the OpenXR loader has no xrGetInstanceProcAddr");
		return 0;
	}
#define AERON_XR_LOAD_FUNCTION(name)                                                               \
	if (XR_FAILED(get_proc(instance, #name, (PFN_xrVoidFunction*)&out->name)) || !out->name) { \
		snprintf(error, error_size, "the OpenXR runtime has no %s", #name);                     \
		return 0;                                                                               \
	}
	AERON_XR_FUNCTIONS(AERON_XR_LOAD_FUNCTION)
#undef AERON_XR_LOAD_FUNCTION
	return 1;
}
