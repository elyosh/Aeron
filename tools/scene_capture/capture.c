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

int Capture_Read(AeronRenderTarget* target, int width, int height, uint16_t* staging, float* rgb) {
	AeronTexture* texture = Aeron_RenderTargetGetTexture(target);
	size_t        pixels  = (size_t)width * (size_t)height;
	if (!texture || Aeron_TextureGetWidth(texture) != width || Aeron_TextureGetHeight(texture) != height ||
		Aeron_TextureGetFormat(texture) != AERON_TEXTURE_FORMAT_RGBA16_FLOAT ||
		!Aeron_ReadRenderTargetRawPixels(target, staging, pixels * 8, (size_t)width * 8))
		return 0;
	for (size_t i = 0; i < pixels; ++i) {
		for (int channel = 0; channel < 3; ++channel) {
			float value = half_to_float(staging[i * 4 + channel]);
			if (!isfinite(value)) {
				Aeron_LogError("scene_capture", "Nonfinite pixel at %zu", i);
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
	return Capture_WriteImage(c, frame, name, c->width, c->height, rgb);
}

int Capture_WriteImage(const CaptureConfig* c, int frame, const char* name, int width, int height,
					   const float* rgb) {
	char path[1200];
	if (!output_path(c, frame, name, "pfm", path, sizeof path) || !write_pfm(path, width, height, rgb))
		return 0;
	return output_path(c, frame, name, "png", path, sizeof path) && write_png(path, width, height, rgb);
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

static int write_yaml_path(SDL_IOStream* stream, const char* prefix, const char* value) {
	char line[2080];
	int  n = snprintf(line, sizeof line, "%s'", prefix);
	if (n < 0 || (size_t)n >= sizeof line)
		return 0;
	for (; *value; ++value) {
		if ((size_t)n + 5 >= sizeof line)
			return 0;
		line[n++] = *value;
		if (*value == '\'')
			line[n++] = '\'';
	}
	line[n++] = '\'';
	line[n++] = '\n';
	return SDL_WriteIO(stream, line, (size_t)n) == (size_t)n;
}

int Capture_SaveConfig(const CaptureConfig* c) {
	char path[1200], text[2048];
	int  n = snprintf(path, sizeof path, "%s/capture.yaml", c->output_dir);
	if (n < 0 || (size_t)n >= sizeof path)
		return 0;
	snprintf(text, sizeof text,
			 "fixture_version: %d\nbackend: %s\nwidth: %d\nheight: %d\nframes: %d\n"
			 "fps: %.9g\npan_degrees_per_second: %.9g\npan_direction_degrees: %.9g\n"
			 "quality: %d\nshutter: %.9g\nexposure_seconds: %.9g\nreference_samples: %d\n"
			 "reference_kernel: centered_box\nimage_data: linear_RGB_float32_PFM\n"
			 "preview: sRGB_PNG_clamped_0_to_1\nmsaa_samples: %d\ntemporal_upscaling: off\n"
			 "ssao: off\nshadows: off\nfixture: %s\nemissive_scale: %.9g\nbloom_enabled: %s\n",
			 c->bloom_chart ? 1 : (c->model_count ? 6 : 3), Aeron_RenderDriverName(), c->width, c->height,
			 c->frames, c->fps, c->pan_speed, c->pan_direction, c->quality, c->shutter, c->shutter * .032,
			 c->reference_samples, c->msaa_samples,
			 c->bloom_chart ? "bloom_chart" : (c->model_count ? "models" : "shapes"), c->emissive_scale,
			 c->bloom_enabled ? "true" : "false");
	SDL_IOStream* stream = SDL_IOFromFile(path, "wb");
	if (!stream)
		return 0;
	int ok = SDL_WriteIO(stream, text, strlen(text)) == strlen(text);
	if (ok && (c->bloom_enabled || c->benchmark_mode)) {
		snprintf(text, sizeof text,
				 "bloom:\n  intensity: %.9g\n  kernel: %d\n"
				 "bloom_capture: native_size_linear_RGB_before_present\n"
				 "present_capture: production_SDR_tonemap_with_dithering\n"
				 "tonemap: %s\nagx_look: punchy\nagx_eotf_exponent: %.9g\n"
				 "agx_punchy_power: %.9g\nagx_punchy_saturation: %.9g\naces_pre_exposure: %.9g\n",
				 c->bloom_intensity, c->bloom_kernel == AERON_SCENE_BLOOM_KERNEL_1_TAP ? 1 : 4,
				 c->tonemap == AERON_SCENE_TONEMAP_ACES ? "aces" : "agx", AeronScenePresent_EotfExponent(),
				 AeronScenePresent_AgxPunchyPower(), AeronScenePresent_AgxPunchySaturation(),
				 AeronScenePresent_AcesExposure());
		ok = SDL_WriteIO(stream, text, strlen(text)) == strlen(text);
	}
	if (ok) {
		snprintf(text, sizeof text, "run_mode: %s\n", c->benchmark_mode ? "benchmark" : "capture");
		ok = SDL_WriteIO(stream, text, strlen(text)) == strlen(text);
	}
	if (ok && c->benchmark_mode) {
		snprintf(text, sizeof text,
				 "benchmark:\n  warmup_seconds: %.9g\n  sample_seconds: %.9g\n  samples: %d\n"
				 "  min_frames: %d\n  frames_in_flight: %d\n",
				 c->benchmark.warmup_seconds, c->benchmark.sample_seconds, c->benchmark.samples,
				 c->benchmark.min_frames, c->benchmark.frames_in_flight);
		ok = SDL_WriteIO(stream, text, strlen(text)) == strlen(text);
	}
	if (ok && c->model_count) {
		const char* lighting = "clear_color: [0, 0, 0, 1]\nlighting: directional_ambient\n"
							   "light_direction: [0.32444284, 0.48666426, 0.81110711]\n"
							   "light_color: [0.9, 0.9, 0.9]\nambient: [0.08, 0.08, 0.08]\n"
							   "smooth_angle_degrees: 90\n";
		ok                   = SDL_WriteIO(stream, lighting, strlen(lighting)) == strlen(lighting) &&
							   write_yaml_path(stream, "asset_root: ", c->asset_root) &&
							   SDL_WriteIO(stream, "models:\n", 8) == 8;
		for (int i = 0; ok && i < c->model_count; ++i) {
			/* Sequence entries use YAML's quoted scalar form, including paths with apostrophes. */
			ok = write_yaml_path(stream, "  - ", c->models[i]);
		}
	}
	if (!SDL_CloseIO(stream))
		ok = 0;
	return ok;
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
