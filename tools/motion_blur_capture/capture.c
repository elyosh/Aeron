#include "capture.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static float half_to_float(uint16_t half) {
	uint32_t exponent = (half >> 10) & 31u, mantissa = half & 1023u;
	if (!exponent) {
		float value = mantissa ? ldexpf((float)mantissa, -24) : 0;
		return half & 0x8000u ? -value : value;
	}
	uint32_t bits = ((uint32_t)(half & 0x8000u) << 16) |
					(exponent == 31 ? 0x7f800000u : (exponent + 112u) << 23) | (mantissa << 13);
	float    result;
	memcpy(&result, &bits, sizeof result);
	return result;
}

int Capture_Read(AeronRenderTarget* target, const CaptureConfig* c, uint16_t* staging, float* rgb) {
	size_t pixels = (size_t)c->width * (size_t)c->height;
	if (!target || !Aeron_ReadRenderTargetRawPixels(target, staging, pixels * 8, (size_t)c->width * 8))
		return 0;
	for (size_t i = 0; i < pixels; ++i) {
		for (int channel = 0; channel < 3; ++channel) {
			float value = half_to_float(staging[i * 4 + channel]);
			if (!isfinite(value)) {
				Aeron_LogError("motion_blur_capture", "Nonfinite pixel at %zu", i);
				return 0;
			}
			rgb[i * 3 + channel] = value;
		}
	}
	return 1;
}

static int output_path(const CaptureConfig* c, int frame, const char* name, const char* extension, char* path,
					   size_t capacity) {
	int n = snprintf(path, capacity, "%s/frame_%03d_%s.%s", c->output_dir, frame, name, extension);
	return n > 0 && (size_t)n < capacity;
}

typedef struct PngWriter {
	SDL_IOStream* stream;
	int           ok;
} PngWriter;

static void png_write(void* context, void* data, int size) {
	PngWriter* w = context;
	if (w->ok && SDL_WriteIO(w->stream, data, (size_t)size) != (size_t)size)
		w->ok = 0;
}

static int write_png(const char* path, int width, int height, const float* rgb) {
	size_t   count = (size_t)width * (size_t)height * 3;
	uint8_t* bytes = malloc(count);
	if (!bytes)
		return 0;
	for (size_t i = 0; i < count; ++i) {
		float value = fminf(1, fmaxf(0, rgb[i]));
		float srgb  = value <= .0031308f ? 12.92f * value : 1.055f * powf(value, 1 / 2.4f) - .055f;
		bytes[i]    = (uint8_t)(srgb * 255 + .5f);
	}
	PngWriter writer = { .stream = SDL_IOFromFile(path, "wb"), .ok = 1 };
	int ok = writer.stream &&
			 stbi_write_png_to_func(png_write, &writer, width, height, 3, bytes, width * 3) && writer.ok;
	if (writer.stream && !SDL_CloseIO(writer.stream))
		ok = 0;
	free(bytes);
	return ok;
}

static int write_pfm(const char* path, int width, int height, const float* rgb) {
	SDL_IOStream* stream = SDL_IOFromFile(path, "wb");
	if (!stream)
		return 0;
	char   header[64];
	int    n         = snprintf(header, sizeof header, "PF\n%d %d\n%s1.0\n", width, height,
								SDL_BYTEORDER == SDL_LIL_ENDIAN ? "-" : "");
	int    ok        = SDL_WriteIO(stream, header, (size_t)n) == (size_t)n;
	size_t row_bytes = (size_t)width * 3 * sizeof(float);
	for (int y = height - 1; ok && y >= 0; --y)
		ok = SDL_WriteIO(stream, rgb + (size_t)y * width * 3, row_bytes) == row_bytes;
	if (!SDL_CloseIO(stream))
		ok = 0;
	return ok;
}

int Capture_Write(const CaptureConfig* c, int frame, const char* name, const float* rgb) {
	char path[1200];
	if (!output_path(c, frame, name, "pfm", path, sizeof path) || !write_pfm(path, c->width, c->height, rgb))
		return 0;
	return output_path(c, frame, name, "png", path, sizeof path) && write_png(path, c->width, c->height, rgb);
}

static int write_text(const char* path, const char* text) {
	SDL_IOStream* stream = SDL_IOFromFile(path, "wb");
	if (!stream)
		return 0;
	size_t size = strlen(text);
	int    ok   = SDL_WriteIO(stream, text, size) == size;
	if (!SDL_CloseIO(stream))
		ok = 0;
	return ok;
}

int Capture_SaveConfig(const CaptureConfig* c) {
	char path[1200], text[2048];
	int  n = snprintf(path, sizeof path, "%s/capture.yaml", c->output_dir);
	if (n < 0 || (size_t)n >= sizeof path)
		return 0;
	snprintf(text, sizeof text,
			 "fixture_version: 2\nbackend: %s\nwidth: %d\nheight: %d\nframes: %d\n"
			 "fps: %.9g\npan_degrees_per_second: %.9g\npan_direction_degrees: %.9g\n"
			 "quality: %d\nshutter: %.9g\nexposure_seconds: %.9g\nreference_samples: %d\n"
			 "reference_kernel: centered_box\nimage_data: linear_RGB_float32_PFM\n"
			 "preview: sRGB_PNG_clamped_0_to_1\nmsaa_samples: 1\ntemporal_upscaling: off\n"
			 "ssao: off\nshadows: off\nbloom: off\n",
			 Aeron_RenderDriverName(), c->width, c->height, c->frames, c->fps, c->pan_speed, c->pan_direction,
			 c->quality, c->shutter, c->shutter * .032, c->reference_samples);
	return write_text(path, text);
}

int Capture_Report(const CaptureConfig* c, int frame, const float* actual, const float* reference) {
	size_t count = (size_t)c->width * c->height * 3, active = 0;
	double squared = 0, absolute = 0, bias = 0, maximum = 0;
	for (size_t i = 0; i < count; i += 3) {
		float peak = 0;
		for (int k = 0; k < 3; ++k)
			peak = fmaxf(peak, fmaxf(actual[i + k], reference[i + k]));
		if (peak <= .02f)
			continue;
		for (int k = 0; k < 3; ++k) {
			double error = (double)actual[i + k] - reference[i + k];
			squared += error * error;
			absolute += fabs(error);
			bias += error;
			maximum = fmax(maximum, fabs(error));
			++active;
		}
	}
	char path[1200], text[1024];
	if (!output_path(c, frame, "metrics", "yaml", path, sizeof path))
		return 0;
	double divisor = (double)(active ? active : 1);
	snprintf(text, sizeof text,
			 "frame: %d\ntime_seconds: %.9g\nactive_pixels: %zu\n"
			 "active_threshold: 0.02\nrmse: %.9g\nmae: %.9g\nmean_error: %.9g\nmax_error: %.9g\n"
			 "note: Differences include shutter-kernel, clamp, visibility and sampling errors.\n",
			 frame, frame / c->fps, active / 3, sqrt(squared / divisor), absolute / divisor, bias / divisor,
			 maximum);
	return write_text(path, text);
}
