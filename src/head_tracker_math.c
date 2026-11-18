/* Converts what a head tracker sends to a pose in the space of a view rig:
 * X right, Y up, -Z forward, in metres. This file doesn't use SDL, so
 * tests/head_tracker_test.c can build it on its own. */

#include "head_tracker_internal.h"

#define HEAD_PI 3.14159265358979323846f
#define HEAD_RADIANS_PER_DEGREE (HEAD_PI / 180.0f)

/* The largest pose that is accepted unchanged. Larger values are clamped.
 * This guards against bad data: no head turns or moves this far. */
#define HEAD_MAX_YAW_DEG 180.0f
#define HEAD_MAX_PITCH_DEG 90.0f
#define HEAD_MAX_ROLL_DEG 90.0f
#define HEAD_MAX_MOVE_M 0.5f

/* A TrackIR value is from -16383 to 16383: 180 degrees or 50 cm. */
#define TRACKIR_MAX_VALUE 16383.0f
#define TRACKIR_MAX_ANGLE_DEG 180.0f
#define TRACKIR_MAX_MOVE_M 0.5f

static float head_clamp(float value, float limit) {
	/* A NaN compares false and becomes 0. */
	if (!(value >= -limit))
		return value < -limit ? -limit : 0.0f;
	return value > limit ? limit : value;
}

/* out = a * b, for quaternions stored as (x, y, z, w). */
static void head_quat_multiply(const float a[4], const float b[4], float out[4]) {
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* The angles are in degrees: to the right, up, and clockwise as seen from
 * behind. The position is to the right, up and back. */
typedef struct HeadAngles {
	float yaw_deg;
	float pitch_deg;
	float roll_deg;
	float right_m;
	float up_m;
	float back_m;
} HeadAngles;

static void head_clamp_angles(HeadAngles* head) {
	head->yaw_deg   = head_clamp(head->yaw_deg, HEAD_MAX_YAW_DEG);
	head->pitch_deg = head_clamp(head->pitch_deg, HEAD_MAX_PITCH_DEG);
	head->roll_deg  = head_clamp(head->roll_deg, HEAD_MAX_ROLL_DEG);
	head->right_m   = head_clamp(head->right_m, HEAD_MAX_MOVE_M);
	head->up_m      = head_clamp(head->up_m, HEAD_MAX_MOVE_M);
	head->back_m    = head_clamp(head->back_m, HEAD_MAX_MOVE_M);
}

static void head_set_position(const HeadAngles* head, AeronViewPose* out) {
	/* The rig's -Z is forward, so +Z is back. */
	out->position_m[0] = head->right_m;
	out->position_m[1] = head->up_m;
	out->position_m[2] = head->back_m;
}

void AeronHeadTracker_FromTrackIR(const float values[6], AeronViewPose* out) {
	const float angle = TRACKIR_MAX_ANGLE_DEG / TRACKIR_MAX_VALUE;
	const float move  = TRACKIR_MAX_MOVE_M / TRACKIR_MAX_VALUE;
	/* TrackIR's axes, from the player's side: +X is left, +Y is up and +Z
	 * is back. Its yaw grows when the head turns left, its pitch when it
	 * looks down, and its roll when it tilts left. */
	HeadAngles head = {
		.roll_deg  = -values[0] * angle,
		.pitch_deg = -values[1] * angle,
		.yaw_deg   = -values[2] * angle,
		.right_m   = -values[3] * move,
		.up_m      = values[4] * move,
		.back_m    = values[5] * move,
	};
	head_clamp_angles(&head);
	/* TrackIR's angles are applied roll first, then yaw, then pitch. Each
	 * turn is around the head's own axes, as the earlier turns left them. */
	float roll[4], yaw[4], pitch[4], roll_yaw[4];
	AeronViewPose_FromAngles(0.0f, 0.0f, head.roll_deg * HEAD_RADIANS_PER_DEGREE, roll);
	AeronViewPose_FromAngles(head.yaw_deg * HEAD_RADIANS_PER_DEGREE, 0.0f, 0.0f, yaw);
	AeronViewPose_FromAngles(0.0f, head.pitch_deg * HEAD_RADIANS_PER_DEGREE, 0.0f, pitch);
	head_quat_multiply(roll, yaw, roll_yaw);
	head_quat_multiply(roll_yaw, pitch, out->orientation);
	head_set_position(&head, out);
}

void AeronHeadTracker_FromOpenTrack(const double values[6], AeronViewPose* out) {
	/* OpenTrack's X grows when the head moves left, its Y up and its Z
	 * back, in centimetres. Its yaw grows when the head turns right, its
	 * pitch when it looks up, and its roll when it tilts left. */
	HeadAngles head = {
		.right_m   = (float)(-values[0] * 0.01),
		.up_m      = (float)(values[1] * 0.01),
		.back_m    = (float)(values[2] * 0.01),
		.yaw_deg   = (float)values[3],
		.pitch_deg = (float)values[4],
		.roll_deg  = (float)-values[5],
	};
	head_clamp_angles(&head);
	/* OpenTrack's angles are applied yaw first, then pitch, then roll. */
	AeronViewPose_FromAngles(head.yaw_deg * HEAD_RADIANS_PER_DEGREE, head.pitch_deg * HEAD_RADIANS_PER_DEGREE,
							 head.roll_deg * HEAD_RADIANS_PER_DEGREE, out->orientation);
	head_set_position(&head, out);
}

void AeronHeadTracker_Relative(const AeronViewPose* reference, const AeronViewPose* pose,
							   AeronViewPose* out) {
	const float inverse[4] = { -reference->orientation[0], -reference->orientation[1],
							   -reference->orientation[2], reference->orientation[3] };
	float       orientation[4];
	head_quat_multiply(inverse, pose->orientation, orientation);
	/* The reference's matrix turns its own axes into the rig's. Its
	 * transpose does the opposite. */
	float rotation[9];
	AeronViewPose_Rotation(reference, rotation);
	float offset[3], position[3];
	for (int axis = 0; axis < 3; ++axis)
		offset[axis] = pose->position_m[axis] - reference->position_m[axis];
	for (int axis = 0; axis < 3; ++axis) {
		position[axis] = rotation[0 * 3 + axis] * offset[0] + rotation[1 * 3 + axis] * offset[1] +
						 rotation[2 * 3 + axis] * offset[2];
	}
	for (int axis = 0; axis < 3; ++axis)
		out->position_m[axis] = position[axis];
	for (int i = 0; i < 4; ++i)
		out->orientation[i] = orientation[i];
}
