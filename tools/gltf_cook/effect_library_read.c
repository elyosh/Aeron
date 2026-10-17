/*
 * Reading an effect library file, <library>.yaml (animated_materials.md,
 * "Effect libraries"): aeron_gltf_cook --library and --library-json.
 */
#include "effect_library.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "yaml_json.h"

static const char* const kKinds[EFFECT_LIBRARY_KIND_COUNT] = { "baseColor", "emissive", "damage", "mark",
															   "energy" };

/* Per kind, its required input and its optional one, named after the
 * material channel they change. */
static const char* const kInputs[EFFECT_LIBRARY_KIND_COUNT][2] = {
	{ "baseColor", NULL }, { "emissive", "baseColor" }, { "factor", NULL },
	{ "baseColor", NULL }, { "emissive", NULL },
};

EffectLibraryKind EffectLibrary_ImageKind(const cJSON* image) {
	const cJSON* kind = cJSON_GetObjectItemCaseSensitive(image, "kind");
	for (int k = 0; k < EFFECT_LIBRARY_KIND_COUNT && cJSON_IsString(kind); ++k)
		if (strcmp(kind->valuestring, kKinds[k]) == 0)
			return (EffectLibraryKind)k;
	return EFFECT_LIBRARY_KIND_COUNT;
}

const char* EffectLibrary_KindName(EffectLibraryKind kind) { return kKinds[kind]; }

const char* EffectLibrary_ImageInput(EffectLibraryKind kind, int optional) {
	return kInputs[kind][optional != 0];
}

/* Library names are file names in the mod folder's effects folder: lower-case letters,
 * digits, '_' and '-'. */
static bool valid_name(const char* s) {
	if (!*s)
		return false;
	for (; *s; ++s)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return false;
	return true;
}

/* Checks image `key`: its kind, its inputs' presence, maxSize and crop. */
static bool check_image(const char* tool, const char* key, const cJSON* entry) {
	const EffectLibraryKind kind = EffectLibrary_ImageKind(entry);
	if (kind == EFFECT_LIBRARY_KIND_COUNT) {
		fprintf(stderr, "%s: image %s: kind must be baseColor, emissive, damage, mark or energy\n", tool,
				key);
		return false;
	}
	bool ok = true;
	for (int optional = 0; optional < 2; ++optional) {
		const char*  field = kInputs[kind][optional];
		const cJSON* name  = field ? cJSON_GetObjectItemCaseSensitive(entry, field) : NULL;
		if (field && !name && !optional) {
			fprintf(stderr, "%s: image %s: no %s\n", tool, key, field);
			ok = false;
		} else if (name && !cJSON_IsString(name)) {
			fprintf(stderr, "%s: image %s: %s is not a path\n", tool, key, field);
			ok = false;
		}
	}
	const cJSON* max_size = cJSON_GetObjectItemCaseSensitive(entry, "maxSize");
	if (max_size && (!cJSON_IsNumber(max_size) || max_size->valueint < 1 ||
					 max_size->valueint > EFFECT_LIBRARY_IMAGE_MAX)) {
		fprintf(stderr, "%s: image %s: maxSize must be 1 to %d\n", tool, key, EFFECT_LIBRARY_IMAGE_MAX);
		ok = false;
	}
	const cJSON* crop = cJSON_GetObjectItemCaseSensitive(entry, "crop");
	if (crop) {
		bool valid = cJSON_IsArray(crop) && cJSON_GetArraySize(crop) == 4;
		for (int i = 0; valid && i < 4; ++i)
			valid = cJSON_IsNumber(cJSON_GetArrayItem(crop, i));
		if (!valid) {
			fprintf(stderr, "%s: image %s: crop must be [x0, y0, x1, y1]\n", tool, key);
			ok = false;
		}
	}
	return ok;
}

bool EffectLibrary_CheckLibrary(const char* tool, const char* file, const cJSON* root) {
	const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(root, "library");
	const cJSON* images  = cJSON_GetObjectItemCaseSensitive(root, "images");
	const bool   object  = cJSON_IsObject(root);
	bool         ok      = object;
	/* A library file holds only the library; a model's animations are its
	   own file, which aeron_gltf_cook reads. */
	const cJSON* member;
	cJSON_ArrayForEach(member, root) {
		if (object && strcmp(member->string, "version") && strcmp(member->string, "library") &&
			strcmp(member->string, "images")) {
			fprintf(stderr, "%s: %s: unknown member %s (a library has version, library and images)\n", tool,
					file, member->string);
			ok = false;
		}
	}
	if (!cJSON_IsNumber(version) || version->valueint != 1) {
		fprintf(stderr, "%s: %s: version must be 1\n", tool, file);
		ok = false;
	}
	if (!cJSON_IsString(library) || !valid_name(library->valuestring)) {
		fprintf(stderr, "%s: %s: library must be a name of a-z, 0-9, '_' and '-'\n", tool, file);
		ok = false;
	}
	if (!cJSON_IsObject(images) || !cJSON_GetArraySize(images)) {
		fprintf(stderr, "%s: %s: no images\n", tool, file);
		ok = false;
	}
	const cJSON* entry;
	cJSON_ArrayForEach(entry, images) {
		if (cJSON_IsObject(images) && !check_image(tool, entry->string, entry))
			ok = false;
	}
	return ok;
}

cJSON* EffectLibrary_ReadLibrary(const char* tool, const char* file) {
	char   err[512];
	cJSON* root = YamlJson_Load(file, err, sizeof err);
	if (!root) {
		fprintf(stderr, "%s: %s\n", tool, err);
		return NULL;
	}
	if (!EffectLibrary_CheckLibrary(tool, file, root)) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}
