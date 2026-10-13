/*
 * The material checks of aeron_gltf_cook (material_checks.h). They check
 * what the runtime loader (aeron/src/scene/gltf_mesh.c) reads, so a cooked
 * model loads with its animations and screens; unknown fields are refused
 * here, to catch typos, though loaders ignore them.
 */
#include "material_checks.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cgltf.h"
#include "effect_library.h"
#include "yaml_json.h"

#define TAG "[aeron_gltf_cook] materials"

/* AERON_ANIM_STATE_* names, as the loader reads them. */
static const char* const kStates[]   = { "threat.laser",
										 "threat.turret",
										 "threat.beam",
										 "threat.missile_locking",
										 "threat.missile_locked",
										 "cannon.1",
										 "cannon.2",
										 "cannon.3",
										 "cannon.4",
										 "cannon.5",
										 "cannon.6",
										 "cannon.7",
										 "cannon.8",
										 "broken.cmd",
										 "broken.laser_ion",
										 "broken.beam_weapon",
										 "broken.shields",
										 "broken.throttle",
										 "broken.sensors",
										 "broken.laser_recharge",
										 "broken.engine_power",
										 "broken.shield_recharge",
										 "broken.beam_recharge",
										 "hull.25",
										 "hull.50",
										 "hull.75",
										 "laser_lock",
										 "warhead.locking",
										 "warhead.locked",
										 "hangar",
										 "target",
										 "always" };
/* Flipbook layers, which are also the kinds of their frames. */
static const char* const kLayers[]   = { "baseColor", "emissive" };
static const char* const kModes[]    = { "loop", "once" };
static const char* const kEnergies[] = { "shields", "tractor", "jamming" };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct Check {
	const char* path;        /* for messages */
	char        owner[64];   /* for messages: "material <name>" or "the model root" */
	const char* effects_dir; /* or NULL */
	const char* project;     /* project whose libraries are built into effects_dir first, or NULL */
	cJSON**     indexes;     /* loaded library indexes, by name */
	char**      index_names;
	size_t      index_count;
} Check;

static bool one_of(const cJSON* item, const char* const* names, size_t count) {
	for (size_t i = 0; cJSON_IsString(item) && i < count; ++i)
		if (strcmp(item->valuestring, names[i]) == 0)
			return true;
	return false;
}

static bool numbers(const cJSON* item, int count) {
	if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != count)
		return false;
	const cJSON* n;
	cJSON_ArrayForEach(n, item) {
		if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble))
			return false;
	}
	return true;
}

static bool int_in(const cJSON* item, double low, double high) {
	return cJSON_IsNumber(item) && item->valuedouble >= low && item->valuedouble <= high &&
		   item->valuedouble == floor(item->valuedouble);
}

static bool fail(Check* c, const char* what) {
	fprintf(stderr, TAG ": %s: %s: %s\n", c->path, c->owner, what);
	return false;
}

/* Only `allowed` members in `object`. */
static bool only_members(Check* c, const char* what, const cJSON* object, const char* const* allowed,
						 size_t count) {
	const cJSON* member;
	cJSON_ArrayForEach(member, object) {
		bool known = false;
		for (size_t i = 0; i < count && !known; ++i)
			known = strcmp(member->string, allowed[i]) == 0;
		if (!known) {
			fprintf(stderr, TAG ": %s: %s: %s: unknown field \"%s\"\n", c->path, c->owner, what, member->string);
			return false;
		}
	}
	return true;
}

/* Builds library `library` into the effects directory from the project,
 * when it holds it; false when that fails. */
static bool build_library(Check* c, const char* library) {
	char file[1024];
	snprintf(file, sizeof file, "%s/effects/%s/%s.yaml", c->project, library, library);
	FILE* f = fopen(file, "rb");
	if (!f)
		return true; /* not a library of the project: only checked */
	fclose(f);
	return EffectLibrary_BuildLibrary("[aeron_gltf_cook] library", file, library, c->effects_dir) !=
		   EFFECT_LIBRARY_FAILED;
}

/* Whether every image of library index `index` records its kind (indexes
 * from before EFFECT_LIBRARY_BAKE_VERSION 2 do not). */
static bool index_has_kinds(const cJSON* index) {
	const cJSON* entry;
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(index, "images")) {
		if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "kind")))
			return false;
	}
	return true;
}

/* Whether `key` is in library `library` of the effects directory, an image
 * of kind `kind` (any key without an effects directory). */
static bool key_known(Check* c, const char* library, const char* key, const char* kind) {
	if (!c->effects_dir)
		return true;
	cJSON* index = NULL;
	size_t i     = 0;
	while (i < c->index_count && strcmp(c->index_names[i], library) != 0)
		i++;
	if (i < c->index_count) {
		index = c->indexes[i];
	} else {
		char path[1024], err[512];
		snprintf(path, sizeof path, "%s/%s.yaml", c->effects_dir, library);
		const bool built = !c->project || build_library(c, library);
		index            = built ? YamlJson_Load(path, err, sizeof err) : NULL;
		if (index && !index_has_kinds(index)) {
			snprintf(err, sizeof err, "built by an older aeron_gltf_cook, without image kinds: rebuild it");
			cJSON_Delete(index);
			index = NULL;
		}
		cJSON** grown = (cJSON**)realloc(c->indexes, (c->index_count + 1) * sizeof *grown);
		char**  names = grown ? (char**)realloc(c->index_names, (c->index_count + 1) * sizeof *names) : NULL;
		if (grown)
			c->indexes = grown;
		if (names)
			c->index_names = names;
		if (!grown || !names) {
			cJSON_Delete(index);
			return false;
		}
		c->indexes[c->index_count]     = index;
		c->index_names[c->index_count] = strdup(library);
		c->index_count++;
		if (!index && built)
			fprintf(stderr, TAG ": %s: library %s: %s\n", c->path, library, err);
	}
	const cJSON* image =
		cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(index, "images"), key);
	const cJSON* image_kind = cJSON_GetObjectItemCaseSensitive(image, "kind");
	if (index && !image)
		fprintf(stderr, TAG ": %s: %s: \"%s\" is not in library %s\n", c->path, c->owner, key, library);
	else if (image && strcmp(image_kind->valuestring, kind) != 0)
		fprintf(stderr, TAG ": %s: %s: \"%s\" of library %s is a %s image, not %s\n", c->path, c->owner, key,
				library, image_kind->valuestring, kind);
	return image && strcmp(image_kind->valuestring, kind) == 0;
}

/* The library a flipbook or damage entry names. */
static const char* entry_library(Check* c, const cJSON* entry, const char* what) {
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(entry, "library");
	if (!cJSON_IsString(library)) {
		fprintf(stderr, TAG ": %s: %s: %s: library must name the effect library of its images\n", c->path,
				c->owner, what);
		return NULL;
	}
	return library->valuestring;
}

static bool check_flipbook(Check* c, const cJSON* in) {
	static const char* const fields[] = { "state",  "layer",            "mode", "library",
										  "frames", "emissiveStrength", "unlit" };
	const cJSON*             state    = cJSON_GetObjectItemCaseSensitive(in, "state");
	const cJSON*             layer    = cJSON_GetObjectItemCaseSensitive(in, "layer");
	const cJSON*             mode     = cJSON_GetObjectItemCaseSensitive(in, "mode");
	const cJSON*             frames   = cJSON_GetObjectItemCaseSensitive(in, "frames");
	const cJSON*             strength = cJSON_GetObjectItemCaseSensitive(in, "emissiveStrength");
	const cJSON*             unlit    = cJSON_GetObjectItemCaseSensitive(in, "unlit");
	if (!cJSON_IsObject(in) || !only_members(c, "flipbook", in, fields, COUNT(fields)))
		return false;
	if (!one_of(state, kStates, COUNT(kStates)) || !one_of(layer, kLayers, COUNT(kLayers)) ||
		!one_of(mode, kModes, COUNT(kModes)) || !cJSON_IsArray(frames) || cJSON_GetArraySize(frames) > 0xFFFF)
		return fail(c, "a flipbook needs a known state, layer (baseColor, emissive), mode (loop, once) and "
					   "frames");
	const char* library = cJSON_GetArraySize(frames) ? entry_library(c, in, "flipbook") : NULL;
	if (cJSON_GetArraySize(frames) && !library)
		return false;
	const cJSON* frame;
	cJSON_ArrayForEach(frame, frames) {
		const cJSON* key     = cJSON_GetArrayItem(frame, 0);
		const cJSON* seconds = cJSON_GetArrayItem(frame, 1);
		if (cJSON_GetArraySize(frame) != 2 || !cJSON_IsString(key) || !cJSON_IsNumber(seconds) ||
			!isfinite(seconds->valuedouble) || seconds->valuedouble < 0.0)
			return fail(c, "frames are [image, seconds] pairs");
		/* A flipbook's frames are images of its layer's kind. */
		if (!key_known(c, library, key->valuestring, layer->valuestring))
			return false;
	}
	if (strength && (!cJSON_IsNumber(strength) || !isfinite(strength->valuedouble) || strength->valuedouble < 0.0))
		return fail(c, "emissiveStrength is a number >= 0");
	if (unlit && (!cJSON_IsNumber(unlit) || !(unlit->valuedouble >= 0.0 && unlit->valuedouble <= 1.0)))
		return fail(c, "unlit is a number in [0, 1]");
	return true;
}

static bool check_damage(Check* c, const cJSON* in) {
	static const char* const fields[]     = { "library", "choice", "placement", "alternatives" };
	static const char* const alt_fields[] = { "stages", "scale", "offset" };
	static const char* const range[]      = { "min", "max" };
	const cJSON*             alternatives = cJSON_GetObjectItemCaseSensitive(in, "alternatives");
	if (!cJSON_IsObject(in) || !only_members(c, "damage", in, fields, COUNT(fields)))
		return false;
	const char* library = entry_library(c, in, "damage");
	if (!library)
		return false;
	if (!int_in(cJSON_GetObjectItemCaseSensitive(in, "choice"), 0, 4095) ||
		!int_in(cJSON_GetObjectItemCaseSensitive(in, "placement"), 0, 4095) || !cJSON_IsArray(alternatives) ||
		!cJSON_GetArraySize(alternatives) || cJSON_GetArraySize(alternatives) > 0xFFFF)
		return fail(c, "damage needs choice and placement (integers 0 to 4095) and alternatives");
	const cJSON* alt;
	cJSON_ArrayForEach(alt, alternatives) {
		const cJSON* stages = cJSON_GetObjectItemCaseSensitive(alt, "stages");
		const cJSON* scale  = cJSON_GetObjectItemCaseSensitive(alt, "scale");
		const cJSON* offset = cJSON_GetObjectItemCaseSensitive(alt, "offset");
		if (!cJSON_IsObject(alt) || !only_members(c, "damage alternative", alt, alt_fields, COUNT(alt_fields)))
			return false;
		const bool offset_ok =
			!offset || (cJSON_IsString(offset) && strcmp(offset->valuestring, "inside") == 0) ||
			(cJSON_IsObject(offset) && only_members(c, "damage offset", offset, range, COUNT(range)) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(offset, "min"), 2) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(offset, "max"), 2));
		bool ok = cJSON_IsArray(stages) && cJSON_GetArraySize(stages) == 3 && (!scale || numbers(scale, 2)) &&
				  offset_ok;
		const cJSON* stage;
		const cJSON* stage_list = ok ? stages : NULL;
		cJSON_ArrayForEach(stage, stage_list) {
			ok = ok && (cJSON_IsNull(stage) ||
						(cJSON_IsString(stage) && key_known(c, library, stage->valuestring, "damage")));
		}
		if (!ok)
			return fail(c, "a damage alternative is { stages: [image | null] x 3, scale: [min, max], offset: "
						   "inside | { min: [x, y], max: [x, y] } }");
	}
	return true;
}

static bool check_energy(Check* c, const cJSON* in) {
	const cJSON* e;
	bool         ok     = cJSON_IsArray(in);
	const cJSON* e_list = ok ? in : NULL;
	cJSON_ArrayForEach(e, e_list) { ok = ok && one_of(e, kEnergies, COUNT(kEnergies)); }
	return ok || fail(c, "energy lists shields, tractor and/or jamming");
}

static bool check_screen(Check* c, const cJSON* in) {
	static const char* const fields[]      = { "cover",  "transparent", "alwaysVisible", "hologram",
											   "noisy",  "hide",        "rects" };
	static const char* const rect_fields[] = { "source", "dst",  "bg",     "highlight",
											   "lock",   "gain", "layers", "bar" };
	static const char* const flags[]       = { "cover", "transparent", "alwaysVisible", "hologram", "noisy" };
	const cJSON*             rects         = cJSON_GetObjectItemCaseSensitive(in, "rects");
	if (!cJSON_IsObject(in) || !only_members(c, "screen", in, fields, COUNT(fields)))
		return false;
	bool ok = int_in(cJSON_GetObjectItemCaseSensitive(in, "hide"), 0, 4294967295.0) && cJSON_IsArray(rects) &&
			  cJSON_GetArraySize(rects) <= 16;
	for (size_t f = 0; ok && f < COUNT(flags); f++) {
		const cJSON* flag = cJSON_GetObjectItemCaseSensitive(in, flags[f]);
		ok                = !flag || cJSON_IsBool(flag);
	}
	const cJSON* rect;
	const cJSON* rect_list = ok ? rects : NULL;
	cJSON_ArrayForEach(rect, rect_list) {
		const cJSON* gain   = cJSON_GetObjectItemCaseSensitive(rect, "gain");
		const cJSON* layers = cJSON_GetObjectItemCaseSensitive(rect, "layers");
		ok = ok && cJSON_IsObject(rect) && only_members(c, "screen rectangle", rect, rect_fields, COUNT(rect_fields)) &&
			 int_in(cJSON_GetObjectItemCaseSensitive(rect, "source"), 0, 63) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(rect, "dst"), 4) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(rect, "bg"), 3) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(rect, "highlight"), 3) &&
			 numbers(cJSON_GetObjectItemCaseSensitive(rect, "lock"), 3) && cJSON_IsNumber(gain) &&
			 isfinite(gain->valuedouble) && gain->valuedouble >= 0.0 && int_in(layers, 0, 7) &&
			 (!((int)layers->valuedouble & 4) || numbers(cJSON_GetObjectItemCaseSensitive(rect, "bar"), 3));
	}
	return ok || fail(c, "a screen is { flags, hide: bitmask, rects: up to 16 { source: 0-63, dst: [u0, v0, u1, "
						 "v1], bg, highlight, lock: [r, g, b], gain >= 0, layers: 0-7, bar: [r, g, b] with "
						 "layer 4 } }");
}

/* A material's AERON_flight_model object. */
static bool check_material(Check* c, const char* json) {
	/* The cook checks `skins` when it expands them (skin_layers.h). */
	static const char* const fields[] = { "flipbooks", "damage", "energy", "screen", "skins" };
	cJSON*                   object   = cJSON_Parse(json);
	const cJSON*             books    = cJSON_GetObjectItemCaseSensitive(object, "flipbooks");
	const cJSON*             damage   = cJSON_GetObjectItemCaseSensitive(object, "damage");
	const cJSON*             energy   = cJSON_GetObjectItemCaseSensitive(object, "energy");
	const cJSON*             screen   = cJSON_GetObjectItemCaseSensitive(object, "screen");
	bool ok = cJSON_IsObject(object) ? only_members(c, "AERON_flight_model", object, fields, COUNT(fields))
									 : fail(c, "AERON_flight_model is an object");
	if (ok && books && !cJSON_IsArray(books))
		ok = fail(c, "flipbooks is a list");
	const cJSON* book;
	const cJSON* book_list = ok ? books : NULL;
	cJSON_ArrayForEach(book, book_list) { ok = ok && check_flipbook(c, book); }
	ok = ok && (!damage || check_damage(c, damage)) && (!energy || check_energy(c, energy)) &&
		 (!screen || check_screen(c, screen));
	cJSON_Delete(object);
	return ok;
}

static const char* flight_model_json(const cgltf_extension* extensions, cgltf_size count) {
	for (cgltf_size e = 0; e < count; e++)
		if (extensions[e].name && extensions[e].data && strcmp(extensions[e].name, "AERON_flight_model") == 0)
			return extensions[e].data;
	return NULL;
}

bool aeron_gltf_cook_check_materials(const char* path, const char* effects_dir, const char* project) {
	cgltf_options options = { 0 };
	cgltf_data*   data    = NULL;
	if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
		fprintf(stderr, TAG ": cannot parse %s\n", path);
		return false;
	}
	Check c  = { .path = path, .effects_dir = effects_dir, .project = project };
	bool  ok = true;
	for (cgltf_size m = 0; m < data->materials_count; m++) {
		const char* json = flight_model_json(data->materials[m].extensions, data->materials[m].extensions_count);
		if (!json)
			continue;
		snprintf(c.owner, sizeof c.owner, "material %s", data->materials[m].name ? data->materials[m].name : "?");
		ok = check_material(&c, json) && ok;
	}
	/* The model root's energy default. */
	const cgltf_node* root = data->scene && data->scene->nodes_count ? data->scene->nodes[0] : NULL;
	const char*       json = root ? flight_model_json(root->extensions, root->extensions_count) : NULL;
	cJSON*            node = json ? cJSON_Parse(json) : NULL;
	const cJSON*      energy = cJSON_GetObjectItemCaseSensitive(node, "energy");
	snprintf(c.owner, sizeof c.owner, "the model root");
	ok = (!energy || check_energy(&c, energy)) && ok;
	cJSON_Delete(node);
	for (size_t i = 0; i < c.index_count; i++) {
		cJSON_Delete(c.indexes[i]);
		free(c.index_names[i]);
	}
	free(c.indexes);
	free(c.index_names);
	cgltf_free(data);
	return ok;
}
