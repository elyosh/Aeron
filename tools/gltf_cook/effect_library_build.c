/*
 * Building an effect library from its file, <library>.yaml (animated_materials.md):
 * `version`, `library` and `images` (aeron_gltf_cook --library).
 *
 * Image paths are relative to the file. Per kind, the inputs, named after
 * the material channel they change, and what is stored (colour sRGB, alpha
 * linear; products in linear light):
 *   baseColor  baseColor (RGBA, alpha coverage)      layer: as given
 *   emissive   emissive (RGB); optional baseColor (RGBA, alpha coverage c)
 *              layer: RGB baseColor·c, A 1 − c; emissive: RGB
 *   damage     factor (RGB factor, A gloss)          layer: as given
 *   mark       baseColor (RGBA)                      layer: RGB rgb·a, A a
 *   energy     emissive (RGBA, A intensity)          emissive: RGB rgb·a
 * `maxSize` caps an image's longer side, `crop` records the part a blast
 * mark's [0, 1] maps to.
 *
 * The index records a hash of the inputs (EFFECT_LIBRARY_BAKE_VERSION, the
 * file and its images); a library whose index in the output directory
 * records the same is left as it is.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "cJSON.h"
#include "effect_library.h"
#include "png_read.h"

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

/* The path of input `name`, resolved next to the library file. */
static void input_path(const char* dir, const char* name, char* path, size_t cap) {
	if (name[0] == '/')
		snprintf(path, cap, "%s", name);
	else
		snprintf(path, cap, "%s/%s", dir, name);
}

/* An input PNG of image `key`, resolved next to the library file; NULL
 * after a message when missing or of another size than `*w` × `*h` (0: any). */
static uint8_t* load_input(const char* tool, const char* dir, const char* key, const char* field,
						   const cJSON* entry, int* w, int* h, bool required, bool* ok) {
	const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, field);
	if (!name) {
		if (required) {
			fprintf(stderr, "%s: image %s: no %s\n", tool, key, field);
			*ok = false;
		}
		return NULL;
	}
	if (!cJSON_IsString(name)) {
		fprintf(stderr, "%s: image %s: %s is not a path\n", tool, key, field);
		*ok = false;
		return NULL;
	}
	char path[1024], err[512];
	input_path(dir, name->valuestring, path, sizeof path);
	uint8_t* rgba = NULL;
	int      iw = 0, ih = 0;
	if (!read_png_rgba(path, &rgba, &iw, &ih, err, sizeof err)) {
		fprintf(stderr, "%s: image %s: %s\n", tool, key, err);
		*ok = false;
		return NULL;
	}
	if (*w && (iw != *w || ih != *h)) {
		fprintf(stderr, "%s: image %s: %s is %dx%d, not %dx%d like its other input\n", tool, key, field, iw,
				ih, *w, *h);
		free(rgba);
		*ok = false;
		return NULL;
	}
	*w = iw;
	*h = ih;
	return rgba;
}

/* `maxSize` and `crop` of a checked image entry into `out` (crop owned by
 * the caller). */
static bool image_options(const cJSON* entry, EffectLibraryImage* out) {
	const cJSON* max_size = cJSON_GetObjectItemCaseSensitive(entry, "maxSize");
	if (max_size)
		out->max_size = max_size->valueint;
	const cJSON* crop = cJSON_GetObjectItemCaseSensitive(entry, "crop");
	if (crop) {
		float* values = (float*)malloc(4 * sizeof *values);
		if (!values)
			return false;
		for (int i = 0; i < 4; ++i)
			values[i] = (float)cJSON_GetArrayItem(crop, i)->valuedouble;
		out->crop = values;
	}
	return true;
}

/* ---- Inputs hash ----------------------------------------------------------- */

static bool hash_file(XXH3_state_t* state, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t buf[1 << 16];
	size_t  n;
	bool    ok = true;
	while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
		ok = XXH3_64bits_update(state, buf, n) == XXH_OK;
	ok = ok && !ferror(f);
	fclose(f);
	return ok;
}

/* The inputs hash of the library (EFFECT_LIBRARY_BAKE_VERSION, the library
 * file and every image input it names), as 16 hex digits; false when
 * unknown. */
static bool library_inputs(const char* file, const char* dir, const cJSON* images, char out[17]) {
	static const char* const fields[] = { "baseColor", "emissive", "factor" };
	static const uint32_t    version  = EFFECT_LIBRARY_BAKE_VERSION;
	char                     path[4096];
	XXH3_state_t*            s = XXH3_createState();
	bool         ok = s && XXH3_64bits_reset(s) == XXH_OK &&
					  XXH3_64bits_update(s, &version, sizeof version) == XXH_OK && hash_file(s, file);
	const cJSON* entry;
	cJSON_ArrayForEach(entry, images) {
		for (size_t i = 0; ok && i < sizeof fields / sizeof *fields; ++i) {
			const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, fields[i]);
			if (!cJSON_IsString(name))
				continue;
			input_path(dir, name->valuestring, path, sizeof path);
			ok = hash_file(s, path);
		}
	}
	if (ok)
		snprintf(out, 17, "%016llx", (unsigned long long)XXH3_64bits_digest(s));
	XXH3_freeState(s);
	return ok;
}

/* Whether library `name` in `out_dir` records `inputs` and has its pages. */
static bool library_current(const char* out_dir, const char* name, const char* inputs) {
	char path[1024], line[512];
	snprintf(path, sizeof path, "%s/%s.yaml", out_dir, name);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	bool same     = false;
	int  pages[2] = { -1, -1 };
	while (fgets(line, sizeof line, f) && strncmp(line, "images:", 7) != 0) {
		char value[64];
		if (sscanf(line, "inputs: %63s", value) == 1)
			same = strcmp(value, inputs) == 0;
		sscanf(line, "layer_pages: %d", &pages[0]);
		sscanf(line, "emissive_pages: %d", &pages[1]);
	}
	fclose(f);
	static const char* const suffix[2] = { "layer", "emissive" };
	for (int a = 0; same && a < 2; ++a) {
		struct stat st;
		snprintf(path, sizeof path, "%s/%s.%s.ktx2", out_dir, name, suffix[a]);
		same = pages[a] >= 0 && (!pages[a] || stat(path, &st) == 0);
	}
	return same;
}

/* Bakes one checked image entry into `out` (layer/emissive owned by the
 * caller). */
static bool bake(const char* tool, const char* dir, const char* key, const cJSON* entry,
				 EffectLibraryImage* out) {
	const EffectLibraryKind kind = EffectLibrary_ImageKind(entry);
	bool                    ok   = true;
	int                     w = 0, h = 0;
	if (!image_options(entry, out))
		return false;
	if (kind == EFFECT_LIBRARY_EMISSIVE) {
		uint8_t* emissive = load_input(tool, dir, key, "emissive", entry, &w, &h, true, &ok);
		uint8_t* colour   = ok ? load_input(tool, dir, key, "baseColor", entry, &w, &h, false, &ok) : NULL;
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
		/* The input names the material channel the image changes. */
		uint8_t* image =
			load_input(tool, dir, key, EffectLibrary_ImageInput(kind, 0), entry, &w, &h, true, &ok);
		if (!ok)
			return false;
		for (size_t i = 0;
			 (kind == EFFECT_LIBRARY_MARK || kind == EFFECT_LIBRARY_ENERGY) && i < (size_t)w * h; ++i) {
			uint8_t*    p = &image[i * 4];
			const float a = p[3] / 255.0f;
			for (int k = 0; k < 3; ++k)
				p[k] = to_srgb8(to_linear(p[k]) * a);
			if (kind == EFFECT_LIBRARY_ENERGY)
				p[3] = 255;
		}
		if (kind == EFFECT_LIBRARY_ENERGY)
			out->emissive = image;
		else
			out->layer = image;
	}
	out->key    = key;
	out->width  = w;
	out->height = h;
	return true;
}

EffectLibraryBuild EffectLibrary_BuildLibrary(const char* tool, const char* file, const char* name,
											  const char* out_dir) {
	cJSON* root = EffectLibrary_ReadLibrary(tool, file);
	if (!root)
		return EFFECT_LIBRARY_FAILED;
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(root, "library");
	const cJSON* images  = cJSON_GetObjectItemCaseSensitive(root, "images");
	if (name && strcmp(library->valuestring, name) != 0) {
		fprintf(stderr, "%s: %s: holds library %s, not %s\n", tool, file, library->valuestring, name);
		cJSON_Delete(root);
		return EFFECT_LIBRARY_FAILED;
	}
	char        dir[1024];
	const char* slash = strrchr(file, '/');
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - file) : 1, slash ? file : ".");
	char       inputs[17];
	const bool have_inputs = library_inputs(file, dir, images, inputs);
	if (have_inputs && library_current(out_dir, library->valuestring, inputs)) {
		fprintf(stderr, "%s: %s: unchanged\n", tool, library->valuestring);
		cJSON_Delete(root);
		return EFFECT_LIBRARY_UNCHANGED;
	}
	const int           count = cJSON_GetArraySize(images);
	EffectLibraryImage* baked = (EffectLibraryImage*)calloc((size_t)count, sizeof *baked);
	bool                ok    = baked != NULL;
	int                 n     = 0;
	const cJSON*        entry;
	if (ok) {
		cJSON_ArrayForEach(entry, images) {
			if (bake(tool, dir, entry->string, entry, &baked[n]))
				n++;
			else
				ok = false;
		}
	}
	EffectLibraryResult result = { 0 };
	if (ok && EffectLibrary_Write(tool, out_dir, library->valuestring, NULL, have_inputs ? inputs : NULL,
								  baked, (size_t)n, &result))
		fprintf(stderr, "%s: %s: built, %d image(s), %d layer and %d emissive page(s)\n", tool,
				library->valuestring, n, result.layer_pages, result.emissive_pages);
	else
		ok = false;
	for (int i = 0; baked && i < count; ++i) {
		free((void*)baked[i].layer);
		free((void*)baked[i].emissive);
		free((void*)baked[i].crop);
	}
	free(baked);
	cJSON_Delete(root);
	return ok ? EFFECT_LIBRARY_BUILT : EFFECT_LIBRARY_FAILED;
}
