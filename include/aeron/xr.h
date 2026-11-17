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

/* The hand that points at the virtual screen and moves the mouse of the
 * input snapshot in this frame: 0 for the left hand, 1 for the right one, or
 * -1 when the mouse is the real one. Its trigger is the left mouse button,
 * its squeeze the right one, and its thumbstick scrolls. */
int Aeron_XrPointerHand(void);

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
/* As Aeron_XrSubmitViews, for eye images that only hold something to show
 * over the virtual screen, such as the beam of a hand that points at it.
 * The headset then shows the screen first, as on a frame without eye
 * images, and the eye images over it: they must be transparent where they
 * are empty, with colours premultiplied by alpha. */
void Aeron_XrSubmitViewsOver(void);

/* The runtime's model of the controller that a hand holds: a binary glTF
 * 2.0 file, and the names of the nodes of that file that the runtime moves,
 * such as the trigger and the buttons. Few runtimes give models (SteamVR
 * does), and only while a session runs. */
#define AERON_XR_MODEL_NODE_MAX 64

typedef struct AeronXrControllerModel {
	/* Changes when the runtime's models change: the file must be read
	 * again. */
	uint32_t generation;
	/* Borrowed until the next Aeron_BeginFrame. */
	const uint8_t* glb;
	size_t         glb_size;
	int            node_count;
	const char*    node_names[AERON_XR_MODEL_NODE_MAX];
} AeronXrControllerModel;

/* Gives the model of a hand's controller. `hand` is 0 for the left hand and
 * 1 for the right one. Returns 0 when the runtime has none. */
int Aeron_XrControllerModel(int hand, AeronXrControllerModel* out);

typedef struct AeronXrControllerModelState {
	/* The model's origin in the rig's space. */
	AeronViewPose pose;
	/* For each moving node, in the order of the model's node_names: its pose
	 * in the space of its parent node, in place of the pose that the file
	 * gives it, and whether it is shown. A node that isn't shown hides its
	 * descendants. node_count is 0 when the runtime gave no states: the
	 * model is then at rest. */
	int           node_count;
	AeronViewPose nodes[AERON_XR_MODEL_NODE_MAX];
	uint8_t       node_visible[AERON_XR_MODEL_NODE_MAX];
} AeronXrControllerModelState;

/* Gives where a hand's controller model is, and how its moving nodes are,
 * when the current frame is shown. Returns 0 when the hand has no model or
 * the runtime doesn't know where it is. */
int Aeron_XrControllerModelState(int hand, AeronXrControllerModelState* out);

/* The panel: a second flat surface in the headset, beside the virtual
 * screen, that the application draws itself, such as a keyboard. */
typedef struct AeronXrPanelDesc {
	int           width_px, height_px; /* the size of its image */
	float         width_m;             /* its width in the headset; the height follows the image's shape */
	AeronViewPose pose;                /* its centre in the rig's space, with X to the right and Y up on it */
} AeronXrPanelDesc;

/* Returns the image to draw the panel into for this frame, or NULL when no
 * VR session runs or the frame isn't rendered. The headset shows the panel
 * in the frames where this is called, over the virtual screen and under the
 * eye images of Aeron_XrSubmitViewsOver. The image must be transparent
 * where the panel is empty, with colours premultiplied by alpha. The image
 * is created at the first call, and again when the size in pixels changes. */
AeronRenderTarget* Aeron_XrPanelBegin(const AeronXrPanelDesc* desc);
/* Finds where the aim ray of a hand meets the panel of the last
 * Aeron_XrPanelBegin: `x` and `y` are in the image's pixels, from its top
 * left corner. `hand` is 0 for the left hand and 1 for the right one.
 * Returns 0 when the ray misses the panel, or there is no panel. */
int Aeron_XrPanelHit(int hand, float* x, float* y);
/* The point of the panel at the pixel (x, y), in the rig's space. Returns 0
 * when there is no panel. */
int Aeron_XrPanelPoint(float x, float y, float point_m[3]);

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
