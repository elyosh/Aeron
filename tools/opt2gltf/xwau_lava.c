/*
 * The generated lava library.
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xwau_lava.h"

#include "cJSON.h"
#include "effect_files.h"
#include "png_write.h"
#include "util.h"

#define NOISE_SIZE 256
/* rz is kept up to LAVA_RZ_MAX. */
#define LAVA_RZ_MAX 2.0f

static const float kLavaColor[3] = { 1.0f, 0.35f, 0.05f };

/* ---- XWAU's lava ------------------------------------------------------ */

/* XWAU's gray noise texture: NOISE_SIZE² rand() / RAND_MAX values, uploaded
 * with a row pitch of NOISE_SIZE bytes. Each row then starts 64 values after
 * the previous one. rand() is MSVC's, from its default seed. */
static float g_noise[NOISE_SIZE * NOISE_SIZE];

static void make_noise(void) {
	static float raw[NOISE_SIZE * NOISE_SIZE];
	uint32_t     state = 1;
	for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; ++i) {
		state  = state * 214013u + 2531011u;
		raw[i] = (float)((state >> 16) & 0x7fff) / 32767.0f;
	}
	for (int y = 0; y < NOISE_SIZE; ++y)
		for (int x = 0; x < NOISE_SIZE; ++x)
			g_noise[y * NOISE_SIZE + x] = raw[y * (NOISE_SIZE / 4) + x];
}

/* noise(): the texture at x × 0.01, filtered bilinearly and wrapped. */
static float noise(float x, float y) {
	const float u = x * 0.01f * NOISE_SIZE - 0.5f, v = y * 0.01f * NOISE_SIZE - 0.5f;
	const float fu = floorf(u), fv = floorf(v);
	const float tu = u - fu, tv = v - fv;
	const int   x0 = (int)fu & (NOISE_SIZE - 1), y0 = (int)fv & (NOISE_SIZE - 1);
	const int   x1 = (x0 + 1) & (NOISE_SIZE - 1), y1 = (y0 + 1) & (NOISE_SIZE - 1);
	const float a = g_noise[y0 * NOISE_SIZE + x0], b = g_noise[y0 * NOISE_SIZE + x1];
	const float c = g_noise[y1 * NOISE_SIZE + x0], d = g_noise[y1 * NOISE_SIZE + x1];
	return (a + (b - a) * tu) + ((c + (d - c) * tu) - (a + (b - a) * tu)) * tv;
}

/* flow(): six octaves of noise, each displaced by the turned gradient of the
 * noise. `seconds` is XWAU's iTime. */
static float flow(float px, float py, float seconds) {
	const float time = seconds * 0.1f;
	const float st = sinf(time), ct = cosf(time);
	/* The rotation of the gradient, [[c, -s], [s, c]], set once from p. */
	const float angle = time * 6.0f - (0.05f * px + 0.03f * py) * 10.0f;
	const float c = cosf(angle), s = sinf(angle);
	const float ep = 0.09f;
	float       z = 2.0f, rz = 0.0f, bx = px, by = py;
	for (int i = 1; i < 7; ++i) {
		px += st + time * 0.3f;
		py += ct + time * 0.3f;
		bx += ct + time * 1.1f;
		by += st + time * 1.1f;
		const float gx0 = i * px * 0.34f, gy0 = i * py * 0.34f;
		const float gx = noise(gx0 + ep, gy0) - noise(gx0 - ep, gy0);
		const float gy = noise(gx0, gy0 + ep) - noise(gx0, gy0 - ep);
		px += (c * gx - s * gy) * 0.5f;
		py += (s * gx + c * gy) * 0.5f;
		rz += (sinf(noise(px, py) * 7.0f) * 0.5f + 0.5f) / z;
		px = bx + (px - bx) * 0.77f;
		py = by + (py - by) * 0.77f;
		z *= 1.8f;
		px *= 2.0f;
		py *= 2.0f;
		bx *= 1.9f;
		by *= 1.9f;
	}
	return rz;
}

/* Overlaid copies of uncorrelated noise have less variance where they mix, by
 * 4w(1 − w) at weight w. This scales it back, as XWAU's noisetile does. */
static float mix_variance(float v, float w) {
	const float f = 0.5f * (1.0f + sqrtf(2.0f));
	return v + (f * powf(fabsf(v), f) - v) * 4.0f * w * (1.0f - w);
}

/* noisetile(): flow at p in [0, repeat]², blended with its copies one repeat
 * away so that it tiles. */
static float lerp_x(float px, float py, float repeat, float seconds) {
	const float w = px / repeat;
	const float v =
		flow(px - repeat * 0.5f, py, seconds) * w + flow(px + repeat * 0.5f, py, seconds) * (1.0f - w);
	return mix_variance(v, w);
}

static float noise_tile(float px, float py, float repeat, float seconds) {
	const float w = py / repeat;
	const float v = lerp_x(px, py - repeat * 0.5f, repeat, seconds) * w +
					lerp_x(px, py + repeat * 0.5f, repeat, seconds) * (1.0f - w);
	return mix_variance(v, w);
}

/* ---- Frames ----------------------------------------------------------- */

/* The linear value of a gamma-encoded value, with the sRGB curve extended
 * above 1, as for the other colours that XWAU draws above 1. */
static float linear(float c) {
	c = c < 0.0f ? 0.0f : c;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t srgb8(float c) {
	c             = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

void XwauLava_Key(char* out, size_t cap, bool tiling, int frame) {
	snprintf(out, cap, "t%d-%02d", tiling ? 2 : 3, frame);
}

float XwauLava_Strength(void) { return linear(LAVA_RZ_MAX * kLavaColor[0]); }

/* Renders frame `frame` of a flipbook into `rgba`. */
static void render_frame(bool tiling, int frame, uint8_t* rgba) {
	const float repeat = tiling ? 2.0f : 3.0f;
	const float period = XWAU_LAVA_SECONDS;
	const float t      = period * (float)frame / XWAU_LAVA_FRAMES;
	const float w      = t / period;
	const float scale  = 1.0f / XwauLava_Strength();
	for (int y = 0; y < XWAU_LAVA_SIZE; ++y) {
		for (int x = 0; x < XWAU_LAVA_SIZE; ++x) {
			const float px = (x + 0.5f) / XWAU_LAVA_SIZE * repeat;
			const float py = (y + 0.5f) / XWAU_LAVA_SIZE * repeat;
			/* The loop: the lava one period later fades into the lava now. */
			float v = noise_tile(px, py, repeat, t + period) * (1.0f - w) + noise_tile(px, py, repeat, t) * w;
			v       = mix_variance(v, w);
			const float rz = v > 0.2f / LAVA_RZ_MAX ? 0.2f / v : LAVA_RZ_MAX;
			uint8_t*    p  = &rgba[((size_t)y * XWAU_LAVA_SIZE + x) * 4];
			for (int k = 0; k < 3; ++k)
				p[k] = srgb8(linear(kLavaColor[k] * rz) * scale);
			p[3] = 255;
		}
	}
}

/* The library file content, without images. */
static cJSON* library_doc(cJSON** images) {
	cJSON* doc = cJSON_CreateObject();
	*images    = doc && cJSON_AddNumberToObject(doc, "version", 1) &&
						 cJSON_AddStringToObject(doc, "library", XWAU_LAVA_LIBRARY)
					 ? cJSON_AddObjectToObject(doc, "images")
					 : NULL;
	return doc;
}

/* Adds image `key`, whose frame is `name`. */
static bool add_image(cJSON* images, const char* key, const char* name) {
	cJSON* entry = cJSON_AddObjectToObject(images, key);
	return entry && cJSON_AddStringToObject(entry, "kind", "emissive") &&
		   cJSON_AddStringToObject(entry, "emissive", name);
}

bool XwauLava_WriteLibrary(const char* effects_dir, bool merge) {
	char dir[1024], frames[1100], file[1100];
	if (!PathFormat(dir, sizeof dir, "%s/%s", effects_dir, XWAU_LAVA_LIBRARY) ||
		!PathFormat(frames, sizeof frames, "%s/frames", dir) ||
		!PathFormat(file, sizeof file, "%s/%s.yaml", dir, XWAU_LAVA_LIBRARY))
		return false;
	if (merge && IsFile(file))
		return true;
	if (!MakeDirs(frames)) {
		Report(NULL, "cannot create %s", frames);
		return false;
	}
	uint8_t* rgba = (uint8_t*)malloc((size_t)XWAU_LAVA_SIZE * XWAU_LAVA_SIZE * 4);
	cJSON*   images;
	cJSON*   doc = library_doc(&images);
	bool     ok  = rgba && images;
	if (!ok)
		Report(NULL, "out of memory (lava)");
	make_noise();
	for (int tiling = 1; ok && tiling >= 0; --tiling) {
		for (int frame = 0; ok && frame < XWAU_LAVA_FRAMES; ++frame) {
			char key[16], name[64], png[1200];
			XwauLava_Key(key, sizeof key, tiling, frame);
			snprintf(name, sizeof name, "frames/%s.png", key);
			render_frame(tiling, frame, rgba);
			ok = PathFormat(png, sizeof png, "%s/%s", dir, name);
			if (ok && !write_png_rgba(png, XWAU_LAVA_SIZE, XWAU_LAVA_SIZE, rgba)) {
				Report(NULL, "cannot write %s", png);
				ok = false;
			}
			ok = ok && add_image(images, key, name);
		}
	}
	ok = ok && EffectFiles_Write("opt2gltf", doc,
								 "# Effect library lava, generated from XWAU's procedural lava.\n", file);
	if (ok)
		Report(NULL, "library %s: %d image(s) written", XWAU_LAVA_LIBRARY, 2 * XWAU_LAVA_FRAMES);
	cJSON_Delete(doc);
	free(rgba);
	return ok;
}

/* The frames that one thread renders: every `step`th one from `first`. */
typedef struct Renderer {
	XwauLibraryInput* frames;
	int               first, step;
} Renderer;

static void* render_frames(void* user) {
	const Renderer* r = (const Renderer*)user;
	for (int i = r->first; i < 2 * XWAU_LAVA_FRAMES; i += r->step)
		render_frame(i < XWAU_LAVA_FRAMES, i % XWAU_LAVA_FRAMES, r->frames[i].rgba);
	return NULL;
}

bool XwauLava_MakeLibrary(int threads, cJSON** library, XwauLibraryInput** inputs, size_t* count) {
	const int         n      = 2 * XWAU_LAVA_FRAMES;
	XwauLibraryInput* frames = (XwauLibraryInput*)calloc((size_t)n, sizeof *frames);
	cJSON*            images;
	cJSON*            doc = library_doc(&images);
	bool              ok  = frames && images;
	/* The flipbook with LavaTiling on comes first, as in the written library. */
	for (int i = 0; ok && i < n; ++i) {
		char key[16];
		XwauLava_Key(key, sizeof key, i < XWAU_LAVA_FRAMES, i % XWAU_LAVA_FRAMES);
		snprintf(frames[i].name, sizeof frames[i].name, "frames/%s.png", key);
		frames[i].width  = XWAU_LAVA_SIZE;
		frames[i].height = XWAU_LAVA_SIZE;
		frames[i].rgba   = (uint8_t*)malloc((size_t)XWAU_LAVA_SIZE * XWAU_LAVA_SIZE * 4);
		ok               = frames[i].rgba && add_image(images, key, frames[i].name);
	}
	if (!ok) {
		Report(NULL, "out of memory (lava)");
		XwauLibrary_FreeInputs(frames, frames ? (size_t)n : 0);
		cJSON_Delete(doc);
		return false;
	}
	make_noise();
	if (threads < 1)
		threads = 1;
	if (threads > n)
		threads = n;
	Renderer*  renderers = (Renderer*)calloc((size_t)threads, sizeof *renderers);
	pthread_t* ids       = (pthread_t*)calloc((size_t)threads, sizeof *ids);
	if (!renderers || !ids) {
		const Renderer all = { frames, 0, 1 };
		render_frames((void*)&all);
	} else {
		for (int t = 0; t < threads; ++t)
			renderers[t] = (Renderer) { frames, t, threads };
		/* The frames of the threads that can't start are rendered here. */
		int started = 0;
		for (int t = 1; t < threads && pthread_create(&ids[t], NULL, render_frames, &renderers[t]) == 0; ++t)
			started = t;
		render_frames(&renderers[0]);
		for (int t = started + 1; t < threads; ++t)
			render_frames(&renderers[t]);
		for (int t = 1; t <= started; ++t)
			pthread_join(ids[t], NULL);
	}
	free(renderers);
	free(ids);
	Report(NULL, "library %s: %d image(s) made", XWAU_LAVA_LIBRARY, n);
	*library = doc;
	*inputs  = frames;
	*count   = (size_t)n;
	return true;
}
