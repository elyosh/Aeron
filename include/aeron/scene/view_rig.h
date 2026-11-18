#ifndef AERON_SCENE_VIEW_RIG_H
#define AERON_SCENE_VIEW_RIG_H

#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A view rig lists the views that one frame is rendered into: the two eyes
 * of a headset, or the screens of a surround setup. Each view has a pose and
 * a field of view. This file only does math and draws nothing.
 *
 * Rig space uses the OpenXR axes: X right, Y up, -Z forward, in metres.
 * The scene camera's eye space is different: X right, Y down, Z forward. */

#define AERON_MAX_VIEWS 4

/* A pose in rig space. The orientation is a quaternion (x, y, z, w) that
 * rotates from the view's axes to the rig's axes. */
typedef struct AeronViewPose {
	float position_m[3];
	float orientation[4];
} AeronViewPose;

/* The four edges of a view's frustum, as angles in radians from the view's
 * axis. The angles are signed, as in OpenXR: left and down are normally
 * negative. */
typedef struct AeronViewFov {
	float angle_left;
	float angle_right;
	float angle_up;
	float angle_down;
} AeronViewFov;

typedef struct AeronRigView {
	AeronViewPose pose;
	AeronViewFov  fov;
	/* The part of the output target that the view is drawn into, in pixels. */
	AeronRectI rect;
} AeronRigView;

typedef struct AeronViewRig {
	int          count;
	AeronRigView views[AERON_MAX_VIEWS];
	/* The pose of the head, between the eyes. It is the identity when the
	 * head isn't tracked. */
	AeronViewPose center;
	/* The size of one view in pixels. All views of a rig have the same size. */
	int width;
	int height;
	/* The index of the view that sets the exposure. In surround, the HUD is
	 * also drawn in this view. */
	int primary;
	/* Nonzero when the poses can change from one frame to the next. */
	int tracked;
	/* Nonzero when sprites can be built once, facing the head, and drawn in
	 * every view. Otherwise they are built for each view. */
	int shared_sprites;
	/* The caller changes this value when the views' history must be reset,
	 * for example after the headset is recentred. */
	uint64_t generation;
} AeronViewRig;

/* Three flat screens of the same size. The centre screen faces the viewer,
 * `distance_m` away. Each side screen is turned toward the viewer by
 * `side_angle_rad`, around a vertical hinge. The hinge is one bezel width
 * past the edge of the centre picture, and the side picture starts one more
 * bezel width after the hinge. The width and height are those of the
 * picture, without the bezel. */
typedef struct AeronSurroundDesc {
	float screen_width_m;
	float screen_height_m;
	float bezel_m;
	float distance_m;
	float side_angle_rad;
} AeronSurroundDesc;

/* Converts a field of view to the values AeronSceneCamera uses: the half
 * angles and the projection offsets. */
void AeronViewFov_ToCamera(const AeronViewFov* fov, float* h_half_rad, float* v_half_rad,
						   float* proj_x_offset, float* proj_y_offset);

/* Projects points on the plane one unit in front of the eye, with X to the
 * right and Y up, to clip space, for a camera with these half angles and
 * projection offsets. A headset gives the hidden and visible areas of an eye
 * image as meshes on that plane. `xy` and `clip_xy` have two floats for each
 * point. */
void AeronView_PlaneToClip(const float* xy, int count, float h_half_rad, float v_half_rad,
						   float proj_x_offset, float proj_y_offset, float* clip_xy);

/* Converts the pose's orientation to a row-major 3x3 matrix. The matrix
 * rotates from the view's axes to the rig's axes. */
void AeronViewPose_Rotation(const AeronViewPose* pose, float out[9]);

/* Builds the orientation of a head or a hand from its yaw, pitch and roll, in
 * radians: a positive yaw turns right, a positive pitch turns up, and a
 * positive roll turns clockwise as seen from behind. The turns are applied
 * in that order, each around the axes that the earlier turns gave. */
void AeronViewPose_FromAngles(float yaw_rad, float pitch_rad, float roll_rad, float out_orientation[4]);

/* Computes the camera of a view from the camera at the rig's origin, called
 * the seat camera. `seat_rows` are the seat camera's world-to-eye rows, in
 * the scene's eye space, and `seat_pos` is its position in scene units.
 * `units_per_meter` is the number of scene units in one metre. The function
 * writes the view's world-to-eye rows and its position. */
void AeronViewPose_ComposeCamera(const AeronViewPose* pose, const float seat_rows[9], const float seat_pos[3],
								 float units_per_meter, float out_rows[9], float out_pos[3]);

/* Builds the rig of a surround setup. The eye is in front of the middle of
 * the centre screen. `rects` says where the left, centre and right screens
 * are in the output target; the three rectangles must have the same size.
 * The views are in the same order, and the centre view is the primary one.
 * Returns zero when the setup isn't valid: a size or the distance is zero or
 * negative, the rectangles differ in size, or a side screen is turned so far
 * that the eye is behind it. */
int AeronViewRig_FromSurround(const AeronSurroundDesc* desc, const AeronRectI rects[3], AeronViewRig* out);

/* Builds a stereo rig for testing without a headset. The two eyes are
 * `ipd_m` apart, look the same way as the head, and both use `fov`. The left
 * eye is view 0 and is drawn at {0, 0}; the right eye is drawn next to it.
 * Returns zero when a view size is zero or negative. */
int AeronViewRig_DebugStereo(float ipd_m, const AeronViewFov* fov, const AeronViewPose* head, int view_width,
							 int view_height, AeronViewRig* out);

#ifdef __cplusplus
}
#endif

#endif
