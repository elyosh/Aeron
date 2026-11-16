/* The OpenXR session and its state. The device has no session yet, so every
 * function reports that VR is off. */

#include "xr_internal.h"

#include <string.h>

int Aeron_XrEnabled(void) {
	return 0;
}

int Aeron_XrSessionRunning(void) {
	return 0;
}

int Aeron_XrFocused(void) {
	return 0;
}

void Aeron_XrRecenter(void) {
}

int Aeron_XrViewSize(int* width, int* height) {
	if (width)
		*width = 0;
	if (height)
		*height = 0;
	return 0;
}

int Aeron_XrBeginViews(AeronXrFrame* out) {
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

void Aeron_XrSubmitViews(void) {
}

int Aeron_XrSubmitMirror(const AeronSwapchainRenderLayerDesc* desc) {
	(void)desc;
	return 0;
}
