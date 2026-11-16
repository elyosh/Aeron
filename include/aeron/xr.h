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

/* Loads the OpenXR loader, asks it for the runtime's extensions, and unloads
 * it. Returns nonzero when a runtime answered. This is for tools and
 * diagnostics. A game that runs with AERON_XR_OFF must not call it. */
int Aeron_XrProbeRuntime(void);

/* Nonzero when the GPU device has an OpenXR session. */
int Aeron_XrEnabled(void);
/* Nonzero while the session runs. OpenXR then paces the frame. */
int Aeron_XrSessionRunning(void);
/* Nonzero while the session has the user's input focus. */
int Aeron_XrFocused(void);
/* Makes the current head position and yaw the origin of the rig. */
void Aeron_XrRecenter(void);
/* Size of one eye image in pixels. Returns 0 when VR is off. */
int Aeron_XrViewSize(int* width, int* height);

typedef struct AeronXrFrame {
	AeronViewRig       rig;
	AeronRenderTarget* color[AERON_MAX_VIEWS]; /* borrowed until Aeron_Present */
} AeronXrFrame;

/* Locates the views and acquires the eye images. Returns 0 when this frame
 * must not render views. Call at most once per frame. */
int Aeron_XrBeginViews(AeronXrFrame* out);
/* The eye images are complete. Without this call Aeron_Present releases
 * them and leaves the projection layer out. */
void Aeron_XrSubmitViews(void);
/* Draws into the window after the frame's XR work, for the eye mirror. */
int Aeron_XrSubmitMirror(const AeronSwapchainRenderLayerDesc* desc);

#ifdef __cplusplus
}
#endif

#endif
