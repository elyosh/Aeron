#ifndef AERON_XR_H
#define AERON_XR_H

#include "aeron/render.h"
#include "aeron/scene/view_rig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* VR through OpenXR. Aeron owns the OpenXR instance, the session, the eye
 * images and the frame pacing. A game only sees the types in this header.
 *
 * These functions exist in every build. In a build without AERON_XR, and
 * when the device has no session, they report that VR is off. */

typedef enum AeronXrMode {
	AERON_XR_OFF,      /* never load OpenXR */
	AERON_XR_AUTO,     /* use a headset when one is found */
	AERON_XR_REQUIRED, /* fail at startup when there is no headset */
} AeronXrMode;

/* Nonzero when this build has VR support. It is zero on macOS. */
int Aeron_XrSupported(void);
/* Nonzero when the GPU device has an OpenXR session. */
int Aeron_XrEnabled(void);
/* Nonzero while the session runs. OpenXR then paces the frame. */
int Aeron_XrSessionRunning(void);
/* Nonzero while the session has the user's input focus. */
int Aeron_XrFocused(void);
/* Makes the current head position and yaw the origin of the rig. */
void Aeron_XrRecenter(void);
/* Sets the size of the virtual screen, as a multiple of the width given in the
 * startup options. The screen keeps its distance and its shape, so this changes
 * how large it looks. It applies from the next frame. Values of 0 or less are
 * ignored. */
void Aeron_XrSetScreenScale(float scale);
/* The size of one eye image, in pixels: the size the runtime recommends,
 * times the render scale of the startup options. Returns 0 when VR is off. */
int Aeron_XrViewSize(int* width, int* height);

/* One hand's controller, in the space of the view rig. */
typedef struct AeronXrHand {
	/* The controller is on and the headset knows where it is. */
	int active;
	/* The pose of the palm, and the pose to point with: the ray starts at
	 * the aim pose's position and goes along its -Z axis. */
	AeronViewPose grip;
	AeronViewPose aim;
	/* How far the trigger and the squeeze are pressed, from 0 to 1. */
	float trigger;
	float squeeze;
} AeronXrHand;

/* Gives the two hands, where they will be when the current frame is shown.
 * Index 0 is the left hand. Returns 0 when neither hand is active. */
int Aeron_XrHands(AeronXrHand out[2]);

/* A hand for tests without a headset. `buttons` has one bit for each
 * AeronVrHandButton. A pressed trigger or squeeze button also sets its axis
 * to the full value. */
typedef struct AeronXrFakeHand {
	int           active;
	AeronViewPose pose; /* both the grip pose and the aim pose */
	uint32_t      buttons;
} AeronXrFakeHand;

/* Sets the two hands that the input snapshot and Aeron_XrHands report while
 * the device has no VR session. Index 0 is the left hand. NULL removes
 * them. */
void Aeron_XrSetFakeHands(const AeronXrFakeHand hands[2]);

/* Gives the virtual screen's rectangle in the space of the view rig: the
 * pose of its centre, with X to the right and Y up on the screen, and its
 * size in metres. Returns 0 when the headset didn't show the screen in the
 * last frame. */
int Aeron_XrScreenQuad(AeronViewPose* pose, float* width_m, float* height_m);

/* A part of an eye image, as a list of triangles: two floats for each
 * vertex, X to the right and Y up, on the plane one unit in front of the
 * eye. `vertex_count` is zero when the headset gives none. `generation`
 * changes when the mesh changes. */
typedef struct AeronXrAreaMesh {
	const float* xy;
	int          vertex_count;
	uint32_t     generation;
} AeronXrAreaMesh;

typedef struct AeronXrFrame {
	AeronViewRig       rig;
	AeronRenderTarget* color[AERON_MAX_VIEWS]; /* borrowed until Aeron_Present */
	/* The part of each eye image that the headset's lens never shows, and
	 * the part that it shows. Borrowed until the next Aeron_XrBeginViews. */
	AeronXrAreaMesh hidden_area[AERON_MAX_VIEWS];
	AeronXrAreaMesh visible_area[AERON_MAX_VIEWS];
} AeronXrFrame;

/* Locates the views and acquires the eye images. Returns 0 when this frame
 * must not render views. Call at most once per frame. */
int Aeron_XrBeginViews(AeronXrFrame* out);
/* The eye images are complete. Without this call Aeron_Present releases
 * them and leaves the projection layer out. */
void Aeron_XrSubmitViews(void);
/* Asks Aeron_Present to call `desc->callback` while it draws the window, so
 * the application can show there what the headset shows. The callback draws
 * into the whole window, behind the copy of the virtual screen. A required
 * size of zero accepts any window size; otherwise the callback is skipped
 * when the window has another size. Returns 0 when no VR session runs. */
int Aeron_XrSubmitMirror(const AeronSwapchainRenderLayerDesc* desc);

#ifdef __cplusplus
}
#endif

#endif
