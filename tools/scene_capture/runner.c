#include "capture.h"

#include <stdlib.h>
#include <string.h>

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
		double             time   = frame / c->fps;
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
