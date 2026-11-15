#include "aeron/scene/view_rig.h"

#include <math.h>
#include <string.h>

void AeronViewFov_ToCamera(const AeronViewFov* fov, float* h_half_rad, float* v_half_rad,
						   float* proj_x_offset, float* proj_y_offset) {
	const float tan_left  = tanf(fov->angle_left);
	const float tan_right = tanf(fov->angle_right);
	const float tan_up    = tanf(fov->angle_up);
	const float tan_down  = tanf(fov->angle_down);
	/* The scene computes NDC x as x / (z * tan(h_half)) + offset. The half
	 * angle and the offset are chosen so that the left edge gives -1 and the
	 * right edge +1. Y works the same way, with the upper edge at +1. */
	*h_half_rad    = atanf((tan_right - tan_left) * 0.5f);
	*v_half_rad    = atanf((tan_up - tan_down) * 0.5f);
	*proj_x_offset = -(tan_right + tan_left) / (tan_right - tan_left);
	*proj_y_offset = -(tan_up + tan_down) / (tan_up - tan_down);
}

void AeronViewPose_Rotation(const AeronViewPose* pose, float out[9]) {
	const float x = pose->orientation[0], y = pose->orientation[1], z = pose->orientation[2];
	const float w  = pose->orientation[3];
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;
	out[0] = 1.0f - 2.0f * (yy + zz);
	out[1] = 2.0f * (xy - wz);
	out[2] = 2.0f * (xz + wy);
	out[3] = 2.0f * (xy + wz);
	out[4] = 1.0f - 2.0f * (xx + zz);
	out[5] = 2.0f * (yz - wx);
	out[6] = 2.0f * (xz - wy);
	out[7] = 2.0f * (yz + wx);
	out[8] = 1.0f - 2.0f * (xx + yy);
}

void AeronViewPose_ComposeCamera(const AeronViewPose* pose, const float seat_rows[9], const float seat_pos[3],
								 float units_per_meter, float out_rows[9], float out_pos[3]) {
	/* Eye space is rig space with Y and Z negated. */
	static const float flip[3] = { 1.0f, -1.0f, -1.0f };
	float              rotation[9];
	AeronViewPose_Rotation(pose, rotation);
	/* To go from the seat's eye space to the view's eye space: negate Y and
	 * Z to get rig space, rotate into the view's axes with the transpose of
	 * the pose's rotation, then negate Y and Z again. */
	float eye_from_seat[9];
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			eye_from_seat[row * 3 + column] = flip[row] * rotation[column * 3 + row] * flip[column];
	}
	float rows[9];
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) {
			rows[row * 3 + column] = eye_from_seat[row * 3 + 0] * seat_rows[0 * 3 + column] +
									 eye_from_seat[row * 3 + 1] * seat_rows[1 * 3 + column] +
									 eye_from_seat[row * 3 + 2] * seat_rows[2 * 3 + column];
		}
	}
	/* The view's offset from the seat, first in the seat's eye space, then
	 * in world space. */
	float offset[3];
	for (int axis = 0; axis < 3; ++axis)
		offset[axis] = flip[axis] * pose->position_m[axis] * units_per_meter;
	float position[3];
	for (int axis = 0; axis < 3; ++axis) {
		position[axis] = seat_pos[axis] + seat_rows[0 * 3 + axis] * offset[0] +
						 seat_rows[1 * 3 + axis] * offset[1] + seat_rows[2 * 3 + axis] * offset[2];
	}
	memcpy(out_rows, rows, sizeof rows);
	memcpy(out_pos, position, sizeof position);
}

static void rig_identity_pose(AeronViewPose* pose) {
	memset(pose, 0, sizeof *pose);
	pose->orientation[3] = 1.0f;
}

/* Writes the quaternion (x, y, z, w) of a rotation around the Y axis. */
static void rig_yaw_orientation(float angle_rad, float out[4]) {
	out[0] = 0.0f;
	out[1] = sinf(angle_rad * 0.5f);
	out[2] = 0.0f;
	out[3] = cosf(angle_rad * 0.5f);
}

/* Computes the view for one vertical screen, with the eye at the rig's
 * origin. `left` and `right` are the left and right ends of the picture, at
 * eye height. The view looks straight at the screen's plane, and its frustum
 * passes through the picture's edges. Returns zero when the eye isn't in
 * front of the screen. */
static int rig_screen_view(const float left[3], const float right[3], float half_height, AeronRigView* view) {
	const float dx    = right[0] - left[0];
	const float dz    = right[2] - left[2];
	const float width = sqrtf(dx * dx + dz * dz);
	if (!(width > 0.0f))
		return 0;
	/* The screen's right axis and its normal. The normal is right cross up,
	 * so it points toward the viewer. */
	const float right_x  = dx / width;
	const float right_z  = dz / width;
	const float normal_x = -right_z;
	const float normal_z = right_x;
	const float distance = -(normal_x * left[0] + normal_z * left[2]);
	if (!(distance > 0.0f))
		return 0;
	rig_identity_pose(&view->pose);
	/* The view is the rig turned around Y until its right axis matches the
	 * screen's. */
	rig_yaw_orientation(atan2f(-right_z, right_x), view->pose.orientation);
	view->fov.angle_left  = atanf((right_x * left[0] + right_z * left[2]) / distance);
	view->fov.angle_right = atanf((right_x * right[0] + right_z * right[2]) / distance);
	view->fov.angle_up    = atanf(half_height / distance);
	view->fov.angle_down  = -view->fov.angle_up;
	return 1;
}

int AeronViewRig_FromSurround(const AeronSurroundDesc* desc, const AeronRectI rects[3], AeronViewRig* out) {
	if (!desc || !rects || !out || !(desc->screen_width_m > 0.0f) || !(desc->screen_height_m > 0.0f) ||
		!(desc->distance_m > 0.0f) || desc->bezel_m < 0.0f || rects[0].width <= 0 || rects[0].height <= 0) {
		return 0;
	}
	for (int i = 1; i < 3; ++i) {
		if (rects[i].width != rects[0].width || rects[i].height != rects[0].height)
			return 0;
	}
	const float half_width  = desc->screen_width_m * 0.5f;
	const float half_height = desc->screen_height_m * 0.5f;
	const float hinge_x     = half_width + desc->bezel_m;
	const float along_x     = cosf(desc->side_angle_rad);
	const float along_z     = sinf(desc->side_angle_rad);
	const float near_edge   = desc->bezel_m;
	const float far_edge    = desc->bezel_m + desc->screen_width_m;
	/* The left and right ends of each picture. A side screen goes outward
	 * from its hinge and toward the viewer (+Z). */
	const float ends[3][2][3] = {
		{ { -hinge_x - far_edge * along_x, 0.0f, -desc->distance_m + far_edge * along_z },
		  { -hinge_x - near_edge * along_x, 0.0f, -desc->distance_m + near_edge * along_z } },
		{ { -half_width, 0.0f, -desc->distance_m }, { half_width, 0.0f, -desc->distance_m } },
		{ { hinge_x + near_edge * along_x, 0.0f, -desc->distance_m + near_edge * along_z },
		  { hinge_x + far_edge * along_x, 0.0f, -desc->distance_m + far_edge * along_z } },
	};
	AeronViewRig rig;
	memset(&rig, 0, sizeof rig);
	for (int i = 0; i < 3; ++i) {
		if (!rig_screen_view(ends[i][0], ends[i][1], half_height, &rig.views[i]))
			return 0;
		rig.views[i].rect = rects[i];
	}
	rig.count = 3;
	rig_identity_pose(&rig.center);
	rig.width   = rects[0].width;
	rig.height  = rects[0].height;
	rig.primary = 1;
	*out        = rig;
	return 1;
}

int AeronViewRig_DebugStereo(float ipd_m, const AeronViewFov* fov, const AeronViewPose* head, int view_width,
							 int view_height, AeronViewRig* out) {
	if (!fov || !head || !out || view_width <= 0 || view_height <= 0)
		return 0;
	float rotation[9];
	AeronViewPose_Rotation(head, rotation);
	AeronViewRig rig;
	memset(&rig, 0, sizeof rig);
	for (int eye = 0; eye < 2; ++eye) {
		/* Each eye is half of `ipd_m` from the head, along the head's right
		 * axis. */
		const float   side = (eye == 0 ? -0.5f : 0.5f) * ipd_m;
		AeronRigView* view = &rig.views[eye];
		view->pose         = *head;
		for (int axis = 0; axis < 3; ++axis)
			view->pose.position_m[axis] += rotation[axis * 3 + 0] * side;
		view->fov  = *fov;
		view->rect = (AeronRectI) { eye * view_width, 0, view_width, view_height };
	}
	rig.count          = 2;
	rig.center         = *head;
	rig.width          = view_width;
	rig.height         = view_height;
	rig.primary        = 0;
	rig.tracked        = 1;
	rig.shared_sprites = 1;
	*out               = rig;
	return 1;
}
