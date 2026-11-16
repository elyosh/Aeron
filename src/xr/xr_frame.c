/* The OpenXR frame: xrWaitFrame and xrBeginFrame at the start of an Aeron
 * frame, xrEndFrame at its end. While the session runs, xrWaitFrame paces
 * the application at the headset's rate. No layer is submitted yet. */

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
	if (g_xr.recenter_pending)
		xr_apply_recenter();
	return 1;
}

int AeronXr_EndFrame(void) {
	if (!g_xr.frame_open || g_xr.failed)
		return 1;
	g_xr.frame_open = 0;

	const XrFrameEndInfo end_info = {
		.type                 = XR_TYPE_FRAME_END_INFO,
		.displayTime          = g_xr.frame_display_time,
		.environmentBlendMode = g_xr.blend_mode,
		.layerCount           = 0,
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
