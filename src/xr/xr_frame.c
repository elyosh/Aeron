/* The OpenXR frame: xrWaitFrame and xrBeginFrame at the start of an Aeron
 * frame, xrEndFrame at its end. While the session runs, xrWaitFrame paces
 * the application at the headset's rate. xrEndFrame gives the runtime the
 * frame's layers: the eye images and the virtual screen. */

#include "../internal.h"
#include "xr_internal.h"

/* Moves the rig's origin to the head: its position and its yaw. It needs
 * the head's pose, so it runs inside a frame. It stays pending while the
 * runtime doesn't track the head. */
static void xr_apply_recenter(void) {
	XrSpaceLocation head = { .type = XR_TYPE_SPACE_LOCATION };
	const XrResult  result =
		g_xr.fn.xrLocateSpace(g_xr.view_space, g_xr.local_space, g_xr.frame_display_time, &head);
	const XrSpaceLocationFlags needed =
		XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(result) || (head.locationFlags & needed) != needed)
		return;

	XrPosef origin;
	AeronXr_LevelOrigin(&head.pose, &origin);
	const XrReferenceSpaceCreateInfo info = {
		.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL,
		.poseInReferenceSpace = origin,
	};
	XrSpace rig_space = XR_NULL_HANDLE;
	if (XR_FAILED(g_xr.fn.xrCreateReferenceSpace(g_xr.machine.session, &info, &rig_space)))
		return;
	g_xr.fn.xrDestroySpace(g_xr.rig_space);
	g_xr.rig_space        = rig_space;
	g_xr.recenter_pending = 0;
	g_xr.machine.generation++;
	Aeron_LogInfo("aeron.xr", "recentred the view on the headset");
}

int AeronXr_BeginFrame(void) {
	char error[256];

	if (!g_xr.machine.running || g_xr.failed)
		return 1;
	/* The application skipped Aeron_Present for the last frame. */
	if (!AeronXr_EndFrame())
		return 0;

	const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState          state     = { .type = XR_TYPE_FRAME_STATE };
	XrResult              result    = g_xr.fn.xrWaitFrame(g_xr.machine.session, &wait_info, &state);
	if (XR_FAILED(result)) {
		AeronXr_ResultError(error, sizeof error, "could not wait for the headset's next frame", result);
		AeronXr_Fail(error);
		return 0;
	}
	const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
	result                            = g_xr.fn.xrBeginFrame(g_xr.machine.session, &begin_info);
	if (XR_FAILED(result)) {
		AeronXr_ResultError(error, sizeof error, "could not begin the headset's frame", result);
		AeronXr_Fail(error);
		return 0;
	}
	g_xr.frame_open          = 1;
	g_xr.frame_display_time  = state.predictedDisplayTime;
	g_xr.frame_should_render = state.shouldRender == XR_TRUE;
	g_xr.frame_period        = state.predictedDisplayPeriod > 0 ? state.predictedDisplayPeriod : 0;
	if (g_xr.recenter_pending)
		xr_apply_recenter();
	return 1;
}

/* The layer that shows the two eye images. */
static void xr_fill_projection(XrCompositionLayerProjection* layer, XrCompositionLayerProjectionView views[2]) {
	for (int eye = 0; eye < 2; eye++) {
		views[eye] = (XrCompositionLayerProjectionView) {
			.type     = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
			.pose     = g_xr.located_views[eye].pose,
			.fov      = g_xr.located_views[eye].fov,
			.subImage = {
				.swapchain = g_xr.eyes[eye].tracker.swapchain,
				.imageRect = { { 0, 0 }, { g_xr.view_width, g_xr.view_height } },
			},
		};
	}
	*layer = (XrCompositionLayerProjection) {
		.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		.space     = g_xr.rig_space,
		.viewCount = 2,
		.views     = views,
	};
}

/* The layer that shows the virtual screen: a flat rectangle straight ahead
 * of the rig's origin. Its colours are premultiplied by alpha. */
static void xr_fill_screen(XrCompositionLayerQuad* layer) {
	const AeronStartupOptions* options = &g_aeron.xr_options;
	const float distance = options->xr_screen_distance_m > 0.0f ? options->xr_screen_distance_m
																: AERON_XR_SCREEN_DEFAULT_DISTANCE_M;
	const float width =
		options->xr_screen_width_m > 0.0f ? options->xr_screen_width_m : AERON_XR_SCREEN_DEFAULT_WIDTH_M;
	*layer = (XrCompositionLayerQuad) {
		.type          = XR_TYPE_COMPOSITION_LAYER_QUAD,
		.layerFlags    = g_xr.screen_transparent ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0,
		.space         = g_xr.rig_space,
		.eyeVisibility = XR_EYE_VISIBILITY_BOTH,
		.subImage = {
			.swapchain = g_xr.screen.tracker.swapchain,
			.imageRect = { { 0, 0 }, { g_xr.screen.width, g_xr.screen.height } },
		},
		.pose = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f }, .position = { 0.0f, 0.0f, -distance } },
		.size = { width, width * (float)g_xr.screen.height / (float)g_xr.screen.width },
	};
}

int AeronXr_EndFrame(void) {
	if (!g_xr.frame_open || g_xr.failed)
		return 1;
	g_xr.frame_open = 0;

	/* Every command buffer that draws into the images was submitted. */
	AeronXr_ReleaseImages();
	const int show_views  = g_xr.views_submitted;
	const int show_screen = g_xr.screen_visible && g_xr.screen.tracker.released_once;
	g_xr.views_begun      = 0;
	g_xr.views_submitted  = 0;
	g_xr.screen_visible   = 0;
	if (g_xr.failed)
		return 0;

	/* The eye images are behind the virtual screen. */
	XrCompositionLayerProjection       projection;
	XrCompositionLayerProjectionView   projection_views[2];
	XrCompositionLayerQuad             screen;
	const XrCompositionLayerBaseHeader* layers[2];
	uint32_t                            layer_count = 0;
	if (g_xr.frame_should_render && show_views) {
		xr_fill_projection(&projection, projection_views);
		layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&projection;
	}
	if (g_xr.frame_should_render && show_screen) {
		xr_fill_screen(&screen);
		layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&screen;
	}

	const XrFrameEndInfo end_info = {
		.type                 = XR_TYPE_FRAME_END_INFO,
		.displayTime          = g_xr.frame_display_time,
		.environmentBlendMode = g_xr.blend_mode,
		.layerCount           = layer_count,
		.layers               = layer_count ? layers : NULL,
	};
	const XrResult result = g_xr.fn.xrEndFrame(g_xr.machine.session, &end_info);
	if (XR_FAILED(result)) {
		char error[256];
		AeronXr_ResultError(error, sizeof error, "could not end the headset's frame", result);
		AeronXr_Fail(error);
		return 0;
	}
	return 1;
}
