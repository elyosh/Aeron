/* The OpenXR instance, session and reference space of the GPU device.
 *
 * SDL creates the instance together with the GPU device, because the runtime
 * takes part in creating the device. Aeron then creates the session on that
 * device. The frame loop and the eye images are not here yet, so the frame
 * functions report that there is nothing to render. */

#include "../internal.h"
#include "xr_internal.h"

#include <string.h>

#if AERON_XR

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

	AeronXrFunctions fn;
	XrSession        session;
	/* The space that the views are located in. */
	XrSpace local_space;

	/* The size the runtime recommends for one eye image, and the largest
	 * size it accepts. Both eyes use the same size. */
	int view_width, view_height;
	int view_max_width, view_max_height;
	/* The format of the eye images. It is an sRGB format: the tone map writes
	 * linear values and the format encodes them. */
	SDL_GPUTextureFormat color_format;
} AeronXrState;

static AeronXrState g_xr;

/* Writes "<what>: <the result's name>" into `error`. */
static void xr_result_error(char* error, size_t error_size, const char* what, XrResult result) {
	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (g_xr.instance && g_xr.fn.xrResultToString)
		g_xr.fn.xrResultToString(g_xr.instance, result, name);
	if (name[0])
		SDL_snprintf(error, error_size, "%s: %s", what, name);
	else
		SDL_snprintf(error, error_size, "%s: XrResult %d", what, (int)result);
}

int AeronXr_PrepareDevice(char* error, size_t error_size) {
	if (g_xr.runtime_loaded)
		return 1;
	if (!AeronXr_LoadRuntime(&g_xr.runtime, error, error_size))
		return 0;
	g_xr.runtime_loaded = 1;
	/* An extension that the runtime doesn't have makes SDL's device creation
	 * fail, so only ask for the ones it lists. */
	g_xr.extension_count = 0;
	if (g_xr.runtime.has_visibility_mask)
		g_xr.extensions[g_xr.extension_count++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
	return 1;
}

int AeronXr_SetDeviceProperties(SDL_PropertiesID properties) {
	g_xr.instance  = XR_NULL_HANDLE;
	g_xr.system_id = XR_NULL_SYSTEM_ID;
	return SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true) &&
		   SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER, &g_xr.instance) &&
		   SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER,
								  &g_xr.system_id) &&
		   SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_ENGINE_NAME_STRING, "Aeron") &&
		   SDL_SetNumberProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_COUNT_NUMBER,
								 g_xr.extension_count) &&
		   SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_NAMES_POINTER,
								  (void*)g_xr.extensions);
}

/* Reads the size of the eye images from the stereo view configuration. */
static int xr_read_view_configuration(char* error, size_t error_size) {
	XrViewConfigurationType types[8];
	uint32_t                type_count = 0;
	XrResult                result =
		g_xr.fn.xrEnumerateViewConfigurations(g_xr.instance, g_xr.system_id, SDL_arraysize(types),
											  &type_count, types);
	if (XR_FAILED(result)) {
		xr_result_error(error, error_size, "could not list the view configurations", result);
		return 0;
	}
	int stereo = 0;
	for (uint32_t i = 0; i < type_count; i++)
		stereo = stereo || types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!stereo) {
		SDL_snprintf(error, error_size, "the headset has no stereo view configuration");
		return 0;
	}

	XrViewConfigurationView views[2];
	uint32_t                view_count = 0;
	for (int i = 0; i < 2; i++) {
		SDL_zero(views[i]);
		views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	result = g_xr.fn.xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system_id,
													   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
													   &view_count, views);
	if (XR_FAILED(result) || view_count != 2) {
		xr_result_error(error, error_size, "could not read the stereo views", result);
		return 0;
	}
	g_xr.view_width      = (int)SDL_max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
	g_xr.view_height     = (int)SDL_max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);
	g_xr.view_max_width  = (int)SDL_min(views[0].maxImageRectWidth, views[1].maxImageRectWidth);
	g_xr.view_max_height = (int)SDL_min(views[0].maxImageRectHeight, views[1].maxImageRectHeight);
	if (g_xr.view_width <= 0 || g_xr.view_height <= 0) {
		SDL_snprintf(error, error_size, "the runtime gave no size for the eye images");
		return 0;
	}
	return 1;
}

/* Picks the format of the eye images from the runtime's list, which is in
 * its order of preference. */
static int xr_pick_color_format(SDL_GPUDevice* device, char* error, size_t error_size) {
	int                   count   = 0;
	SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(device, g_xr.session, &count);
	if (!formats) {
		SDL_snprintf(error, error_size, "could not list the eye image formats: %s", SDL_GetError());
		return 0;
	}
	g_xr.color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	for (int i = 0; i < count && g_xr.color_format == SDL_GPU_TEXTUREFORMAT_INVALID; i++) {
		if (formats[i] == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
			formats[i] == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
			g_xr.color_format = formats[i];
	}
	SDL_free(formats);
	if (g_xr.color_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		SDL_snprintf(error, error_size, "the runtime offers no 8-bit sRGB format for the eye images");
		return 0;
	}
	return 1;
}

static void xr_log_runtime(void) {
	XrInstanceProperties instance = { .type = XR_TYPE_INSTANCE_PROPERTIES };
	XrSystemProperties   system   = { .type = XR_TYPE_SYSTEM_PROPERTIES };
	if (XR_FAILED(g_xr.fn.xrGetInstanceProperties(g_xr.instance, &instance)))
		SDL_strlcpy(instance.runtimeName, "unknown", sizeof instance.runtimeName);
	if (XR_FAILED(g_xr.fn.xrGetSystemProperties(g_xr.instance, g_xr.system_id, &system)))
		SDL_strlcpy(system.systemName, "unknown", sizeof system.systemName);
	Aeron_LogInfo("aeron.xr", "OpenXR runtime %s %u.%u.%u, headset %s", instance.runtimeName,
				  (unsigned)XR_VERSION_MAJOR(instance.runtimeVersion),
				  (unsigned)XR_VERSION_MINOR(instance.runtimeVersion),
				  (unsigned)XR_VERSION_PATCH(instance.runtimeVersion), system.systemName);
	Aeron_LogInfo("aeron.xr", "eye images %dx%d (at most %dx%d), %s, visibility mask %s", g_xr.view_width,
				  g_xr.view_height, g_xr.view_max_width, g_xr.view_max_height,
				  g_xr.color_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ? "RGBA8 sRGB" : "BGRA8 sRGB",
				  g_xr.extension_count ? "yes" : "no");
}

int AeronXr_DeviceCreated(SDL_GPUDevice* device, char* error, size_t error_size) {
	if (!g_xr.instance) {
		SDL_snprintf(error, error_size, "SDL created the device without an OpenXR instance");
		return 0;
	}
	if (!AeronXr_LoadFunctions(g_xr.instance, &g_xr.fn, error, error_size))
		return 0;

	const XrSessionCreateInfo session_info = { .type = XR_TYPE_SESSION_CREATE_INFO };
	XrResult                  result       = SDL_CreateGPUXRSession(device, &session_info, &g_xr.session);
	if (XR_FAILED(result)) {
		g_xr.session = XR_NULL_HANDLE;
		xr_result_error(error, error_size, "could not create the OpenXR session", result);
		return 0;
	}

	const XrReferenceSpaceCreateInfo space_info = {
		.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL,
		.poseInReferenceSpace = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } },
	};
	result = g_xr.fn.xrCreateReferenceSpace(g_xr.session, &space_info, &g_xr.local_space);
	if (XR_FAILED(result)) {
		g_xr.local_space = XR_NULL_HANDLE;
		xr_result_error(error, error_size, "could not create the reference space", result);
		return 0;
	}

	if (!xr_read_view_configuration(error, error_size) || !xr_pick_color_format(device, error, error_size))
		return 0;
	xr_log_runtime();
	return 1;
}

void AeronXr_DeviceCreateFailed(void) {
	/* SDL destroys the instance it created when the device creation fails. */
	g_xr.instance  = XR_NULL_HANDLE;
	g_xr.system_id = XR_NULL_SYSTEM_ID;
}

void AeronXr_DestroySession(void) {
	if (g_xr.local_space)
		g_xr.fn.xrDestroySpace(g_xr.local_space);
	if (g_xr.session)
		g_xr.fn.xrDestroySession(g_xr.session);
	g_xr.local_space = XR_NULL_HANDLE;
	g_xr.session     = XR_NULL_HANDLE;
}

void AeronXr_DestroyInstance(void) {
	/* SDL_DestroyGPUDevice leaves the instance alone. */
	if (g_xr.instance && g_xr.fn.xrDestroyInstance)
		g_xr.fn.xrDestroyInstance(g_xr.instance);
	if (g_xr.runtime_loaded)
		AeronXr_UnloadRuntime();
	SDL_zero(g_xr);
}

int Aeron_XrEnabled(void) {
	return g_xr.session != XR_NULL_HANDLE;
}

int Aeron_XrViewSize(int* width, int* height) {
	const int enabled = Aeron_XrEnabled();
	if (width)
		*width = enabled ? g_xr.view_width : 0;
	if (height)
		*height = enabled ? g_xr.view_height : 0;
	return enabled;
}

#else /* !AERON_XR */

int AeronXr_PrepareDevice(char* error, size_t error_size) {
	SDL_snprintf(error, error_size, "this build has no VR support");
	return 0;
}

int AeronXr_SetDeviceProperties(SDL_PropertiesID properties) {
	(void)properties;
	return 0;
}

int AeronXr_DeviceCreated(SDL_GPUDevice* device, char* error, size_t error_size) {
	(void)device;
	SDL_snprintf(error, error_size, "this build has no VR support");
	return 0;
}

void AeronXr_DeviceCreateFailed(void) {
}

void AeronXr_DestroySession(void) {
}

void AeronXr_DestroyInstance(void) {
}

int Aeron_XrEnabled(void) {
	return 0;
}

int Aeron_XrViewSize(int* width, int* height) {
	if (width)
		*width = 0;
	if (height)
		*height = 0;
	return 0;
}

#endif /* AERON_XR */

int Aeron_XrSupported(void) {
	return AERON_XR != 0;
}

int Aeron_XrSessionRunning(void) {
	return 0;
}

int Aeron_XrFocused(void) {
	return 0;
}

void Aeron_XrRecenter(void) {
}

int Aeron_XrBeginViews(AeronXrFrame* out) {
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

void Aeron_XrSubmitViews(void) {
}

int Aeron_XrSubmitMirror(const AeronSwapchainRenderLayerDesc* desc) {
	(void)desc;
	return 0;
}
