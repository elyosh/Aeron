/*
 * The animation merge of aeron_gltf_cook (animations.h). Validates what the
 * runtime loader (aeron/src/scene/gltf_mesh.c) reads, so a merged model
 * loads with its animations.
 */
#include "animations.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "effect_files.h"
#include "effect_library.h"
#include "yaml_json.h"

#define TAG "[aeron_gltf_cook] animations"

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
/* Flipbook layers as authored, and as the loader reads them. */
static const char* const kLayers[]       = { "baseColor", "emissive" };
static const char* const kLoaderLayers[] = { "colour", "illumination" };
static const char* const kModes[]    = { "loop", "once" };
static const char* const kEnergies[] = { "shields", "tractor", "jamming" };
static const char* const kMembers[]  = { "aeronFlipbooks", "aeronDamage", "aeronEnergy" };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct Merge {
	const char* anim;        /* for messages */
	const char* material;    /* for messages */
	const char* effects_dir; /* or NULL */
	const char* sources;     /* library sources built into effects_dir first, or NULL */
	cJSON**     indexes;     /* loaded library indexes, by name */
	char**      index_names;
	size_t      index_count;
} Merge;

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

static bool non_negative_int(const cJSON* item) {
	return cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= INT_MAX &&
		   item->valuedouble == floor(item->valuedouble);
}

/* Only `allowed` members in `object`. */
static bool only_members(Merge* m, const char* what, const cJSON* object, const char* const* allowed,
						 size_t count) {
	const cJSON* member;
	cJSON_ArrayForEach(member, object) {
		bool known = false;
		for (size_t i = 0; i < count && !known; ++i)
			known = strcmp(member->string, allowed[i]) == 0;
		if (!known) {
			fprintf(stderr, TAG ": %s: material %s: %s: unknown field \"%s\"\n", m->anim, m->material, what,
					member->string);
			return false;
		}
	}
	return true;
}

/* Builds library `library` into the effects directory from the library
 * sources, when they hold it; false when that fails. */
static bool build_library(Merge* m, const char* library) {
	char file[1024];
	snprintf(file, sizeof file, "%s/%s/%s.yaml", m->sources, library, library);
	FILE* f = fopen(file, "rb");
	if (!f)
		return true; /* not a source library: only checked */
	fclose(f);
	return EffectLibrary_BuildLibrary("[aeron_gltf_cook] library", file, library, m->effects_dir) !=
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
 * of kind `kind`. */
static bool key_known(Merge* m, const char* library, const char* key, const char* kind) {
	if (!m->effects_dir)
		return true;
	cJSON* index = NULL;
	size_t i     = 0;
	while (i < m->index_count && strcmp(m->index_names[i], library) != 0)
		i++;
	if (i < m->index_count) {
		index = m->indexes[i];
	} else {
		char path[1024], err[512];
		snprintf(path, sizeof path, "%s/%s.yaml", m->effects_dir, library);
		const bool built = !m->sources || build_library(m, library);
		index            = built ? YamlJson_Load(path, err, sizeof err) : NULL;
		if (index && !index_has_kinds(index)) {
			snprintf(err, sizeof err, "built by an older aeron_gltf_cook, without image kinds: rebuild it");
			cJSON_Delete(index);
			index = NULL;
		}
		cJSON** grown = (cJSON**)realloc(m->indexes, (m->index_count + 1) * sizeof *grown);
		char**  names = grown ? (char**)realloc(m->index_names, (m->index_count + 1) * sizeof *names) : NULL;
		if (grown)
			m->indexes = grown;
		if (names)
			m->index_names = names;
		if (!grown || !names) {
			cJSON_Delete(index);
			return false;
		}
		m->indexes[m->index_count]     = index;
		m->index_names[m->index_count] = strdup(library);
		m->index_count++;
		if (!index && built)
			fprintf(stderr, TAG ": %s: library %s: %s\n", m->anim, library, err);
	}
	const cJSON* image =
		cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(index, "images"), key);
	const cJSON* image_kind = cJSON_GetObjectItemCaseSensitive(image, "kind");
	if (index && !image)
		fprintf(stderr, TAG ": %s: material %s: \"%s\" is not in library %s\n", m->anim, m->material, key,
				library);
	else if (image && strcmp(image_kind->valuestring, kind) != 0)
		fprintf(stderr, TAG ": %s: material %s: \"%s\" of library %s is a %s image, not %s\n", m->anim,
				m->material, key, library, image_kind->valuestring, kind);
	return image && strcmp(image_kind->valuestring, kind) == 0;
}

/* The library a flipbook or damage entry names. */
static const char* entry_library(Merge* m, const cJSON* entry, const char* what) {
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(entry, "library");
	if (!cJSON_IsString(library)) {
		fprintf(stderr, TAG ": %s: material %s: %s: library must name the effect library of its images\n",
				m->anim, m->material, what);
		return NULL;
	}
	return library->valuestring;
}

/* One flipbook, validated, in the order the loader documents. */
static cJSON* flipbook(Merge* m, const cJSON* in) {
	static const char* const fields[] = { "state",  "layer",           "mode", "library",
										  "frames", "emissiveStrength", "unlit" };
	const cJSON*             state    = cJSON_GetObjectItemCaseSensitive(in, "state");
	const cJSON*             layer    = cJSON_GetObjectItemCaseSensitive(in, "layer");
	const cJSON*             mode     = cJSON_GetObjectItemCaseSensitive(in, "mode");
	const cJSON*             frames   = cJSON_GetObjectItemCaseSensitive(in, "frames");
	const cJSON*             strength = cJSON_GetObjectItemCaseSensitive(in, "emissiveStrength");
	const cJSON*             unlit    = cJSON_GetObjectItemCaseSensitive(in, "unlit");
	if (!cJSON_IsObject(in) || !only_members(m, "flipbook", in, fields, COUNT(fields)))
		return NULL;
	if (!one_of(state, kStates, COUNT(kStates)) || !one_of(layer, kLayers, COUNT(kLayers)) ||
		!one_of(mode, kModes, COUNT(kModes)) || !cJSON_IsArray(frames)) {
		fprintf(stderr,
				TAG ": %s: material %s: a flipbook needs a known state, layer (baseColor, emissive), "
					"mode (loop, once) and frames\n",
				m->anim, m->material);
		return NULL;
	}
	const char* library = cJSON_GetArraySize(frames) ? entry_library(m, in, "flipbook") : NULL;
	if (cJSON_GetArraySize(frames) && !library)
		return NULL;
	const cJSON* frame;
	cJSON_ArrayForEach(frame, frames) {
		const cJSON* key     = cJSON_GetArrayItem(frame, 0);
		const cJSON* seconds = cJSON_GetArrayItem(frame, 1);
		if (cJSON_GetArraySize(frame) != 2 || !cJSON_IsString(key) || !cJSON_IsNumber(seconds) ||
			!isfinite(seconds->valuedouble) || seconds->valuedouble < 0.0) {
			fprintf(stderr, TAG ": %s: material %s: frames are [image, seconds] pairs\n", m->anim,
					m->material);
			return NULL;
		}
		/* A flipbook's frames are images of its layer's kind. */
		if (!key_known(m, library, key->valuestring, layer->valuestring))
			return NULL;
	}
	if (strength &&
		(!cJSON_IsNumber(strength) || !isfinite(strength->valuedouble) || strength->valuedouble < 0.0)) {
		fprintf(stderr, TAG ": %s: material %s: emissiveStrength is a number >= 0\n", m->anim, m->material);
		return NULL;
	}
	if (unlit && (!cJSON_IsNumber(unlit) || !(unlit->valuedouble >= 0.0 && unlit->valuedouble <= 1.0))) {
		fprintf(stderr, TAG ": %s: material %s: unlit is a number in [0, 1]\n", m->anim, m->material);
		return NULL;
	}
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "state", state->valuestring);
	cJSON_AddStringToObject(out, "layer", kLoaderLayers[strcmp(layer->valuestring, kLayers[0]) == 0 ? 0 : 1]);
	cJSON_AddStringToObject(out, "mode", mode->valuestring);
	if (library)
		cJSON_AddStringToObject(out, "library", library);
	cJSON_AddItemToObject(out, "frames", cJSON_Duplicate(frames, true));
	if (strength)
		cJSON_AddNumberToObject(out, "emissiveStrength", strength->valuedouble);
	if (unlit)
		cJSON_AddNumberToObject(out, "unlit", unlit->valuedouble);
	return out;
}

/* The damage entry, validated. */
static cJSON* damage(Merge* m, const cJSON* in) {
	static const char* const fields[]     = { "library", "choice", "placement", "alternatives" };
	static const char* const alt_fields[] = { "stages", "scale", "offset" };
	const cJSON*             choice       = cJSON_GetObjectItemCaseSensitive(in, "choice");
	const cJSON*             placement    = cJSON_GetObjectItemCaseSensitive(in, "placement");
	const cJSON*             alternatives = cJSON_GetObjectItemCaseSensitive(in, "alternatives");
	if (!cJSON_IsObject(in) || !only_members(m, "damage", in, fields, COUNT(fields)))
		return NULL;
	const char* library = entry_library(m, in, "damage");
	if (!library)
		return NULL;
	if (!non_negative_int(choice) || !non_negative_int(placement) || !cJSON_IsArray(alternatives) ||
		!cJSON_GetArraySize(alternatives)) {
		fprintf(stderr,
				TAG ": %s: material %s: damage needs choice, placement (integers >= 0) and "
					"alternatives\n",
				m->anim, m->material);
		return NULL;
	}
	const cJSON* alt;
	cJSON_ArrayForEach(alt, alternatives) {
		const cJSON* stages = cJSON_GetObjectItemCaseSensitive(alt, "stages");
		const cJSON* scale  = cJSON_GetObjectItemCaseSensitive(alt, "scale");
		const cJSON* offset = cJSON_GetObjectItemCaseSensitive(alt, "offset");
		if (!cJSON_IsObject(alt) ||
			!only_members(m, "damage alternative", alt, alt_fields, COUNT(alt_fields)))
			return NULL;
		bool ok = cJSON_IsArray(stages) && cJSON_GetArraySize(stages) == 3 && (!scale || numbers(scale, 2)) &&
				  (!offset || numbers(offset, 4) ||
				   (cJSON_IsString(offset) && strcmp(offset->valuestring, "inside") == 0));
		const cJSON* stage;
		const cJSON* stage_list = ok ? stages : NULL;
		cJSON_ArrayForEach(stage, stage_list) {
			ok = ok && (cJSON_IsNull(stage) ||
						(cJSON_IsString(stage) && key_known(m, library, stage->valuestring, "damage")));
		}
		if (!ok) {
			fprintf(stderr,
					TAG ": %s: material %s: a damage alternative is { stages: [image | null] x 3, "
						"scale: [min, max], offset: [x0, y0, x1, y1] | inside }\n",
					m->anim, m->material);
			return NULL;
		}
	}
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "library", library);
	cJSON_AddNumberToObject(out, "choice", choice->valuedouble);
	cJSON_AddNumberToObject(out, "placement", placement->valuedouble);
	cJSON_AddItemToObject(out, "alternatives", cJSON_Duplicate(alternatives, true));
	return out;
}

/* The energy list, validated. */
static cJSON* energy(Merge* m, const cJSON* in) {
	const cJSON* e;
	bool         ok     = cJSON_IsArray(in);
	const cJSON* e_list = ok ? in : NULL;
	cJSON_ArrayForEach(e, e_list) { ok = ok && one_of(e, kEnergies, COUNT(kEnergies)); }
	if (!ok) {
		fprintf(stderr, TAG ": %s: material %s: energy lists shields, tractor and/or jamming\n", m->anim,
				m->material);
		return NULL;
	}
	return cJSON_Duplicate(in, true);
}

static char* read_text(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = n >= 0 ? (char*)malloc((size_t)n + 1) : NULL;
	if (text && fread(text, 1, (size_t)n, f) == (size_t)n) {
		text[n] = 0;
	} else {
		free(text);
		text = NULL;
	}
	fclose(f);
	return text;
}

/* Sets material `mat`'s extras member `name` to `item` (owned), replacing it. */
static void set_extra(cJSON* mat, const char* name, cJSON* item) {
	cJSON* extras = cJSON_GetObjectItemCaseSensitive(mat, "extras");
	if (!cJSON_IsObject(extras)) {
		cJSON_DeleteItemFromObjectCaseSensitive(mat, "extras");
		extras = cJSON_AddObjectToObject(mat, "extras");
	}
	cJSON_DeleteItemFromObjectCaseSensitive(extras, name);
	cJSON_AddItemToObject(extras, name, item);
}

/* Merges one material entry of the animation file into material `mat`. */
static bool merge_material(Merge* m, cJSON* mat, const cJSON* entry) {
	static const char* const fields[] = { "flipbooks", "damage", "energy" };
	if (!cJSON_IsObject(entry) || !only_members(m, "material", entry, fields, COUNT(fields)))
		return false;
	cJSON*       members[3] = { NULL, NULL, NULL };
	bool         ok         = true;
	const cJSON* books      = cJSON_GetObjectItemCaseSensitive(entry, "flipbooks");
	if (books) {
		ok         = cJSON_IsArray(books);
		members[0] = cJSON_CreateArray();
		const cJSON* book;
		const cJSON* book_list = ok ? books : NULL;
		cJSON_ArrayForEach(book, book_list) {
			cJSON* b = flipbook(m, book);
			ok       = ok && b;
			if (b)
				cJSON_AddItemToArray(members[0], b);
		}
		if (!cJSON_IsArray(books))
			fprintf(stderr, TAG ": %s: material %s: flipbooks is a list\n", m->anim, m->material);
	}
	const cJSON* dmg = cJSON_GetObjectItemCaseSensitive(entry, "damage");
	if (ok && dmg)
		ok = (members[1] = damage(m, dmg)) != NULL;
	const cJSON* nrg = cJSON_GetObjectItemCaseSensitive(entry, "energy");
	if (ok && nrg)
		ok = (members[2] = energy(m, nrg)) != NULL;
	if (!ok) {
		for (int k = 0; k < 3; ++k)
			cJSON_Delete(members[k]);
		return false;
	}
	for (int k = 0; k < 3; ++k) {
		if (members[k])
			set_extra(mat, kMembers[k], members[k]);
	}
	return true;
}

/* Whether `file` (read from `anim`) has a model animation file's members,
 * with a message when not. */
static bool check_members(const char* anim, const cJSON* file) {
	const cJSON* version   = cJSON_GetObjectItemCaseSensitive(file, "version");
	const cJSON* materials = cJSON_GetObjectItemCaseSensitive(file, "materials");
	bool         ok        = cJSON_IsNumber(version) && version->valueint == 1 && cJSON_IsObject(materials);
	/* A model's animations only; an effect library is its own file
	   (--library). */
	const cJSON* member;
	cJSON_ArrayForEach(member, file) {
		ok = ok && (strcmp(member->string, "version") == 0 || strcmp(member->string, "energy") == 0 ||
					strcmp(member->string, "materials") == 0);
	}
	if (!ok) {
		fprintf(stderr, TAG ": %s: a model animation file has version 1, materials and an energy default\n",
				anim);
		return false;
	}
	/* The energy effects of every material without its own. */
	const cJSON* defaults = cJSON_GetObjectItemCaseSensitive(file, "energy");
	if (defaults) {
		Merge  m       = { .anim = anim, .material = "(the energy default)" };
		cJSON* checked = energy(&m, defaults);
		ok             = checked != NULL;
		cJSON_Delete(checked);
	}
	return ok;
}

/* The model animation file `anim`, or NULL with a message. */
static cJSON* load_file(const char* anim) {
	char   err[512];
	cJSON* file = YamlJson_Load(anim, err, sizeof err);
	if (!file) {
		fprintf(stderr, TAG ": %s\n", err);
		return NULL;
	}
	if (!check_members(anim, file)) {
		cJSON_Delete(file);
		return NULL;
	}
	return file;
}

/* Checks every material entry of `file` as the merge does, libraries aside. */
static bool check_materials(const char* anim, const cJSON* file) {
	Merge        m  = { .anim = anim };
	bool         ok = true;
	const cJSON* entry;
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(file, "materials")) {
		m.material   = entry->string;
		cJSON* probe = cJSON_CreateObject();
		ok           = merge_material(&m, probe, entry) && ok;
		cJSON_Delete(probe);
	}
	return ok;
}

bool aeron_gltf_cook_write_animations(const char* label, const cJSON* file, const char* out) {
	return check_members(label, file) && check_materials(label, file) &&
		   EffectFiles_Write(TAG, file, NULL, out);
}

bool aeron_gltf_cook_print_animations(const char* anim) {
	cJSON* file = load_file(anim);
	if (!file)
		return false;
	bool  ok      = check_materials(anim, file);
	char* printed = ok ? cJSON_PrintUnformatted(file) : NULL;
	ok = printed && fputs(printed, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
	free(printed);
	cJSON_Delete(file);
	return ok;
}

bool aeron_gltf_cook_merge_animations(const char* gltf, const char* anim, const char* effects_dir,
									  const char* library_sources, const char* out_gltf) {
	char*  text = read_text(gltf);
	cJSON* doc  = text ? cJSON_Parse(text) : NULL;
	free(text);
	if (!doc) {
		fprintf(stderr, TAG ": the input is not a .gltf (JSON)\n");
		return false;
	}
	cJSON* file = load_file(anim);
	if (!file) {
		cJSON_Delete(doc);
		return false;
	}
	Merge        m              = { .anim = anim, .effects_dir = effects_dir, .sources = library_sources };
	bool         ok             = true;
	cJSON*       gltf_materials = cJSON_GetObjectItemCaseSensitive(doc, "materials");
	const cJSON* entry;
	size_t       merged = 0;
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(file, "materials")) {
		m.material  = entry->string;
		cJSON* mat  = NULL;
		int    hits = 0;
		cJSON* candidate;
		cJSON_ArrayForEach(candidate, gltf_materials) {
			const cJSON* name = cJSON_GetObjectItemCaseSensitive(candidate, "name");
			if (cJSON_IsString(name) && strcmp(name->valuestring, entry->string) == 0) {
				mat = candidate;
				hits++;
			}
		}
		if (hits != 1) {
			fprintf(stderr, TAG ": %s: material %s: %s in %s\n", anim, entry->string,
					hits ? "more than one material has this name" : "no such material", gltf);
			ok = false;
			continue;
		}
		if (merge_material(&m, mat, entry))
			merged++;
		else
			ok = false;
	}
	/* The energy default, for every material without energy of its own in
	   the file, listed or not: it replaces energy extras the material has. */
	const cJSON* defaults  = cJSON_GetObjectItemCaseSensitive(file, "energy");
	const cJSON* entries   = cJSON_GetObjectItemCaseSensitive(file, "materials");
	cJSON*       defaulted = defaults ? gltf_materials : NULL;
	cJSON*       target;
	cJSON_ArrayForEach(target, defaulted) {
		const cJSON* name = cJSON_GetObjectItemCaseSensitive(target, "name");
		const cJSON* own =
			cJSON_IsString(name) ? cJSON_GetObjectItemCaseSensitive(entries, name->valuestring) : NULL;
		if (!cJSON_GetObjectItemCaseSensitive(own, "energy"))
			set_extra(target, "aeronEnergy", cJSON_Duplicate(defaults, true));
	}
	char* printed = ok ? cJSON_PrintUnformatted(doc) : NULL;
	if (printed) {
		FILE* f = fopen(out_gltf, "wb");
		ok      = f && fputs(printed, f) >= 0;
		ok      = f && fclose(f) == 0 && ok;
		if (!ok)
			fprintf(stderr, TAG ": cannot write %s\n", out_gltf);
	} else {
		ok = false;
	}
	if (ok)
		fprintf(stderr, TAG ": %zu material(s) from %s\n", merged, anim);
	free(printed);
	for (size_t i = 0; i < m.index_count; ++i) {
		cJSON_Delete(m.indexes[i]);
		free(m.index_names[i]);
	}
	free(m.indexes);
	free(m.index_names);
	cJSON_Delete(doc);
	cJSON_Delete(file);
	return ok;
}
