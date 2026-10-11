/*
 * effect_pack — builds an effect image library from the `images` of an
 * animation file (README.md).
 *
 *   effect_pack --out DIR FILE.anim.yaml
 *
 * Image paths are relative to the animation file. Per kind, the inputs, named
 * after the material channel they change, and what is stored (colour sRGB,
 * alpha linear; products in linear light):
 *   colour        baseColor (RGBA, alpha coverage)   layer: as given
 *   illumination  emissive (RGB); optional baseColor (RGBA, alpha coverage c)
 *                 layer: RGB baseColor·c, A 1 − c; emissive: RGB
 *   multiply      factor (RGB factor, A gloss)       layer: as given
 *   mark          baseColor (RGBA)                   layer: RGB rgb·a, A a
 *   energy        emissive (RGBA, A intensity)       emissive: RGB rgb·a
 * `maxSize` caps an image's longer side, `crop` records the part a blast
 * mark's [0, 1] maps to.
 *
 * The index records a hash of the inputs (this executable, the file and
 * its images); a library whose index in DIR records the same is left as
 * it is.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#define XXH_INLINE_ALL
#include "xxhash.h"

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

/* The path of input `name`, resolved next to the animation file. */
static void input_path(const char* dir, const char* name, char* path, size_t cap) {
	if (name[0] == '/')
		snprintf(path, cap, "%s", name);
	else
		snprintf(path, cap, "%s/%s", dir, name);
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
	input_path(dir, name->valuestring, path, sizeof path);
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

/* `maxSize` and `crop` of an image entry into `out` (crop owned by the
 * caller). */
static bool image_options(const char* key, const cJSON* entry, EffectPackImage* out) {
	const cJSON* max_size = cJSON_GetObjectItemCaseSensitive(entry, "maxSize");
	if (max_size) {
		if (!cJSON_IsNumber(max_size) || max_size->valueint < 1 || max_size->valueint > EFFECT_PACK_IMAGE_MAX) {
			fprintf(stderr, "effect_pack: image %s: maxSize must be 1 to %d\n", key, EFFECT_PACK_IMAGE_MAX);
			return false;
		}
		out->max_size = max_size->valueint;
	}
	const cJSON* crop = cJSON_GetObjectItemCaseSensitive(entry, "crop");
	if (crop) {
		float* values = (float*)malloc(4 * sizeof *values);
		bool   ok     = values && cJSON_IsArray(crop) && cJSON_GetArraySize(crop) == 4;
		for (int i = 0; ok && i < 4; ++i) {
			const cJSON* v = cJSON_GetArrayItem(crop, i);
			ok             = cJSON_IsNumber(v);
			if (ok)
				values[i] = (float)v->valuedouble;
		}
		if (!ok) {
			free(values);
			fprintf(stderr, "effect_pack: image %s: crop must be [x0, y0, x1, y1]\n", key);
			return false;
		}
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

static bool executable_path(char* path, size_t cap) {
#if defined(__APPLE__)
	uint32_t size = (uint32_t)cap;
	return _NSGetExecutablePath(path, &size) == 0;
#elif defined(__linux__)
	const ssize_t n = readlink("/proc/self/exe", path, cap - 1);
	if (n <= 0)
		return false;
	path[n] = 0;
	return true;
#else
	(void)path;
	(void)cap;
	return false;
#endif
}

/* The inputs hash of the library (this executable, the animation file and
 * every image input it names), as 16 hex digits; false when unknown. */
static bool library_inputs(const char* file, const char* dir, const cJSON* images, char out[17]) {
	static const char* const fields[] = { "baseColor", "emissive", "factor" };
	char                     path[4096];
	XXH3_state_t*            s  = XXH3_createState();
	bool                     ok = s && XXH3_64bits_reset(s) == XXH_OK && executable_path(path, sizeof path) &&
			 hash_file(s, path) && hash_file(s, file);
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
	char  path[1024], line[512];
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
	if (!image_options(key, entry, out))
		return false;
	if (kind == KIND_ILLUMINATION) {
		uint8_t* emissive = load_input(dir, key, "emissive", entry, &w, &h, true, &ok);
		uint8_t* colour   = ok ? load_input(dir, key, "baseColor", entry, &w, &h, false, &ok) : NULL;
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
		const char* field = kind == KIND_MULTIPLY ? "factor" : kind == KIND_ENERGY ? "emissive" : "baseColor";
		uint8_t*    image = load_input(dir, key, field, entry, &w, &h, true, &ok);
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
	char       inputs[17];
	const bool have_inputs = ok && library_inputs(file, dir, images, inputs);
	if (have_inputs && library_current(out, library->valuestring, inputs)) {
		fprintf(stderr, "effect_pack: %s: unchanged\n", library->valuestring);
		cJSON_Delete(root);
		return 0;
	}
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
	if (ok && EffectPack_Write("effect_pack", out, library->valuestring, NULL, have_inputs ? inputs : NULL, baked,
							   (size_t)n, &result))
		fprintf(stderr, "effect_pack: %s: %d image(s), %d layer and %d emissive page(s)\n",
				library->valuestring, n, result.layer_pages, result.emissive_pages);
	else
		ok = false;
	for (int i = 0; i < count; ++i) {
		free((void*)baked[i].layer);
		free((void*)baked[i].emissive);
		free((void*)baked[i].crop);
	}
	free(baked);
	cJSON_Delete(root);
	return ok ? 0 : 1;
}
