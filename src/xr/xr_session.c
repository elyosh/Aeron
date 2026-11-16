/* The OpenXR instance, session and reference spaces of the GPU device, and
 * the events that drive the session.
 *
 * SDL creates the instance together with the GPU device, because the runtime
 * takes part in creating the device. Aeron then creates the session on that
 * device. The eye images are not here yet, so the view functions report
 * that there is nothing to render. */

#include "../internal.h"
#include "xr_internal.h"

#include <string.h>

#if AERON_XR

AeronXrState g_xr;

void AeronXr_ResultError(char* error, size_t error_size, const char* what, XrResult result) {
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
		AeronXr_ResultError(error, error_size, "could not list the view configurations", result);
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
		AeronXr_ResultError(error, error_size, "could not read the stereo views", result);
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

	/* The runtime lists the blend modes in its order of preference. */
	uint32_t blend_count = 0;
	result = g_xr.fn.xrEnumerateEnvironmentBlendModes(
		g_xr.instance, g_xr.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &blend_count,
		&g_xr.blend_mode);
	if (result != XR_ERROR_SIZE_INSUFFICIENT && (XR_FAILED(result) || blend_count == 0))
		g_xr.blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	return 1;
}

/* Creates a reference space of this type, at `pose` in it. */
static int xr_create_space(XrReferenceSpaceType type, XrPosef pose, XrSpace* out, char* error,
						   size_t error_size) {
	const XrReferenceSpaceCreateInfo info = {
		.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType   = type,
		.poseInReferenceSpace = pose,
	};
	const XrResult result = g_xr.fn.xrCreateReferenceSpace(g_xr.machine.session, &info, out);
	if (XR_FAILED(result)) {
		*out = XR_NULL_HANDLE;
		AeronXr_ResultError(error, error_size, "could not create a reference space", result);
		return 0;
	}
	return 1;
}

/* Picks the format of the eye images from the runtime's list, which is in
 * its order of preference. */
static int xr_pick_color_format(SDL_GPUDevice* device, char* error, size_t error_size) {
	int                   count   = 0;
	SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(device, g_xr.machine.session, &count);
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
	XrResult                  result       = SDL_CreateGPUXRSession(device, &session_info, &g_xr.machine.session);
	if (XR_FAILED(result)) {
		g_xr.machine.session = XR_NULL_HANDLE;
		AeronXr_ResultError(error, error_size, "could not create the OpenXR session", result);
		return 0;
	}

	/* The rig's origin is the runtime's origin until the first frame, which
	 * recentres. */
	const XrPosef identity = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } };
	if (!xr_create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity, &g_xr.local_space, error, error_size) ||
		!xr_create_space(XR_REFERENCE_SPACE_TYPE_VIEW, identity, &g_xr.view_space, error, error_size) ||
		!xr_create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity, &g_xr.rig_space, error, error_size))
		return 0;
	g_xr.recenter_pending = 1;

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
	if (g_xr.machine.session && !g_xr.failed) {
		AeronXr_EndFrame();
		const XrSpace spaces[] = { g_xr.rig_space, g_xr.view_space, g_xr.local_space };
		for (size_t i = 0; i < SDL_arraysize(spaces); i++) {
			if (spaces[i])
				g_xr.fn.xrDestroySpace(spaces[i]);
		}
		g_xr.fn.xrDestroySession(g_xr.machine.session);
	}
	g_xr.rig_space   = XR_NULL_HANDLE;
	g_xr.view_space  = XR_NULL_HANDLE;
	g_xr.local_space = XR_NULL_HANDLE;
	SDL_zero(g_xr.machine);
}

void AeronXr_Fail(const char* message) {
	if (g_xr.failed)
		return;
	g_xr.failed          = 1;
	g_xr.frame_open      = 0;
	g_xr.machine.running = 0;
	Aeron_LogError("aeron.xr", "%s", message);
	Aeron_OnXrSessionRunningChanged(0);
	Aeron_RequestFatalError("VR Error", message);
}

void AeronXr_PollEvents(void) {
	if (!g_xr.machine.session || g_xr.failed)
		return;
	const int was_running = g_xr.machine.running;
	const int was_focused = g_xr.machine.focused;
	for (;;) {
		XrEventDataBuffer event  = { .type = XR_TYPE_EVENT_DATA_BUFFER };
		const XrResult    result = g_xr.fn.xrPollEvent(g_xr.instance, &event);
		char              error[256];
		if (result == XR_EVENT_UNAVAILABLE)
			break;
		if (XR_FAILED(result)) {
			AeronXr_ResultError(error, sizeof error, "the OpenXR runtime stopped answering", result);
			AeronXr_Fail(error);
			return;
		}
		if (!AeronXrSession_HandleEvent(&g_xr.machine, &g_xr.fn, &event, error, sizeof error)) {
			AeronXr_Fail(error);
			return;
		}
	}
	if (g_xr.machine.running != was_running) {
		Aeron_LogInfo("aeron.xr", "the VR session %s", g_xr.machine.running ? "started" : "stopped");
		/* A frame that was open when the session stopped is gone with it. */
		if (!g_xr.machine.running)
			g_xr.frame_open = 0;
		else
			g_xr.recenter_pending = 1;
		Aeron_OnXrSessionRunningChanged(g_xr.machine.running);
	}
	if (g_xr.machine.focused != was_focused)
		Aeron_LogInfo("aeron.xr", "the VR session %s the input focus",
					  g_xr.machine.focused ? "has" : "lost");
	if (g_xr.machine.exit_requested) {
		g_xr.machine.exit_requested = 0;
		Aeron_LogInfo("aeron.xr", "the OpenXR runtime asked the application to quit");
		Aeron_RequestQuit();
	}
}

int Aeron_XrSessionRunning(void) {
	return g_xr.machine.running;
}

int Aeron_XrFocused(void) {
	return g_xr.machine.focused;
}

void Aeron_XrRecenter(void) {
	if (g_xr.machine.session)
		g_xr.recenter_pending = 1;
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
	return g_xr.machine.session != XR_NULL_HANDLE;
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

void AeronXr_PollEvents(void) {
}

int AeronXr_BeginFrame(void) {
	return 1;
}

int AeronXr_EndFrame(void) {
	return 1;
}

int Aeron_XrSessionRunning(void) {
	return 0;
}

int Aeron_XrFocused(void) {
	return 0;
}

void Aeron_XrRecenter(void) {
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
