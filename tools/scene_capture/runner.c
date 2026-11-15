#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ViewCapture {
	const CaptureConfig* config;
	int                  frame;
	uint16_t*            staging;
	/* One image for each view. */
	float* rgb[3];
} ViewCapture;

/* Keeps one view of a frame and saves it as frame_NNN_viewK. */
static int capture_view(void* user, int view, AeronRenderTarget* target) {
	const ViewCapture* capture = user;
	char               name[16];
	snprintf(name, sizeof name, "view%d", view);
	return Capture_Read(target, capture->config->width, capture->config->height, capture->staging,
						capture->rgb[view]) &&
		   Capture_Write(capture->config, capture->frame, name, capture->rgb[view]);
}

/* Renders the frame into all its views, then each view alone, and compares
 * the two renders of each view. */
static int capture_views(Fixture* fixture, int frame, double time, uint16_t* staging, float* alone) {
	const CaptureConfig* c       = &fixture->config;
	const size_t         count   = (size_t)c->width * c->height * 3;
	ViewCapture          capture = { c, frame, staging, { NULL, NULL, NULL } };
	int                  ok      = 1;
	for (int view = 0; view < c->views; ++view) {
		capture.rgb[view] = malloc(count * sizeof(float));
		ok                = ok && capture.rgb[view];
	}
	ok = ok && Fixture_RenderViews(fixture, time, capture_view, &capture);
	for (int view = 0; ok && view < c->views; ++view) {
		char name[24];
		snprintf(name, sizeof name, "view%d_alone", view);
		ok =
			Capture_Read(Fixture_RenderViewAlone(fixture, time, view), c->width, c->height, staging, alone) &&
			Capture_Write(c, frame, name, alone) &&
			Capture_CompareView(c, frame, view, capture.rgb[view], alone);
	}
	for (int view = 0; view < c->views; ++view)
		free(capture.rgb[view]);
	return ok;
}

int Capture_Run(Fixture* fixture) {
	const CaptureConfig* c      = &fixture->config;
	size_t               pixels = (size_t)c->width * c->height, count = pixels * 3;
	uint16_t*            staging   = malloc(pixels * 8);
	float*               blurred   = malloc(count * sizeof(float));
	float*               sample    = malloc(count * sizeof(float));
	float*               reference = c->reference_samples ? calloc(count, sizeof(float)) : NULL;
	int                  ok        = staging && blurred && sample && (!c->reference_samples || reference);
	CaptureBloom         bloom     = { 0 };
	if (ok && c->bloom_enabled)
		ok = CaptureBloom_Create(&bloom, c, 1);
	for (int frame = 0; ok && frame < c->frames; ++frame) {
		double time = frame / c->fps;
		if (c->views > 1) {
			ok = capture_views(fixture, frame, time, staging, sample);
			if (ok)
				Aeron_LogInfo("scene_capture", "Captured frame %d of %d in %d views", frame + 1, c->frames,
							  c->views);
			continue;
		}
		AeronRenderTarget* target = Fixture_Render(fixture, time, 1);
		ok                        = Capture_Read(target, c->width, c->height, staging, blurred) &&
									Capture_Write(c, frame, c->bloom_chart ? "scene" : "blur", blurred);
		if (!ok)
			break;
		if (c->bloom_enabled) {
			ok = CaptureBloom_Render(&bloom, c, target) &&
				 Capture_Read(bloom.readback_rt, bloom.width, bloom.height, staging, sample) &&
				 Capture_WriteImage(c, frame, "bloom", bloom.width, bloom.height, sample) &&
				 CaptureBloom_Report(c, frame, blurred, sample, bloom.width, bloom.height) &&
				 Capture_Read(bloom.present_rt, c->width, c->height, staging, sample) &&
				 Capture_Write(c, frame, "present", sample);
			if (!ok)
				break;
		}
		if (c->bloom_chart) {
			Aeron_LogInfo("scene_capture", "Captured frame %d of %d", frame + 1, c->frames);
			continue;
		}
		target = Fixture_Render(fixture, time, 0);
		ok     = Capture_Read(target, c->width, c->height, staging, sample) &&
				 Capture_Write(c, frame, "sharp", sample);
		if (!ok)
			break;
		if (!c->reference_samples) {
			Aeron_LogInfo("scene_capture", "Captured frame %d of %d", frame + 1, c->frames);
			continue;
		}
		memset(reference, 0, count * sizeof(float));
		/* Midpoint quadrature of an actual camera exposure, independent of
		 * the screen-space reconstruction shader and its velocity clamp. */
		for (int i = 0; ok && i < c->reference_samples; ++i) {
			double offset = ((i + .5) / c->reference_samples - .5) * c->shutter * .032;
			target        = Fixture_Render(fixture, time + offset, 0);
			ok            = Capture_Read(target, c->width, c->height, staging, sample);
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
			Aeron_LogInfo("scene_capture", "Captured frame %d of %d", frame + 1, c->frames);
	}
	CaptureBloom_Destroy(&bloom);
	free(staging);
	free(blurred);
	free(sample);
	free(reference);
	return ok;
}
