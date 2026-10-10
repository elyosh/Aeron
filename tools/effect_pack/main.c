/*
 * effect_pack — builds an effect image library from the `images` of an
 * animation file (README.md).
 *
 *   effect_pack --out DIR FILE.anim.yaml
 *
 * Image paths are relative to the animation file. Per kind, the inputs and
 * what is stored (colour sRGB, alpha linear; products in linear light):
 *   colour        image (RGBA, alpha coverage)   layer: as given
 *   illumination  emissive (RGB); optional colour (RGBA, alpha coverage c)
 *                 layer: RGB colour·c, A 1 − c; emissive: RGB
 *   multiply      image (RGB factor, A gloss)    layer: as given
 *   mark          image (RGBA)                   layer: RGB rgb·a, A a
 *   energy        image (RGBA)                   emissive: RGB rgb·a
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "effect_pack.h"
#include "png_read.h"
#include "yaml_json.h"

typedef enum Kind { KIND_COLOUR, KIND_ILLUMINATION, KIND_MULTIPLY, KIND_MARK, KIND_ENERGY, KIND_COUNT } Kind;

static const char* const kKinds[KIND_COUNT] = { "colour", "illumination", "multiply", "mark", "energy" };

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float to_linear(uint8_t v) {
	const float c = v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t to_srgb8(float c) {
	c             = clamp01(c);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

/* An input PNG of image `key`, resolved next to the animation file; NULL
 * after a message when missing or of another size than `*w` × `*h` (0: any). */
static uint8_t* load_input(const char* dir, const char* key, const char* field, const cJSON* entry, int* w,
						   int* h, bool required, bool* ok) {
	const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, field);
	if (!name) {
		if (required) {
			fprintf(stderr, "effect_pack: image %s: no %s\n", key, field);
			*ok = false;
		}
		return NULL;
	}
	if (!cJSON_IsString(name)) {
		fprintf(stderr, "effect_pack: image %s: %s is not a path\n", key, field);
		*ok = false;
		return NULL;
	}
	char path[1024], err[512];
	if (name->valuestring[0] == '/')
		snprintf(path, sizeof path, "%s", name->valuestring);
	else
		snprintf(path, sizeof path, "%s/%s", dir, name->valuestring);
	uint8_t* rgba = NULL;
	int      iw = 0, ih = 0;
	if (!read_png_rgba(path, &rgba, &iw, &ih, err, sizeof err)) {
		fprintf(stderr, "effect_pack: image %s: %s\n", key, err);
		*ok = false;
		return NULL;
	}
	if (*w && (iw != *w || ih != *h)) {
		fprintf(stderr, "effect_pack: image %s: %s is %dx%d, not %dx%d like its other input\n", key, field,
				iw, ih, *w, *h);
		free(rgba);
		*ok = false;
		return NULL;
	}
	*w = iw;
	*h = ih;
	return rgba;
}

/* Bakes one image entry into `out` (layer/emissive owned by the caller). */
static bool bake(const char* dir, const char* key, const cJSON* entry, EffectPackImage* out) {
	const cJSON* kind_item = cJSON_GetObjectItemCaseSensitive(entry, "kind");
	int          kind      = -1;
	for (int k = 0; k < KIND_COUNT && cJSON_IsString(kind_item); ++k)
		if (strcmp(kind_item->valuestring, kKinds[k]) == 0)
			kind = k;
	if (kind < 0) {
		fprintf(stderr,
				"effect_pack: image %s: kind must be colour, illumination, multiply, mark or energy\n", key);
		return false;
	}
	bool ok = true;
	int  w = 0, h = 0;
	if (kind == KIND_ILLUMINATION) {
		uint8_t* emissive = load_input(dir, key, "emissive", entry, &w, &h, true, &ok);
		uint8_t* colour   = ok ? load_input(dir, key, "colour", entry, &w, &h, false, &ok) : NULL;
		uint8_t* layer    = ok ? (uint8_t*)malloc((size_t)w * h * 4) : NULL;
		if (!ok || !layer) {
			free(emissive);
			free(colour);
			free(layer);
			return false;
		}
		for (size_t i = 0; i < (size_t)w * h; ++i) {
			const float c = colour ? colour[i * 4 + 3] / 255.0f : 0.0f;
			for (int k = 0; k < 3; ++k)
				layer[i * 4 + k] = colour ? to_srgb8(to_linear(colour[i * 4 + k]) * c) : 0;
			layer[i * 4 + 3]    = (uint8_t)((1.0f - c) * 255.0f + 0.5f);
			emissive[i * 4 + 3] = 255;
		}
		free(colour);
		out->layer    = layer;
		out->emissive = emissive;
	} else {
		uint8_t* image = load_input(dir, key, "image", entry, &w, &h, true, &ok);
		if (!ok)
			return false;
		for (size_t i = 0; (kind == KIND_MARK || kind == KIND_ENERGY) && i < (size_t)w * h; ++i) {
			uint8_t*    p = &image[i * 4];
			const float a = p[3] / 255.0f;
			for (int k = 0; k < 3; ++k)
				p[k] = to_srgb8(to_linear(p[k]) * a);
			if (kind == KIND_ENERGY)
				p[3] = 255;
		}
		if (kind == KIND_ENERGY)
			out->emissive = image;
		else
			out->layer = image;
	}
	out->key    = key;
	out->width  = w;
	out->height = h;
	return true;
}

/* Library names are file names under remaster/effects: lower-case letters,
 * digits, '_' and '-'. */
static bool valid_name(const char* s) {
	if (!*s)
		return false;
	for (; *s; ++s)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return false;
	return true;
}

static int usage(void) {
	fprintf(stderr, "usage: effect_pack --out DIR FILE.anim.yaml\n");
	return 2;
}

int main(int argc, char** argv) {
	const char* out  = NULL;
	const char* file = NULL;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			out = argv[++i];
		else if (argv[i][0] == '-' || file)
			return usage();
		else
			file = argv[i];
	}
	if (!out || !file)
		return usage();
	char   err[512];
	cJSON* root = YamlJson_Load(file, err, sizeof err);
	if (!root) {
		fprintf(stderr, "effect_pack: %s\n", err);
		return 1;
	}
	const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(root, "library");
	const cJSON* images  = cJSON_GetObjectItemCaseSensitive(root, "images");
	bool         ok      = true;
	if (!cJSON_IsNumber(version) || version->valueint != 1) {
		fprintf(stderr, "effect_pack: %s: version must be 1\n", file);
		ok = false;
	}
	if (!cJSON_IsString(library) || !valid_name(library->valuestring)) {
		fprintf(stderr, "effect_pack: %s: library must be a name of a-z, 0-9, '_' and '-'\n", file);
		ok = false;
	}
	if (!cJSON_IsObject(images) || !cJSON_GetArraySize(images)) {
		fprintf(stderr, "effect_pack: %s: no images\n", file);
		ok = false;
	}
	char        dir[1024];
	const char* slash = strrchr(file, '/');
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - file) : 1, slash ? file : ".");
	const int        count = ok ? cJSON_GetArraySize(images) : 0;
	EffectPackImage* baked = (EffectPackImage*)calloc(count ? (size_t)count : 1, sizeof *baked);
	ok                     = ok && baked;
	int          n         = 0;
	const cJSON* entry;
	if (ok) {
		cJSON_ArrayForEach(entry, images) {
			if (bake(dir, entry->string, entry, &baked[n]))
				n++;
			else
				ok = false;
		}
	}
	EffectPackResult result = { 0 };
	if (ok && EffectPack_Write("effect_pack", out, library->valuestring, NULL, NULL, baked, (size_t)n, &result))
		fprintf(stderr, "effect_pack: %s: %d image(s), %d layer and %d emissive page(s)\n",
				library->valuestring, n, result.layer_pages, result.emissive_pages);
	else
		ok = false;
	for (int i = 0; i < n; ++i) {
		free((void*)baked[i].layer);
		free((void*)baked[i].emissive);
	}
	free(baked);
	cJSON_Delete(root);
	return ok ? 0 : 1;
}
