#include "aeron/main.h"
#include "capture.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int capture_frames(Fixture* fixture) {
	const CaptureConfig* c      = &fixture->config;
	size_t               pixels = (size_t)c->width * c->height, count = pixels * 3;
	uint16_t*            staging   = malloc(pixels * 8);
	float*               blurred   = malloc(count * sizeof(float));
	float*               sample    = malloc(count * sizeof(float));
	float*               reference = c->reference_samples ? calloc(count, sizeof(float)) : NULL;
	int                  ok        = staging && blurred && sample && (!c->reference_samples || reference);
	for (int frame = 0; ok && frame < c->frames; ++frame) {
		double             time   = frame / c->fps;
		AeronRenderTarget* target = Fixture_Render(fixture, time, 1);
		ok = Capture_Read(target, c, staging, blurred) && Capture_Write(c, frame, "blur", blurred);
		if (!ok)
			break;
		target = Fixture_Render(fixture, time, 0);
		ok     = Capture_Read(target, c, staging, sample) && Capture_Write(c, frame, "sharp", sample);
		if (!ok)
			break;
		if (!c->reference_samples) {
			Aeron_LogInfo("motion_blur_capture", "Captured frame %d of %d", frame + 1, c->frames);
			continue;
		}
		memset(reference, 0, count * sizeof(float));
		/* Midpoint quadrature of an actual camera exposure, independent of
		 * the screen-space reconstruction shader and its velocity clamp. */
		for (int i = 0; ok && i < c->reference_samples; ++i) {
			double offset = ((i + .5) / c->reference_samples - .5) * c->shutter * .032;
			target        = Fixture_Render(fixture, time + offset, 0);
			ok            = Capture_Read(target, c, staging, sample);
			if (ok)
				for (size_t p = 0; p < count; ++p)
					reference[p] += sample[p];
		}
		if (ok) {
			for (size_t p = 0; p < count; ++p)
				reference[p] /= (float)c->reference_samples;
			ok = Capture_Write(c, frame, "reference", reference) &&
				 Capture_Report(c, frame, blurred, reference);
		}
		if (ok)
			Aeron_LogInfo("motion_blur_capture", "Captured frame %d of %d", frame + 1, c->frames);
	}
	free(staging);
	free(blurred);
	free(sample);
	free(reference);
	return ok;
}

int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "Usage: motion_blur_capture <capture.yaml relative to working directory>\n");
		return 2;
	}
	CaptureConfig c = { 0 };
	if (!CaptureConfig_Load(argv[1], &c))
		return 2;
	if (!SDL_CreateDirectory(c.output_dir)) {
		fprintf(stderr, "Cannot create output directory: %s\n", SDL_GetError());
		return 1;
	}
	/* Refuse accidental replacement of a previous run's captures. */
	char manifest[1200];
	snprintf(manifest, sizeof manifest, "%s/capture.yaml", c.output_dir);
	SDL_PathInfo info;
	if (SDL_GetPathInfo(manifest, &info)) {
		fprintf(stderr, "Output directory already contains a capture: %s\n", c.output_dir);
		return 2;
	}
	AeronConfig startup = { .org_name          = "Aeron",
							.app_name          = "MotionBlurCapture",
							.window_title      = "Aeron motion blur capture",
							.window_width      = 800,
							.window_height     = 450,
							.logical_width     = c.width,
							.logical_height    = c.height,
							.shader_path       = MB_CAPTURE_SHADER_DIR,
							.presentation_mode = AERON_PRESENTATION_ASPECT_FIT };
	if (!Aeron_Init(&startup))
		return 1;
	Fixture fixture;
	int     ok = Fixture_Create(&fixture, &c);
	if (ok) {
		ok = Capture_SaveConfig(&c) && capture_frames(&fixture);
		Fixture_Destroy(&fixture);
	}
	if (!ok) {
		const char* error = Aeron_RenderLastError();
		if (!error || !error[0])
			error = SDL_GetError();
		Aeron_LogError("motion_blur_capture", "Capture failed (allocation, rendering, or output): %s", error);
	}
	Aeron_Shutdown();
	return ok ? 0 : 1;
}
