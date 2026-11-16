/* The session state machine and the recentre math. This file uses only the
 * OpenXR types and the function table it is given, so
 * tests/xr_session_test.c can run it with fake functions. */

#include "xr_internal.h"

#include <math.h>
#include <stdio.h>

static int state_fail(char* error, size_t error_size, const char* what, XrResult result) {
	snprintf(error, error_size, "%s (XrResult %d)", what, (int)result);
	return 0;
}

static int session_state_changed(AeronXrSessionMachine* machine, const AeronXrFunctions* fn,
								 XrSessionState state, char* error, size_t error_size) {
	machine->state   = state;
	machine->focused = state == XR_SESSION_STATE_FOCUSED;
	switch (state) {
		case XR_SESSION_STATE_READY: {
			const XrSessionBeginInfo info = {
				.type                         = XR_TYPE_SESSION_BEGIN_INFO,
				.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			};
			const XrResult result = fn->xrBeginSession(machine->session, &info);
			if (XR_FAILED(result))
				return state_fail(error, error_size, "the OpenXR session could not begin", result);
			machine->running = 1;
			return 1;
		}
		case XR_SESSION_STATE_STOPPING: {
			/* The session stops running even when the call fails: no frame
			 * may be submitted after this state. */
			const XrResult result = fn->xrEndSession(machine->session);
			machine->running      = 0;
			if (XR_FAILED(result))
				return state_fail(error, error_size, "the OpenXR session could not end", result);
			return 1;
		}
		case XR_SESSION_STATE_EXITING:
			machine->exit_requested = 1;
			return 1;
		case XR_SESSION_STATE_LOSS_PENDING:
			machine->running = 0;
			snprintf(error, error_size, "the connection to the headset was lost");
			return 0;
		default:
			return 1;
	}
}

int AeronXrSession_HandleEvent(AeronXrSessionMachine* machine, const AeronXrFunctions* fn,
							   const XrEventDataBuffer* event, char* error, size_t error_size) {
	switch (event->type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)event;
			if (changed->session != machine->session)
				return 1;
			return session_state_changed(machine, fn, changed->state, error, error_size);
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			machine->running = 0;
			snprintf(error, error_size, "the OpenXR runtime is shutting down");
			return 0;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			machine->generation++;
			return 1;
		default:
			return 1;
	}
}

int AeronXrImage_Acquire(AeronXrImageTracker* tracker, const AeronXrFunctions* fn, char* error,
						 size_t error_size) {
	if (tracker->held) {
		snprintf(error, error_size, "a swapchain image was acquired twice in one frame");
		return 0;
	}
	const XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t                          index        = 0;
	XrResult result = fn->xrAcquireSwapchainImage(tracker->swapchain, &acquire_info, &index);
	if (XR_FAILED(result))
		return state_fail(error, error_size, "could not acquire a swapchain image", result);
	const XrSwapchainImageWaitInfo wait_info = {
		.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
		.timeout = XR_INFINITE_DURATION,
	};
	result = fn->xrWaitSwapchainImage(tracker->swapchain, &wait_info);
	if (result != XR_SUCCESS)
		return state_fail(error, error_size, "could not wait for a swapchain image", result);
	tracker->held  = 1;
	tracker->index = index;
	return 1;
}

int AeronXrImage_Release(AeronXrImageTracker* tracker, const AeronXrFunctions* fn, char* error,
						 size_t error_size) {
	if (!tracker->held)
		return 1;
	tracker->held = 0;
	const XrSwapchainImageReleaseInfo info   = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	const XrResult                    result = fn->xrReleaseSwapchainImage(tracker->swapchain, &info);
	if (XR_FAILED(result))
		return state_fail(error, error_size, "could not release a swapchain image", result);
	tracker->released_once = 1;
	return 1;
}

/* Rotates `v` by the unit quaternion `q`. */
static XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
	/* v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v) */
	const float      cx  = q.y * v.z - q.z * v.y + q.w * v.x;
	const float      cy  = q.z * v.x - q.x * v.z + q.w * v.y;
	const float      cz  = q.x * v.y - q.y * v.x + q.w * v.z;
	const XrVector3f out = {
		v.x + 2.0f * (q.y * cz - q.z * cy),
		v.y + 2.0f * (q.z * cx - q.x * cz),
		v.z + 2.0f * (q.x * cy - q.y * cx),
	};
	return out;
}

void AeronXr_LevelOrigin(const XrPosef* head, XrPosef* origin) {
	/* A turn by `yaw` around +Y sends forward (0, 0, -1) to
	 * (-sin, 0, -cos) and right (1, 0, 0) to (cos, 0, -sin). The forward
	 * vector gives the yaw, except when the head looks almost straight up
	 * or down: then the right vector does. */
	const XrVector3f forward = rotate(head->orientation, (XrVector3f) { 0.0f, 0.0f, -1.0f });
	float            yaw;
	if (forward.x * forward.x + forward.z * forward.z > 0.01f) {
		yaw = atan2f(-forward.x, -forward.z);
	} else {
		const XrVector3f right = rotate(head->orientation, (XrVector3f) { 1.0f, 0.0f, 0.0f });
		yaw                    = atan2f(-right.z, right.x);
	}
	origin->position    = head->position;
	origin->orientation = (XrQuaternionf) { 0.0f, sinf(0.5f * yaw), 0.0f, cosf(0.5f * yaw) };
}
