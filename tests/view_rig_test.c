/*
 * Tests the view rig math (aeron/scene/view_rig.h): converting a field of
 * view to the scene camera's projection values, computing a view's camera
 * from the seat camera, and building the surround and debug stereo rigs.
 *
 * Built with the tools as aeron_view_rig_test. It needs no window and no
 * GPU. It prints each check and exits with a non-zero status when a check
 * fails.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aeron/scene/view_rig.h"

#define TOLERANCE 1e-4f
#define PI 3.14159265358979f

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

static int close_to(float value, float expected) { return fabsf(value - expected) <= TOLERANCE; }

static int close3(const float value[3], float x, float y, float z) {
	return close_to(value[0], x) && close_to(value[1], y) && close_to(value[2], z);
}

static float radians(float degrees) { return degrees * PI / 180.0f; }

/* Writes the quaternion (x, y, z, w) of a rotation of `degrees` around a
 * unit axis. */
static void axis_angle(float x, float y, float z, float degrees, float out[4]) {
	const float half = radians(degrees) * 0.5f;
	out[0]           = x * sinf(half);
	out[1]           = y * sinf(half);
	out[2]           = z * sinf(half);
	out[3]           = cosf(half);
}

static void identity_pose(AeronViewPose* pose) {
	memset(pose, 0, sizeof *pose);
	pose->orientation[3] = 1.0f;
}

static void mat3_apply(const float m[9], const float v[3], float out[3]) {
	for (int row = 0; row < 3; ++row)
		out[row] = m[row * 3 + 0] * v[0] + m[row * 3 + 1] * v[1] + m[row * 3 + 2] * v[2];
}

static void mat3_apply_transposed(const float m[9], const float v[3], float out[3]) {
	for (int row = 0; row < 3; ++row)
		out[row] = m[0 * 3 + row] * v[0] + m[1 * 3 + row] * v[1] + m[2 * 3 + row] * v[2];
}

/* Converts a world point to a camera's eye space. */
static void to_eye(const float rows[9], const float pos[3], const float world[3], float out[3]) {
	const float delta[3] = { world[0] - pos[0], world[1] - pos[1], world[2] - pos[2] };
	mat3_apply(rows, delta, out);
}

/* Projects an eye-space direction to NDC the way the scene does
 * (scene_mat4_perspective_reverse_z_xy). NDC y points up and eye y points
 * down. */
static void project(const AeronViewFov* fov, float eye_x, float eye_y, float eye_z, float* ndc_x,
					float* ndc_y) {
	float h_half, v_half, x_offset, y_offset;
	AeronViewFov_ToCamera(fov, &h_half, &v_half, &x_offset, &y_offset);
	*ndc_x = eye_x / (eye_z * tanf(h_half)) + x_offset;
	*ndc_y = -eye_y / (eye_z * tanf(v_half)) + y_offset;
}

static void test_fov(void) {
	float h_half, v_half, x_offset, y_offset;
	float x, y;

	const AeronViewFov symmetric = { radians(-45.0f), radians(45.0f), radians(30.0f), radians(-30.0f) };
	AeronViewFov_ToCamera(&symmetric, &h_half, &v_half, &x_offset, &y_offset);
	check(close_to(h_half, radians(45.0f)) && close_to(v_half, radians(30.0f)),
		  "a symmetric field of view gives its own half angles");
	check(close_to(x_offset, 0.0f) && close_to(y_offset, 0.0f), "a symmetric field of view has no offset");

	/* A typical left eye of a headset: it sees further to the left than to
	 * the right, and further down than up. */
	const AeronViewFov left_eye = { radians(-50.0f), radians(40.0f), radians(42.0f), radians(-48.0f) };
	project(&left_eye, tanf(left_eye.angle_left), 0.0f, 1.0f, &x, &y);
	check(close_to(x, -1.0f), "the left edge of an off-centre view is at NDC x = -1");
	project(&left_eye, tanf(left_eye.angle_right), 0.0f, 1.0f, &x, &y);
	check(close_to(x, 1.0f), "the right edge of an off-centre view is at NDC x = +1");
	/* Eye y points down, so the upper edge has a negative eye y. */
	project(&left_eye, 0.0f, -tanf(left_eye.angle_up), 1.0f, &x, &y);
	check(close_to(y, 1.0f), "the upper edge of an off-centre view is at NDC y = +1");
	project(&left_eye, 0.0f, -tanf(left_eye.angle_down), 1.0f, &x, &y);
	check(close_to(y, -1.0f), "the lower edge of an off-centre view is at NDC y = -1");
	project(&left_eye, 0.0f, 0.0f, 1.0f, &x, &y);
	check(x > 0.0f && y > 0.0f, "the view axis of a left eye is right of and above the image centre");

	/* A headset's area meshes are on the plane one unit in front of the
	 * eye, with Y up. The corners of the eye image are the corners of clip
	 * space. */
	AeronViewFov_ToCamera(&left_eye, &h_half, &v_half, &x_offset, &y_offset);
	const float plane[8] = { tanf(left_eye.angle_left),
							 tanf(left_eye.angle_down),
							 tanf(left_eye.angle_right),
							 tanf(left_eye.angle_up),
							 0.0f,
							 0.0f,
							 tanf(left_eye.angle_right),
							 tanf(left_eye.angle_down) };
	float       clip[8];
	AeronView_PlaneToClip(plane, 4, h_half, v_half, x_offset, y_offset, clip);
	check(close_to(clip[0], -1.0f) && close_to(clip[1], -1.0f),
		  "the lower left corner of the eye image is at clip (-1, -1)");
	check(close_to(clip[2], 1.0f) && close_to(clip[3], 1.0f),
		  "the upper right corner of the eye image is at clip (+1, +1)");
	check(close_to(clip[6], 1.0f) && close_to(clip[7], -1.0f),
		  "the lower right corner of the eye image is at clip (+1, -1)");
	project(&left_eye, 0.0f, 0.0f, 1.0f, &x, &y);
	check(close_to(clip[4], x) && close_to(clip[5], y),
		  "the view axis is at the same place for a mesh and for the scene");

	const AeronViewFov right_eye = { -left_eye.angle_right, -left_eye.angle_left, left_eye.angle_up,
									 left_eye.angle_down };
	float              mirrored_x, mirrored_y;
	float              h_half_right, v_half_right;
	AeronViewFov_ToCamera(&left_eye, &h_half, &v_half, &x_offset, &y_offset);
	AeronViewFov_ToCamera(&right_eye, &h_half_right, &v_half_right, &mirrored_x, &mirrored_y);
	check(close_to(h_half_right, h_half) && close_to(mirrored_x, -x_offset) && close_to(mirrored_y, y_offset),
		  "the right eye has the opposite x offset of the left eye");
}

static void test_rotation(void) {
	AeronViewPose pose;
	float         m[9];
	float         out[3];
	const float   right[3]   = { 1.0f, 0.0f, 0.0f };
	const float   forward[3] = { 0.0f, 0.0f, -1.0f };

	identity_pose(&pose);
	AeronViewPose_Rotation(&pose, m);
	mat3_apply(m, forward, out);
	check(close3(out, 0.0f, 0.0f, -1.0f), "the identity pose looks along -Z");

	/* A positive rotation around +Y turns the head to the left. */
	axis_angle(0.0f, 1.0f, 0.0f, 90.0f, pose.orientation);
	AeronViewPose_Rotation(&pose, m);
	mat3_apply(m, forward, out);
	check(close3(out, -1.0f, 0.0f, 0.0f), "after a quarter turn around +Y, the view looks along -X");
	mat3_apply(m, right, out);
	check(close3(out, 0.0f, 0.0f, -1.0f), "after a quarter turn around +Y, the right axis is along -Z");
}

static void test_compose(void) {
	/* A seat camera whose eye axes are the same as the world axes: x right,
	 * y down, z forward. */
	const float   identity_rows[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	const float   seat_pos[3]      = { 100.0f, 200.0f, 300.0f };
	const float   units            = 40.96f;
	AeronViewPose pose;
	float         rows[9];
	float         pos[3];
	float         eye[3];

	identity_pose(&pose);
	AeronViewPose_ComposeCamera(&pose, identity_rows, seat_pos, units, rows, pos);
	check(memcmp(rows, identity_rows, sizeof rows) == 0 && close3(pos, 100.0f, 200.0f, 300.0f),
		  "the identity pose gives the seat camera unchanged");

	/* 0.1 m to the right, 0.2 m up, 0.3 m forward. */
	pose.position_m[0] = 0.1f;
	pose.position_m[1] = 0.2f;
	pose.position_m[2] = -0.3f;
	AeronViewPose_ComposeCamera(&pose, identity_rows, seat_pos, units, rows, pos);
	check(close3(pos, 100.0f + 0.1f * units, 200.0f - 0.2f * units, 300.0f + 0.3f * units),
		  "moving the head right, up and forward moves the camera the same way, in scene units");

	/* Three points 10 units from the seat: ahead, to the left and above. */
	const float ahead[3] = { seat_pos[0], seat_pos[1], seat_pos[2] + 10.0f };
	const float left[3]  = { seat_pos[0] - 10.0f, seat_pos[1], seat_pos[2] };
	const float above[3] = { seat_pos[0], seat_pos[1] - 10.0f, seat_pos[2] };

	identity_pose(&pose);
	axis_angle(0.0f, 1.0f, 0.0f, 90.0f, pose.orientation);
	AeronViewPose_ComposeCamera(&pose, identity_rows, seat_pos, units, rows, pos);
	to_eye(rows, pos, left, eye);
	check(close3(eye, 0.0f, 0.0f, 10.0f), "with the head turned left, a point on the left is straight ahead");
	to_eye(rows, pos, ahead, eye);
	check(close3(eye, 10.0f, 0.0f, 0.0f), "with the head turned left, a point ahead is on the right");

	axis_angle(1.0f, 0.0f, 0.0f, 90.0f, pose.orientation);
	AeronViewPose_ComposeCamera(&pose, identity_rows, seat_pos, units, rows, pos);
	to_eye(rows, pos, above, eye);
	check(close3(eye, 0.0f, 0.0f, 10.0f), "with the head turned up, a point above is straight ahead");
	to_eye(rows, pos, ahead, eye);
	check(close3(eye, 0.0f, 10.0f, 0.0f), "with the head turned up, a point ahead is below");

	/* A positive rotation around +Z tilts the head to the left: its right
	 * side goes up. */
	axis_angle(0.0f, 0.0f, 1.0f, 90.0f, pose.orientation);
	AeronViewPose_ComposeCamera(&pose, identity_rows, seat_pos, units, rows, pos);
	to_eye(rows, pos, above, eye);
	check(close3(eye, 10.0f, 0.0f, 0.0f), "with the head tilted left, a point above is on the right");

	/* Check a general pose on a turned seat camera against the same
	 * conversion done one step at a time. */
	const float c = cosf(radians(35.0f)), s = sinf(radians(35.0f));
	/* The seat camera is turned around the world's y axis, then around its
	 * own x axis. */
	const float c2 = cosf(radians(-20.0f)), s2 = sinf(radians(-20.0f));
	const float yaw[9]   = { c, 0, -s, 0, 1, 0, s, 0, c };
	const float pitch[9] = { 1, 0, 0, 0, c2, s2, 0, -s2, c2 };
	float       seat_rows[9];
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) {
			seat_rows[row * 3 + column] = pitch[row * 3 + 0] * yaw[0 * 3 + column] +
										  pitch[row * 3 + 1] * yaw[1 * 3 + column] +
										  pitch[row * 3 + 2] * yaw[2 * 3 + column];
		}
	}
	const float length  = sqrtf(0.2f * 0.2f + 0.5f * 0.5f + 0.1f * 0.1f + 0.8f * 0.8f);
	pose.orientation[0] = 0.2f / length;
	pose.orientation[1] = -0.5f / length;
	pose.orientation[2] = 0.1f / length;
	pose.orientation[3] = 0.8f / length;
	pose.position_m[0]  = -0.03f;
	pose.position_m[1]  = 0.12f;
	pose.position_m[2]  = 0.05f;
	AeronViewPose_ComposeCamera(&pose, seat_rows, seat_pos, units, rows, pos);
	const float world[3] = { 130.0f, 150.0f, 420.0f };
	to_eye(rows, pos, world, eye);
	/* One step at a time: go to the seat's eye space, then to rig space
	 * (negate y and z), then to the view's axes, then back to eye space. */
	float seat_eye[3];
	to_eye(seat_rows, seat_pos, world, seat_eye);
	const float in_rig[3] = { seat_eye[0] - pose.position_m[0] * units,
							  -seat_eye[1] - pose.position_m[1] * units,
							  -seat_eye[2] - pose.position_m[2] * units };
	float       rotation[9];
	float       in_view[3];
	AeronViewPose_Rotation(&pose, rotation);
	mat3_apply_transposed(rotation, in_rig, in_view);
	check(close_to(eye[0], in_view[0]) && close_to(eye[1], -in_view[1]) && close_to(eye[2], -in_view[2]),
		  "a general pose on a turned seat camera matches the conversion done one step at a time");
	float length_check = 0.0f;
	for (int i = 0; i < 3; ++i)
		length_check += rows[i] * rows[i];
	check(close_to(length_check, 1.0f), "the rows of the view camera have unit length");
}

/* Returns nonzero when the edge of the view's frustum at the horizontal
 * tangent `tangent` passes through the point (x, z) of rig space. */
static int edge_through(const AeronRigView* view, float tangent, float x, float z) {
	float       rotation[9];
	float       direction[3];
	const float local[3] = { tangent, 0.0f, -1.0f };
	AeronViewPose_Rotation(&view->pose, rotation);
	mat3_apply(rotation, local, direction);
	/* The edge must be parallel to the point's direction and point the same
	 * way. */
	const float cross = direction[0] * z - direction[2] * x;
	const float dot   = direction[0] * x + direction[2] * z;
	return fabsf(cross) <= TOLERANCE * sqrtf(x * x + z * z) && dot > 0.0f && close_to(direction[1], 0.0f);
}

static void test_surround(void) {
	const AeronRectI  rects[3] = { { 0, 0, 2560, 1440 }, { 2560, 0, 2560, 1440 }, { 5120, 0, 2560, 1440 } };
	AeronViewRig      rig;
	AeronSurroundDesc desc = { .screen_width_m = 0.6f, .screen_height_m = 0.34f, .distance_m = 0.7f };

	/* With no side angle and no bezel, the three views together make one
	 * wide view. */
	check(AeronViewRig_FromSurround(&desc, rects, &rig) && rig.count == 3,
		  "a surround setup with no side angle gives three views");
	check(rig.primary == 1 && rig.width == 2560 && rig.height == 1440 && !rig.tracked && !rig.shared_sprites,
		  "the centre view is the primary one, and sprites are built for each view");
	check(close_to(tanf(rig.views[1].fov.angle_right), 0.3f / 0.7f) &&
			  close_to(tanf(rig.views[1].fov.angle_left), -0.3f / 0.7f),
		  "the centre view reaches the left and right edges of the picture");
	check(close_to(tanf(rig.views[1].fov.angle_up), 0.17f / 0.7f) &&
			  close_to(rig.views[1].fov.angle_down, -rig.views[1].fov.angle_up),
		  "the centre view reaches the upper and lower edges of the picture");
	check(close_to(rig.views[2].fov.angle_left, rig.views[1].fov.angle_right) &&
			  close_to(tanf(rig.views[2].fov.angle_right), 0.9f / 0.7f) &&
			  close_to(rig.views[2].pose.orientation[3], 1.0f),
		  "with no side angle, the right view starts where the centre view ends and isn't turned");
	check(close_to(rig.views[0].fov.angle_right, rig.views[1].fov.angle_left) &&
			  close_to(tanf(rig.views[0].fov.angle_left), -0.9f / 0.7f),
		  "with no side angle, the left view ends where the centre view starts");
	check(rig.views[0].rect.x == 0 && rig.views[1].rect.x == 2560 && rig.views[2].rect.x == 5120,
		  "each view gets its rectangle, left to right");

	/* Side screens at 45 degrees with 1 cm bezels. Worked out by hand: the
	 * right picture goes from (0.317071, -0.692929) to (0.741335, -0.268665)
	 * in the rig's X and Z, and its plane is 0.714178 m from the eye. */
	desc.bezel_m        = 0.01f;
	desc.side_angle_rad = radians(45.0f);
	check(AeronViewRig_FromSurround(&desc, rects, &rig), "a surround setup at 45 degrees is accepted");
	const AeronRigView* right = &rig.views[2];
	const AeronRigView* left  = &rig.views[0];
	check(close_to(tanf(right->fov.angle_left), -0.372137f) &&
			  close_to(tanf(right->fov.angle_right), 0.467990f) &&
			  close_to(tanf(right->fov.angle_up), 0.238036f),
		  "the right view at 45 degrees has the frustum worked out by hand");
	check(edge_through(right, tanf(right->fov.angle_left), 0.317071f, -0.692929f) &&
			  edge_through(right, tanf(right->fov.angle_right), 0.741335f, -0.268665f),
		  "the edges of the right view pass through the edges of its picture");
	check(edge_through(left, tanf(left->fov.angle_right), -0.317071f, -0.692929f) &&
			  edge_through(left, tanf(left->fov.angle_left), -0.741335f, -0.268665f),
		  "the edges of the left view pass through the edges of its picture");
	check(close_to(left->fov.angle_left, -right->fov.angle_right) &&
			  close_to(left->fov.angle_right, -right->fov.angle_left) &&
			  close_to(left->pose.orientation[1], -right->pose.orientation[1]),
		  "the left view is the mirror image of the right view");
	float       forward[3];
	float       rotation[9];
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	AeronViewPose_Rotation(&right->pose, rotation);
	mat3_apply(rotation, ahead, forward);
	check(close3(forward, sinf(radians(45.0f)), 0.0f, -cosf(radians(45.0f))),
		  "the right view looks 45 degrees to the right");
	check(close_to(tanf(rig.views[1].fov.angle_right), 0.3f / 0.7f),
		  "the bezel and the side angle don't change the centre view");

	/* At 60 degrees. The hinge is at x = 0.31, and the right picture starts
	 * 1 cm after it. */
	desc.side_angle_rad = radians(60.0f);
	check(AeronViewRig_FromSurround(&desc, rects, &rig), "a surround setup at 60 degrees is accepted");
	const float along_x = cosf(radians(60.0f)), along_z = sinf(radians(60.0f));
	check(edge_through(&rig.views[2], tanf(rig.views[2].fov.angle_left), 0.31f + 0.01f * along_x,
					   -0.7f + 0.01f * along_z) &&
			  edge_through(&rig.views[2], tanf(rig.views[2].fov.angle_right), 0.31f + 0.61f * along_x,
						   -0.7f + 0.61f * along_z),
		  "at 60 degrees, the edges of the right view pass through the edges of its picture");

	desc.side_angle_rad = radians(170.0f);
	check(!AeronViewRig_FromSurround(&desc, rects, &rig),
		  "a side screen that faces away from the eye is refused");
	desc.side_angle_rad        = radians(45.0f);
	const AeronRectI uneven[3] = { { 0, 0, 1920, 1080 }, { 1920, 0, 2560, 1440 }, { 4480, 0, 1920, 1080 } };
	check(!AeronViewRig_FromSurround(&desc, uneven, &rig), "screens of different sizes are refused");
	desc.distance_m = 0.0f;
	check(!AeronViewRig_FromSurround(&desc, rects, &rig), "a distance of zero is refused");
}

static void test_debug_stereo(void) {
	const AeronViewFov fov = { radians(-45.0f), radians(45.0f), radians(45.0f), radians(-45.0f) };
	AeronViewPose      head;
	AeronViewRig       rig;

	identity_pose(&head);
	head.position_m[1] = 0.1f;
	check(AeronViewRig_DebugStereo(0.064f, &fov, &head, 1000, 900, &rig) && rig.count == 2,
		  "the debug stereo rig has two views");
	check(close3(rig.views[0].pose.position_m, -0.032f, 0.1f, 0.0f) &&
			  close3(rig.views[1].pose.position_m, 0.032f, 0.1f, 0.0f),
		  "each eye is half the IPD to the left or right of the head");
	check(rig.views[0].rect.x == 0 && rig.views[1].rect.x == 1000 && rig.views[1].rect.width == 1000 &&
			  rig.views[1].rect.height == 900 && rig.width == 1000 && rig.height == 900,
		  "the views are side by side");
	check(rig.tracked && rig.shared_sprites && rig.primary == 0 &&
			  close3(rig.center.position_m, 0.0f, 0.1f, 0.0f),
		  "the rig is tracked, shares its sprites and has the head as its centre");

	/* With the head turned left, its right axis points along -Z. */
	axis_angle(0.0f, 1.0f, 0.0f, 90.0f, head.orientation);
	check(AeronViewRig_DebugStereo(0.064f, &fov, &head, 1000, 900, &rig) &&
			  close3(rig.views[1].pose.position_m, 0.0f, 0.1f, -0.032f) &&
			  close3(rig.views[0].pose.position_m, 0.0f, 0.1f, 0.032f),
		  "the eyes move with the head when it turns");
	check(!AeronViewRig_DebugStereo(0.064f, &fov, &head, 0, 900, &rig), "a view size of zero is refused");
}

int main(void) {
	test_fov();
	test_rotation();
	test_compose();
	test_surround();
	test_debug_stereo();
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
