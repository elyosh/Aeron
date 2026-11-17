/*
 * Tests the OpenXR session state machine and the recentre math
 * (src/xr/xr_state.c), with fake OpenXR functions in place of a runtime.
 *
 * Built with the tools as aeron_xr_session_test when AERON_XR is on. It
 * needs no headset, no window and no GPU. It prints each check and exits
 * with a non-zero status when a check fails.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/xr/xr_internal.h"

#define TOLERANCE 1e-4f
#define PI 3.14159265358979f

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

/* The fake runtime: it counts the calls and returns what the test asks. */
static int      g_begin_calls, g_end_calls;
static XrResult g_begin_result, g_end_result;

static XrResult XRAPI_CALL fake_begin_session(XrSession session, const XrSessionBeginInfo* info) {
	(void)session;
	g_begin_calls += info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	return g_begin_result;
}

static XrResult XRAPI_CALL fake_end_session(XrSession session) {
	(void)session;
	g_end_calls++;
	return g_end_result;
}

static const XrSession TEST_SESSION = (XrSession)0x1234;

static void reset(AeronXrSessionMachine* machine, AeronXrFunctions* fn) {
	memset(machine, 0, sizeof *machine);
	memset(fn, 0, sizeof *fn);
	machine->session   = TEST_SESSION;
	fn->xrBeginSession = fake_begin_session;
	fn->xrEndSession   = fake_end_session;
	g_begin_calls = g_end_calls = 0;
	g_begin_result = g_end_result = XR_SUCCESS;
}

static int send_state(AeronXrSessionMachine* machine, const AeronXrFunctions* fn, XrSession session,
					  XrSessionState state) {
	XrEventDataBuffer               buffer  = { .type = XR_TYPE_EVENT_DATA_BUFFER };
	XrEventDataSessionStateChanged* changed = (XrEventDataSessionStateChanged*)&buffer;
	char                            error[128];
	changed->type    = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
	changed->session = session;
	changed->state   = state;
	return AeronXrSession_HandleEvent(machine, fn, &buffer, error, sizeof error);
}

static int send_event(AeronXrSessionMachine* machine, const AeronXrFunctions* fn, XrStructureType type) {
	XrEventDataBuffer buffer = { .type = type };
	char              error[128];
	return AeronXrSession_HandleEvent(machine, fn, &buffer, error, sizeof error);
}

static void test_session_failures(void) {
	AeronXrSessionMachine machine;
	AeronXrFunctions      fn;

	reset(&machine, &fn);
	check(send_state(&machine, &fn, (XrSession)0x9999, XR_SESSION_STATE_READY) && !machine.running &&
			  g_begin_calls == 0,
		  "an event for another session is ignored");

	reset(&machine, &fn);
	g_begin_result = XR_ERROR_RUNTIME_FAILURE;
	check(!send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_READY) && !machine.running,
		  "a failed xrBeginSession is an error, and the session does not run");

	reset(&machine, &fn);
	send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_READY);
	g_end_result = XR_ERROR_RUNTIME_FAILURE;
	check(!send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_STOPPING) && !machine.running,
		  "a failed xrEndSession is an error, and the session stops running");

	reset(&machine, &fn);
	send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_READY);
	check(!send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_LOSS_PENDING) && !machine.running &&
			  g_end_calls == 0,
		  "loss pending: an error, without xrEndSession");

	reset(&machine, &fn);
	send_state(&machine, &fn, TEST_SESSION, XR_SESSION_STATE_READY);
	check(!send_event(&machine, &fn, XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) && !machine.running,
		  "instance loss: an error");

	reset(&machine, &fn);
	check(send_event(&machine, &fn, XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) &&
			  machine.generation == 1,
		  "a reference space change gives a new generation");
	check(send_event(&machine, &fn, XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) &&
			  machine.generation == 1 && !machine.running,
		  "an event that aeron doesn't use changes nothing");
}

/* Hamilton product a * b: the rotation b, then the rotation a. */
static XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
	const XrQuaternionf out = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
	return out;
}

static XrQuaternionf axis_turn(float x, float y, float z, float angle) {
	const float         s   = sinf(0.5f * angle);
	const XrQuaternionf out = { x * s, y * s, z * s, cosf(0.5f * angle) };
	return out;
}

/* Two unit quaternions give the same rotation when they are equal or
 * opposite. */
static int same_rotation(XrQuaternionf a, XrQuaternionf b) {
	const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	return fabsf(fabsf(dot) - 1.0f) <= TOLERANCE;
}

static void test_recenter(void) {
	const float yaw = 50.0f * PI / 180.0f, pitch = 20.0f * PI / 180.0f, roll = -15.0f * PI / 180.0f;
	/* A head turned by yaw, then pitched and rolled in its own axes. */
	const XrQuaternionf tilt = multiply(axis_turn(1, 0, 0, pitch), axis_turn(0, 0, 1, roll));
	XrPosef             head = { .position = { 0.3f, 1.6f, -0.2f } };
	XrPosef             origin;

	head.orientation = multiply(axis_turn(0, 1, 0, yaw), tilt);
	AeronXr_LevelOrigin(&head, &origin);
	check(origin.position.x == head.position.x && origin.position.y == head.position.y &&
			  origin.position.z == head.position.z,
		  "recentre: the origin is at the head");
	check(same_rotation(origin.orientation, axis_turn(0, 1, 0, yaw)),
		  "recentre: the origin has the head's yaw and is level");
	/* The head relative to the origin: the inverse of the origin's turn,
	 * then the head's. */
	const XrQuaternionf inverse = { -origin.orientation.x, -origin.orientation.y, -origin.orientation.z,
									origin.orientation.w };
	check(same_rotation(multiply(inverse, head.orientation), tilt),
		  "recentre: the head keeps its pitch and roll");

	head.orientation = axis_turn(0, 0, 0, 0.0f);
	AeronXr_LevelOrigin(&head, &origin);
	check(same_rotation(origin.orientation, axis_turn(0, 1, 0, 0.0f)),
		  "recentre: a level head gives no turn");

	/* Looking straight down: the forward vector has no direction on the
	 * ground, so the yaw comes from the right vector. */
	head.orientation = multiply(axis_turn(0, 1, 0, yaw), axis_turn(1, 0, 0, -0.5f * PI));
	AeronXr_LevelOrigin(&head, &origin);
	check(same_rotation(origin.orientation, axis_turn(0, 1, 0, yaw)),
		  "recentre: a head that looks straight down keeps its yaw");
}

/* The fake swapchain: it counts the calls, checks their order as a runtime
 * does, and returns what the test asks. */
static int      g_acquire_calls, g_wait_calls, g_release_calls, g_order_errors;
static int      g_runtime_acquired, g_runtime_waited;
static XrResult g_acquire_result, g_wait_result, g_release_result;

static XrResult XRAPI_CALL fake_acquire_image(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info,
											  uint32_t* index) {
	(void)swapchain;
	(void)info;
	g_acquire_calls++;
	if (XR_FAILED(g_acquire_result))
		return g_acquire_result;
	g_order_errors += g_runtime_acquired;
	g_runtime_acquired = 1;
	*index             = (uint32_t)(g_acquire_calls % 3);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_wait_image(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info) {
	(void)swapchain;
	g_wait_calls++;
	g_order_errors += !g_runtime_acquired || g_runtime_waited || info->timeout != XR_INFINITE_DURATION;
	if (g_wait_result != XR_SUCCESS)
		return g_wait_result;
	g_runtime_waited = 1;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_release_image(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
	(void)swapchain;
	(void)info;
	g_release_calls++;
	g_order_errors += !g_runtime_acquired || !g_runtime_waited;
	g_runtime_acquired = g_runtime_waited = 0;
	return g_release_result;
}

static void test_swapchain_images(void) {
	AeronXrImageTracker tracker;
	AeronXrFunctions    fn;
	char                error[128];
	memset(&tracker, 0, sizeof tracker);
	memset(&fn, 0, sizeof fn);
	tracker.swapchain          = (XrSwapchain)0x5678;
	fn.xrAcquireSwapchainImage = fake_acquire_image;
	fn.xrWaitSwapchainImage    = fake_wait_image;
	fn.xrReleaseSwapchainImage = fake_release_image;
	g_acquire_result = g_wait_result = g_release_result = XR_SUCCESS;

	check(AeronXrImage_Release(&tracker, &fn, error, sizeof error) && g_release_calls == 0,
		  "images: a release with no image held calls nothing");
	check(AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) && tracker.held && tracker.index == 1 &&
			  g_acquire_calls == 1 && g_wait_calls == 1 && !tracker.released_once,
		  "images: acquire also waits, and gives the runtime's index");
	check(!AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) && g_acquire_calls == 1 && tracker.held,
		  "images: a second acquire fails and calls nothing");
	check(AeronXrImage_Release(&tracker, &fn, error, sizeof error) && !tracker.held && g_release_calls == 1 &&
			  tracker.released_once,
		  "images: release gives the image back");
	check(AeronXrImage_Release(&tracker, &fn, error, sizeof error) && g_release_calls == 1,
		  "images: an image is released once");
	check(AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) && tracker.index == 2 &&
			  AeronXrImage_Release(&tracker, &fn, error, sizeof error) && g_release_calls == 2,
		  "images: the next frame gets the next image");

	g_acquire_result = XR_ERROR_RUNTIME_FAILURE;
	check(!AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) && !tracker.held && g_wait_calls == 2,
		  "images: a failed acquire holds nothing and doesn't wait");
	check(AeronXrImage_Release(&tracker, &fn, error, sizeof error) && g_release_calls == 2,
		  "images: nothing is released after a failed acquire");
	g_acquire_result = XR_SUCCESS;

	g_wait_result = XR_TIMEOUT_EXPIRED;
	check(!AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) && !tracker.held,
		  "images: a wait that doesn't succeed fails the acquire");
	check(AeronXrImage_Release(&tracker, &fn, error, sizeof error) && g_release_calls == 2,
		  "images: an image that was not waited for is not released");
	g_wait_result      = XR_SUCCESS;
	g_runtime_acquired = 0;

	g_release_result = XR_ERROR_RUNTIME_FAILURE;
	check(AeronXrImage_Acquire(&tracker, &fn, error, sizeof error) &&
			  !AeronXrImage_Release(&tracker, &fn, error, sizeof error) && !tracker.held,
		  "images: a failed release is reported, and the image is not held any more");
	check(g_order_errors == 0, "images: the runtime saw every call in a valid order");
}

int main(void) {
	test_swapchain_images();
	test_session_failures();
	test_recenter();
	printf(g_failures ? "%d checks FAILED\n" : "all checks passed\n", g_failures);
	return g_failures ? 1 : 0;
}
