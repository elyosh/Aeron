#include "xwau_skins.h"

#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include "stb_image.h"

static char* read_text(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	char* text = NULL;
	long  size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (size >= 0 && fseek(f, 0, SEEK_SET) == 0 && (text = (char*)malloc((size_t)size + 1)) != NULL) {
		if (fread(text, 1, (size_t)size, f) != (size_t)size) {
			free(text);
			text = NULL;
		} else {
			text[size] = '\0';
		}
	}
	fclose(f);
	return text;
}

static char* dup_string(const char* s) {
	const size_t n    = strlen(s) + 1;
	char*        copy = (char*)malloc(n);
	if (copy)
		memcpy(copy, s, n);
	return copy;
}

/* Index of `name` in the skin list, added when new; -1 when out of memory. */
static int skin_index(XwauSkins* s, const char* name) {
	for (size_t i = 0; i < s->skins.skin_count; ++i)
		if (strcmp(s->skin_names[i], name) == 0)
			return (int)i;
	char** grown = (char**)realloc(s->skin_names, (s->skins.skin_count + 1) * sizeof *grown);
	if (!grown)
		return -1;
	s->skin_names = grown;
	if (!(grown[s->skins.skin_count] = dup_string(name)))
		return -1;
	return (int)s->skins.skin_count++;
}

/* The skin's image for texture `t`: NULL rgba when it has none, or when its
 * size differs (reported). */
static bool load_image(const char* skins_dir, const char* skin, const opt_texture_t* t, const char* label,
					   size_t* skipped, OptGltfImageView* out) {
	static const char* const extensions[] = { ".bmp", ".png", ".jpg" };
	char                     name[256];
	size_t                   n = 0;
	for (const char* c = t->name; *c && n + 1 < sizeof name; ++c)
		name[n++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
	name[n] = '\0';
	memset(out, 0, sizeof *out);
	if (!name[0])
		return true;
	for (size_t e = 0; e < sizeof extensions / sizeof extensions[0]; ++e) {
		char  path[1024];
		FILE* probe;
		snprintf(path, sizeof path, "%s/%s/%s%s", skins_dir, skin, name, extensions[e]);
		if (!(probe = fopen(path, "rb")))
			continue;
		fclose(probe);
		int      w = 0, h = 0, channels = 0;
		uint8_t* rgba = stbi_load(path, &w, &h, &channels, 4);
		if (!rgba) {
			fprintf(stderr, "opt2gltf: %s: skin image %s: %s\n", label, path, stbi_failure_reason());
			return false;
		}
		if (w != t->width || h != t->height) {
			fprintf(stderr, "opt2gltf: %s: skin %s image %s%s is %dx%d, texture %dx%d; skipped\n", label,
					skin, name, extensions[e], w, h, (int)t->width, (int)t->height);
			stbi_image_free(rgba);
			++*skipped;
			return true;
		}
		/* OPT row order: the hook flips the loaded image (FlipPixels). */
		uint8_t*     flipped = (uint8_t*)malloc((size_t)w * h * 4);
		const size_t stride  = (size_t)w * 4;
		if (!flipped) {
			stbi_image_free(rgba);
			return false;
		}
		for (int y = 0; y < h; ++y)
			memcpy(flipped + (size_t)y * stride, rgba + (size_t)(h - 1 - y) * stride, stride);
		stbi_image_free(rgba);
		*out = (OptGltfImageView) { flipped, (uint32_t)w, (uint32_t)h };
		return true;
	}
	return true;
}

static bool parse(const cJSON* root, XwauSkins* s, const char* label) {
	const cJSON* variants = cJSON_GetObjectItemCaseSensitive(root, "variants");
	const cJSON* markings = cJSON_GetObjectItemCaseSensitive(root, "default_markings");
	if (!cJSON_IsArray(variants) || !cJSON_IsArray(markings)) {
		fprintf(stderr, "opt2gltf: %s: skin variants need 'variants' and 'default_markings' arrays\n", label);
		return false;
	}
	const size_t variant_count = (size_t)cJSON_GetArraySize(variants);
	size_t       layer_total   = 0;
	const cJSON* variant;
	cJSON_ArrayForEach(variant, variants) {
		layer_total += (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(variant, "layers"));
	}
	s->variants      = (OptGltfSkinVariant*)calloc(variant_count ? variant_count : 1, sizeof *s->variants);
	s->variant_names = (char**)calloc(variant_count ? variant_count : 1, sizeof *s->variant_names);
	s->layers        = (OptGltfSkinLayer*)calloc(layer_total ? layer_total : 1, sizeof *s->layers);
	if (!s->variants || !s->variant_names || !s->layers)
		return false;
	s->skins.variants      = s->variants;
	s->skins.variant_count = variant_count;
	size_t v = 0, next_layer = 0;
	cJSON_ArrayForEach(variant, variants) {
		const cJSON* name   = cJSON_GetObjectItemCaseSensitive(variant, "name");
		const cJSON* state  = cJSON_GetObjectItemCaseSensitive(variant, "state");
		const cJSON* layers = cJSON_GetObjectItemCaseSensitive(variant, "layers");
		if (!cJSON_IsString(name) || !cJSON_IsNumber(state) || state->valuedouble < 0 ||
			!cJSON_IsArray(layers)) {
			fprintf(stderr, "opt2gltf: %s: skin variant %zu needs 'name', 'state' and 'layers'\n", label, v);
			return false;
		}
		OptGltfSkinVariant* out = &s->variants[v];
		if (!(s->variant_names[v] = dup_string(name->valuestring)))
			return false;
		out->name   = s->variant_names[v];
		out->state  = (uint32_t)state->valueint;
		out->layers = &s->layers[next_layer];
		const cJSON* layer;
		cJSON_ArrayForEach(layer, layers) {
			const cJSON* skin    = cJSON_GetObjectItemCaseSensitive(layer, "skin");
			const cJSON* opacity = cJSON_GetObjectItemCaseSensitive(layer, "opacity");
			if (!cJSON_IsString(skin) || !cJSON_IsNumber(opacity) || opacity->valueint < 0 ||
				opacity->valueint > 100) {
				fprintf(stderr, "opt2gltf: %s: skin variant '%s' has a bad layer\n", label, out->name);
				return false;
			}
			const int index = skin_index(s, skin->valuestring);
			if (index < 0)
				return false;
			s->layers[next_layer++] = (OptGltfSkinLayer) { (uint32_t)index, (uint32_t)opacity->valueint };
			out->layer_count++;
		}
		++v;
	}
	const size_t markings_count = (size_t)cJSON_GetArraySize(markings);
	s->markings                 = (char**)calloc(markings_count ? markings_count : 1, sizeof *s->markings);
	if (!s->markings)
		return false;
	s->skins.default_markings = (const char* const*)s->markings;
	s->skins.markings_count   = markings_count;
	size_t       m            = 0;
	const cJSON* entry;
	cJSON_ArrayForEach(entry, markings) {
		if (cJSON_IsString(entry)) {
			if (!(s->markings[m] = dup_string(entry->valuestring)))
				return false;
		} else if (!cJSON_IsNull(entry)) {
			fprintf(stderr, "opt2gltf: %s: default_markings %zu is neither a name nor null\n", label, m);
			return false;
		}
		++m;
	}
	return true;
}

bool XwauSkins_Load(const char* variants_path, const char* skins_dir, const opt_file_t* opt,
					const char* label, XwauSkins* out) {
	memset(out, 0, sizeof *out);
	char* text = read_text(variants_path);
	if (!text) {
		fprintf(stderr, "opt2gltf: %s: cannot read %s\n", label, variants_path);
		return false;
	}
	cJSON* root = cJSON_Parse(text);
	free(text);
	if (!root) {
		fprintf(stderr, "opt2gltf: %s: %s is not JSON\n", label, variants_path);
		return false;
	}
	bool ok = parse(root, out, label);
	cJSON_Delete(root);

	const size_t texture_count = (size_t)opt->texture_count;
	size_t       found = 0, skipped = 0;
	if (ok) {
		const size_t count = out->skins.skin_count * texture_count;
		out->images        = (OptGltfImageView*)calloc(count ? count : 1, sizeof *out->images);
		out->image_count   = out->images ? count : 0;
		ok                 = out->images != NULL;
	}
	for (size_t s = 0; ok && s < out->skins.skin_count; ++s) {
		for (size_t t = 0; ok && t < texture_count; ++t) {
			OptGltfImageView* view = &out->images[s * texture_count + t];
			ok = load_image(skins_dir, out->skin_names[s], &opt->textures[t], label, &skipped, view);
			found += view->rgba != NULL;
		}
	}
	if (!ok) {
		XwauSkins_Free(out);
		return false;
	}
	out->skins.images = out->images;
	fprintf(stderr, "opt2gltf: %s: %zu skins, %zu skin images, %zu skipped\n", label, out->skins.skin_count,
			found, skipped);
	return true;
}

void XwauSkins_Free(XwauSkins* s) {
	for (size_t i = 0; i < s->image_count; ++i)
		free((void*)s->images[i].rgba);
	free(s->images);
	for (size_t i = 0; s->skin_names && i < s->skins.skin_count; ++i)
		free(s->skin_names[i]);
	free(s->skin_names);
	for (size_t i = 0; s->variant_names && i < s->skins.variant_count; ++i)
		free(s->variant_names[i]);
	free(s->variant_names);
	free(s->variants);
	free(s->layers);
	for (size_t i = 0; s->markings && i < s->skins.markings_count; ++i)
		free(s->markings[i]);
	free(s->markings);
	memset(s, 0, sizeof *s);
}
