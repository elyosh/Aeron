#include "xwau_hangars.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_mission_files.h"
#include "xwau_missions.h"

#define LABEL "hangars"

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

/* The sections of a layer: the hook's section, and its key in the file. */
enum { OBJECTS, MAP, CAMERA, FAM_MAP, FAM_CAMERA, SECTION_COUNT };

static const char* const kSections[SECTION_COUNT] = { "HangarObjects", "HangarMap", "HangarCamera",
													  "FamHangarMap", "FamHangarCamera" };

static const int kCameraKeys[]    = { 1, 2, 3, 6, 9 };
static const int kFamCameraKeys[] = { 1, 2, 3, 6, 7, 8, 9 };

typedef enum KeyKind { KEY_INT, KEY_TYPE, KEY_STRING, KEY_INT_LIST, KEY_TYPE_LIST, KEY_NOT_IMPORTED } KeyKind;

/* The HangarObjects keys, and how each value is read. */
static const struct {
	const char* name;
	KeyKind     kind;
} kKeys[] = {
	{ "LoadShuttle", KEY_INT },
	{ "ShuttleMarkings", KEY_INT },
	{ "ShuttlePositionX", KEY_INT },
	{ "ShuttlePositionY", KEY_INT },
	{ "ShuttlePositionZ", KEY_INT },
	{ "ShuttleOrientation", KEY_INT },
	{ "IsShuttleFloorInverted", KEY_INT },
	{ "ShuttleAnimationStraightLine", KEY_INT },
	{ "ShuttleAnimationElevation", KEY_INT },
	{ "LoadDroids", KEY_INT },
	{ "DroidsPositionZ", KEY_INT },
	{ "IsDroidsFloorInverted", KEY_INT },
	{ "LoadDroid1", KEY_INT },
	{ "Droid1PositionZ", KEY_INT },
	{ "IsDroid1FloorInverted", KEY_INT },
	{ "Droid1Update", KEY_INT },
	{ "Droid1Markings", KEY_INT },
	{ "LoadDroid2", KEY_INT },
	{ "Droid2PositionZ", KEY_INT },
	{ "IsDroid2FloorInverted", KEY_INT },
	{ "Droid2Update", KEY_INT },
	{ "Droid2Markings", KEY_INT },
	{ "HangarRoofCranePositionX", KEY_INT },
	{ "HangarRoofCranePositionY", KEY_INT },
	{ "HangarRoofCranePositionZ", KEY_INT },
	{ "HangarRoofCraneAxis", KEY_INT },
	{ "HangarRoofCraneLowOffset", KEY_INT },
	{ "HangarRoofCraneHighOffset", KEY_INT },
	{ "IsHangarFloorInverted", KEY_INT },
	{ "HangarFloorInvertedHeight", KEY_INT },
	{ "HangarIff", KEY_INT },
	{ "DrawShadows", KEY_INT },
	{ "PlayerAnimationElevation", KEY_INT },
	{ "PlayerAnimationInvertedElevation", KEY_INT },
	{ "PlayerAnimationStraightLine", KEY_INT },
	{ "PlayerOffsetX", KEY_INT },
	{ "PlayerOffsetY", KEY_INT },
	{ "PlayerOffsetZ", KEY_INT },
	{ "PlayerInvertedOffsetX", KEY_INT },
	{ "PlayerInvertedOffsetY", KEY_INT },
	{ "PlayerInvertedOffsetZ", KEY_INT },
	{ "IsPlayerFloorInverted", KEY_INT },
	{ "ShuttleModelIndex", KEY_TYPE },
	{ "Droid1ModelIndex", KEY_TYPE },
	{ "Droid2ModelIndex", KEY_TYPE },
	{ "ShuttleObjectProfile", KEY_STRING },
	{ "Droid1ObjectProfile", KEY_STRING },
	{ "Droid2ObjectProfile", KEY_STRING },
	{ "ShuttleAnimation", KEY_STRING },
	{ "PlayerOffsetsX", KEY_INT_LIST },
	{ "PlayerOffsetsY", KEY_INT_LIST },
	{ "PlayerOffsetsZ", KEY_INT_LIST },
	{ "PlayerModelIndices", KEY_TYPE_LIST },
	{ "PlayerFloorInvertedModelIndices", KEY_TYPE_LIST },
	{ "LightColorIntensity", KEY_NOT_IMPORTED },
	{ "LightColorRgb", KEY_NOT_IMPORTED },
};

/* ---- Text ------------------------------------------------------------------ */

/* A string that grows. `failed` is set when out of memory. */
typedef struct Text {
	char*  s;
	size_t len, cap;
	bool   failed;
} Text;

UTIL_PRINTF(2, 3) static void text_add(Text* t, const char* format, ...) {
	if (t->failed)
		return;
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int n = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (n < 0 || t->len + (size_t)n + 1 > t->cap) {
		const size_t cap  = (t->len + (size_t)(n < 0 ? 0 : n) + 1) * 2;
		char*        grow = n < 0 ? NULL : (char*)realloc(t->s, cap);
		if (!grow) {
			t->failed = true;
			va_end(args);
			return;
		}
		t->s   = grow;
		t->cap = cap;
	}
	vsnprintf(t->s + t->len, t->cap - t->len, format, args);
	t->len += (size_t)n;
	va_end(args);
}

/* Adds ISO-8859-1 `value` as a quoted YAML string, in UTF-8. */
static void text_quoted(Text* t, const char* value) {
	text_add(t, "\"");
	for (const unsigned char* c = (const unsigned char*)value; *c; ++c) {
		if (*c == '"' || *c == '\\')
			text_add(t, "\\%c", *c);
		else if (*c < 0x80)
			text_add(t, "%c", *c);
		else
			text_add(t, "%c%c", 0xC0 | (*c >> 6), 0x80 | (*c & 0x3F));
	}
	text_add(t, "\"");
}

/* Moves the text of `t` into `lines`. Returns false when out of memory. */
static bool text_take(Text* t, XwauLines* lines) {
	const bool ok = !t->failed && XwauLines_Add(lines, t->s ? t->s : "", t->len);
	free(t->s);
	memset(t, 0, sizeof *t);
	return ok;
}

/* ---- Models ---------------------------------------------------------------- */

/* XWAU's model indices and OpenXWA's object types, and the converted
 * models. */
typedef struct Models {
	const char*     game;
	char**          glbs; /* the files in <mod>/flight/models */
	size_t          glb_count;
	const XwauGameTables* stock;
	XwauGameTables*       xwau;
	/* The model that OpenXWA loads for each object type: the `model` of its
	   object type file, or "" for the stock row's. */
	char object_models[XWAU_GAME_OBJECT_TYPES][128];
} Models;

static const char* openxwa_model(const Models* m, int type) {
	return m->object_models[type][0] ? m->object_models[type] : XwauGameTables_RowModel(m->stock, type);
}

/* The OpenXWA object type that loads XWAU's model index `index`, or -1. */
static int object_type(const Models* m, long long index) {
	if (index < 0 || index >= XWAU_GAME_OBJECT_TYPES)
		return -1;
	const char* name = XwauGameTables_RowModel(m->xwau, (int)index);
	if (!name[0])
		return -1;
	if (strcmp(openxwa_model(m, (int)index), name) == 0)
		return (int)index;
	int found = -1, count = 0;
	for (int type = 0; type < XWAU_GAME_OBJECT_TYPES; ++type) {
		if (strcmp(openxwa_model(m, type), name) == 0) {
			found = type;
			++count;
		}
	}
	return count == 1 ? found : -1;
}

/* Whether XWAU's model index `index` loads a model in XWAU. An index
 * without one loads nothing in XWAU either. */
static bool xwau_has_model(const Models* m, long long index) {
	return index >= 0 && index < XWAU_GAME_OBJECT_TYPES && XwauGameTables_RowModel(m->xwau, (int)index)[0];
}

/* "model index <n> (<XWAU's model>)", for the reports. */
static const char* describe(const Models* m, long long index, char* out, size_t cap) {
	snprintf(out, cap, "model index %lld (%s)", index,
			 index >= 0 && index < XWAU_GAME_OBJECT_TYPES ? XwauGameTables_RowModel(m->xwau, (int)index)
														  : "");
	return out;
}

static bool converted(const Models* m, const char* name) {
	char glb[300];
	snprintf(glb, sizeof glb, "%s.glb", name);
	for (size_t i = 0; i < m->glb_count; ++i)
		if (strcasecmp(m->glbs[i], glb) == 0)
			return true;
	return false;
}

/* Reads `<key>: <value>` at the start of `line`, as an object type file
 * writes it: the value may be in single quotes and may be followed by a
 * comment. Returns false when the line has another form. */
static bool object_type_line(const char* line, const char* key, char* value, size_t cap) {
	const size_t key_len = strlen(key);
	if (strncmp(line, key, key_len) != 0 || line[key_len] != ':')
		return false;
	const char* c = line + key_len + 1;
	while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\f' || *c == '\v')
		++c;
	if (*c == '\'')
		++c;
	const char* start = c;
	while ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' ||
		   *c == '_' || *c == '-')
		++c;
	const size_t len = (size_t)(c - start);
	if (!len || len >= cap)
		return false;
	if (*c == '\'')
		++c;
	while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\f' || *c == '\v')
		++c;
	if (*c && *c != '#')
		return false;
	memcpy(value, start, len);
	value[len] = 0;
	return true;
}

/* Reads `object_type` (a stock name) and `model` from each object type file
 * in `dir`. If two files have the same object type, the later file wins. */
static void read_object_models(const char* dir, const XwauNames* game_names, Models* m) {
	size_t count = 0;
	char** names = ListDir(dir, true, &count);
	for (size_t i = 0; i < count; ++i) {
		char path[1100];
		if (!EndsWithNoCase(names[i], ".yaml") || !PathFormat(path, sizeof path, "%s/%s", dir, names[i]))
			continue;
		size_t size = 0;
		char*  text = ReadWholeFile(path, &size);
		if (!text)
			continue;
		long type       = -1;
		char model[128] = "";
		for (char *line = text, *next; line; line = next) {
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			char value[128];
			if (object_type_line(line, "object_type", value, sizeof value)) {
				type = XwauNames_Find(&game_names->object_types, value);
			} else if (object_type_line(line, "model", value, sizeof value)) {
				LowerCopy(model, sizeof model, value);
			}
		}
		free(text);
		if (type >= 0 && type < XWAU_GAME_OBJECT_TYPES && model[0])
			snprintf(m->object_models[type], sizeof m->object_models[type], "%s", model);
	}
	FreeList(names, count);
}

static bool models_load(Models* m, const char* game, const XwauGameTables* stock, const XwauNames* game_names,
						const char* mod) {
	memset(m, 0, sizeof *m);
	m->game = game;
	char dir[1100];
	if (PathFormat(dir, sizeof dir, "%s/flight/models", mod))
		m->glbs = ListDir(dir, false, &m->glb_count);
	if (PathFormat(dir, sizeof dir, "%s/flight/object_types", mod))
		read_object_models(dir, game_names, m);
	m->stock = stock;
	m->xwau  = stock ? XwauGameTables_Load(game) : NULL;
	return m->xwau != NULL;
}

static void models_free(Models* m) {
	FreeList(m->glbs, m->glb_count);
	XwauGameTables_Free(m->xwau);
}

/* ---- Settings sources ------------------------------------------------------- */

/* Where a layer's settings are: the files <txt_base><separator><Section>.txt
 * and the sections [<Section>] of the .ini `ini`. */
typedef struct Source {
	const char* game;
	char        txt_base[1024]; /* "" when the layer has no .txt files */
	char        ini[1100];      /* the path found, or "" */
	const char* separator;
} Source;

static void source_init(Source* s, const char* game, const char* txt_base, const char* ini,
						const char* separator) {
	memset(s, 0, sizeof *s);
	s->game      = game;
	s->separator = separator;
	snprintf(s->txt_base, sizeof s->txt_base, "%s", txt_base);
	if (!Xwau_FindFile(game, ini, s->ini, sizeof s->ini))
		s->ini[0] = 0;
}

static bool read_found(const char* game, const char* rel, const char* section, XwauLines* out) {
	char path[1100];
	return XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, section, XWAU_RULES_CPP,
						  out);
}

/* The lines of `section`: the .txt file's and the .ini section's when
 * `append` is set, else the .txt file's, or the .ini section's when the .txt
 * file has none. Returns false when out of memory. */
static bool source_lines(const Source* s, const char* section, bool append, XwauLines* out) {
	memset(out, 0, sizeof *out);
	XwauLines txt = { 0 }, ini = { 0 };
	char      rel[1200];
	bool      ok = true;
	if (s->txt_base[0])
		ok = PathFormat(rel, sizeof rel, "%s%s%s.txt", s->txt_base, s->separator, section) &&
			 read_found(s->game, rel, "", &txt);
	ok = ok && XwauLines_Read(s->ini[0] ? s->ini : NULL, section, XWAU_RULES_CPP, &ini);
	if (ok && append) {
		*out = txt;
		for (size_t i = 0; ok && i < ini.count; ++i)
			ok = XwauLines_Add(out, ini.items[i], strlen(ini.items[i]));
		memset(&txt, 0, sizeof txt);
	} else if (ok) {
		*out = txt.count ? txt : ini;
		memset(txt.count ? &txt : &ini, 0, sizeof txt);
	}
	XwauLines_Free(&txt);
	XwauLines_Free(&ini);
	return ok;
}

/* Reads the digits of `text` up to `end` as a number up to 255. Returns -1
 * when there are none, when another character comes first, or when the
 * number is larger. */
static int small_number(const char* text, const char* end) {
	if (text >= end)
		return -1;
	int value = 0;
	for (const char* c = text; c < end; ++c) {
		if (*c < '0' || *c > '9')
			return -1;
		value = value > 255 ? value : value * 10 + (*c - '0');
	}
	return value <= 255 ? value : -1;
}

/* Marks in `found` the IFF numbers that `s` has for `section`: the .ini
 * headers [<Section><N>] and the files <base><Section><N>.txt. */
static void source_iffs(const Source* s, const char* section, bool found[256]) {
	const size_t section_len = strlen(section);
	size_t       size        = 0;
	char*        text        = s->ini[0] ? ReadWholeFile(s->ini, &size) : NULL;
	for (char *line = text, *next; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		Xwau_Trim(line, XWAU_RULES_CPP);
		const size_t len = strlen(line);
		if (len > section_len + 2 && line[0] == '[' && line[len - 1] == ']' &&
			strncasecmp(line + 1, section, section_len) == 0) {
			const int n = small_number(line + 1 + section_len, line + len - 1);
			if (n >= 0)
				found[n] = true;
		}
	}
	free(text);

	char dir_rel[1024], dir[1100];
	snprintf(dir_rel, sizeof dir_rel, "%s", s->txt_base);
	for (char* c = dir_rel; *c; ++c)
		if (*c == '\\')
			*c = '/';
	char*        slash    = strrchr(dir_rel, '/');
	const char*  base     = slash ? slash + 1 : dir_rel;
	const size_t base_len = strlen(base);
	if (slash)
		*slash = 0;
	if (!s->txt_base[0] || (slash ? !Xwau_FindDir(s->game, dir_rel, dir, sizeof dir)
								  : !PathFormat(dir, sizeof dir, "%s", s->game)))
		return;
	size_t count   = 0;
	char** entries = ListDir(dir, false, &count);
	for (size_t i = 0; i < count; ++i) {
		const char*  e   = entries[i];
		const size_t len = strlen(e);
		if (len > base_len + section_len + 4 && strncasecmp(e, base, base_len) == 0 &&
			strncasecmp(e + base_len, section, section_len) == 0 && EndsWithNoCase(e, ".txt")) {
			const int n = small_number(e + base_len + section_len, e + len - 4);
			if (n >= 0)
				found[n] = true;
		}
	}
	FreeList(entries, count);
}

/* ---- Layers ---------------------------------------------------------------- */

typedef struct Layer Layer;

typedef struct IffLayer {
	int    iff;
	Layer* layer;
} IffLayer;

/* What one source gives: each section's entries, without their indent. */
struct Layer {
	char      model[300];
	XwauLines sections[SECTION_COUNT]; /* objects: "<key>: <value>", map: "<entry>", camera: "<n>: <xyz>" */
	XwauLines swaps;                   /* "<model>: \"<model>\"" */
	XwauLines ships;                   /* a mission's, "<ship>: <ship>", sorted */
	IffLayer* iffs;
	size_t    iff_count;
};

static bool layer_empty(const Layer* l) {
	bool empty = !l->model[0] && !l->swaps.count && !l->ships.count && !l->iff_count;
	for (int s = 0; empty && s < SECTION_COUNT; ++s)
		empty = !l->sections[s].count;
	return empty;
}

static void layer_free(Layer* l) {
	for (int s = 0; s < SECTION_COUNT; ++s)
		XwauLines_Free(&l->sections[s]);
	XwauLines_Free(&l->swaps);
	XwauLines_Free(&l->ships);
	for (size_t i = 0; i < l->iff_count; ++i) {
		layer_free(l->iffs[i].layer);
		free(l->iffs[i].layer);
	}
	free(l->iffs);
	memset(l, 0, sizeof *l);
}

/* A line "<key> = <value>": the trimmed key and value. Returns false when
 * the line has no '=' or the key is empty. */
static bool key_value(const char* line, char* key, size_t key_cap, char* value, size_t value_cap) {
	const char* eq = strchr(line, '=');
	if (!eq)
		return false;
	snprintf(key, key_cap, "%.*s", (int)(eq - line), line);
	Xwau_Trim(key, XWAU_RULES_CPP);
	snprintf(value, value_cap, "%s", eq + 1);
	Xwau_Trim(value, XWAU_RULES_CPP);
	return key[0] != 0;
}

/* The number of `key` in `lines` (GetFileKeyValueInt with 0): 0 when the key
 * is missing or its value isn't a number. */
static long long int_value(const XwauLines* lines, const char* key) {
	char value[512];
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	long long number = 0;
	if (value[0] && !Xwau_StoiAutoInt(value, &number))
		number = 0;
	return number;
}

/* Matches "FlightModels\<model>.opt" in any case, and writes the model in
 * lower case. */
static bool swap_source(const char* key, char* out, size_t cap) {
	static const char prefix[] = "flightmodels\\";
	const size_t      len      = strlen(key);
	const size_t      start    = sizeof prefix - 1;
	if (len < start + 1 + 4 || strncasecmp(key, prefix, start) != 0 || !EndsWithNoCase(key, ".opt") ||
		len - 4 - start >= cap)
		return false;
	snprintf(out, cap, "%.*s", (int)(len - 4 - start), key + start);
	LowerInPlace(out);
	return true;
}

/* Matches a path to an .opt, and writes its model, the file name without
 * the folder and the extension, in lower case. */
static bool opt_target(const char* value, char* out, size_t cap) {
	const size_t len = strlen(value);
	if (!EndsWithNoCase(value, ".opt"))
		return false;
	const char* name = value;
	for (const char* c = value; c < value + len - 4; ++c)
		if (*c == '\\' || *c == '/')
			name = c + 1;
	const size_t name_len = (size_t)(value + len - 4 - name);
	if (!name_len || name_len >= cap)
		return false;
	snprintf(out, cap, "%.*s", (int)name_len, name);
	LowerInPlace(out);
	return true;
}

/* Whether the installation has the file at `value`, a path with '\' or '/'. */
static bool has_file(const char* game, const char* value) {
	char rel[1024], path[1100];
	snprintf(rel, sizeof rel, "%s", value);
	for (char* c = rel; *c; ++c)
		if (*c == '\\')
			*c = '/';
	return Xwau_FindFile(game, rel, path, sizeof path);
}

/* A HangarObjects swap line. Adds "<source>: \"<target>\"" to `swaps`.
 * Returns false when out of memory. */
static bool add_swap(const Models* m, const char* key, const char* value, const char* where,
					 XwauLines* swaps) {
	char source[256], target[256];
	/* The hook only matches FlightModels\<model>.opt keys. XWAU shows an
	   error for a file that doesn't exist, and the hook ignores swaps to the
	   model's exterior. */
	if (!swap_source(key, source, sizeof source))
		return true;
	if (!opt_target(value, target, sizeof target)) {
		if (has_file(m->game, value))
			ReportUnsupported("hangar object swaps to a file that isn't an OPT");
		return true;
	}
	char exterior[300];
	snprintf(exterior, sizeof exterior, "%sexterior", source);
	if (strcmp(target, exterior) == 0 || !has_file(m->game, value))
		return true;
	if (!converted(m, target)) {
		ReportFatal(LABEL, "%s: swap '%s = %s': %s.glb was not converted", where, key, value, target);
		return true;
	}
	Text t = { 0 };
	text_add(&t, "%s: ", source);
	text_quoted(&t, target);
	return text_take(&t, swaps);
}

/* A list key's numbers. For model indices, they become object types, and
 * the ones that load no model in XWAU either are dropped. Returns false
 * with *dropped set when the key is left out, and false without it when out
 * of memory. */
static bool list_value(const Models* m, const char* name, KeyKind kind, const char* value, const char* where,
					   Text* t, bool* dropped) {
	*dropped         = false;
	XwauLines tokens = { 0 };
	if (!Xwau_Tokenize(value, XWAU_RULES_CPP, &tokens))
		return false;
	long long* numbers = (long long*)calloc(tokens.count + 1, sizeof *numbers);
	bool       ok      = numbers != NULL;
	for (size_t i = 0; ok && i < tokens.count; ++i) {
		/* XWAU fails on a field that isn't a number. */
		if (!Xwau_StoiAutoInt(tokens.items[i], &numbers[i])) {
			*dropped = true;
			ok       = false;
		}
	}
	size_t count = tokens.count;
	if (ok && kind == KEY_TYPE_LIST) {
		Text missing = { 0 };
		for (size_t i = 0; i < tokens.count; ++i) {
			char text[300];
			if (object_type(m, numbers[i] & 0xFFFF) < 0)
				text_add(&missing, "%s%s", missing.len ? ", " : "",
						 describe(m, numbers[i], text, sizeof text));
		}
		/* The offsets lists follow PlayerModelIndices, so the key can't lose
		   one index. */
		if (missing.len && strcmp(name, "PlayerModelIndices") == 0) {
			ReportFatal(LABEL, "%s: %s: %s has no OpenXWA type", where, name,
						missing.failed ? "" : missing.s);
			*dropped = true;
			ok       = false;
		}
		free(missing.s);
		count = 0;
		for (size_t i = 0; ok && i < tokens.count; ++i) {
			char      text[300];
			const int type = object_type(m, numbers[i] & 0xFFFF);
			/* An index that loads no model in XWAU either is dropped. */
			if (type < 0 && xwau_has_model(m, numbers[i] & 0xFFFF))
				ReportFatal(LABEL, "%s: %s: %s has no OpenXWA type", where, name,
							describe(m, numbers[i], text, sizeof text));
			else if (type >= 0)
				numbers[count++] = type;
		}
	}
	if (ok) {
		text_add(t, "%s: [", name);
		for (size_t i = 0; i < count; ++i)
			text_add(t, "%s%lld", i ? ", " : "", numbers[i]);
		text_add(t, "]");
	}
	free(numbers);
	XwauLines_Free(&tokens);
	return ok || *dropped;
}

/* The HangarObjects lines: the first line of each key wins. Returns false
 * when out of memory. */
static bool objects_section(const Models* m, const XwauLines* lines, const char* where, Layer* out) {
	XwauLines seen = { 0 };
	bool      ok   = true;
	for (size_t i = 0; ok && i < lines->count; ++i) {
		char key[512], value[1024];
		/* XWAU ignores a line that isn't a key = value line, and uses the
		   first line of a key. */
		if (!key_value(lines->items[i], key, sizeof key, value, sizeof value))
			continue;
		bool known = false;
		for (size_t j = 0; !known && j < seen.count; ++j)
			known = Xwau_SameName(seen.items[j], key);
		if (known)
			continue;
		if (!(ok = XwauLines_Add(&seen, key, strlen(key))))
			break;
		if (EndsWithNoCase(key, ".opt")) {
			ok = add_swap(m, key, value, where, &out->swaps);
			continue;
		}
		int k = -1;
		for (size_t j = 0; k < 0 && j < sizeof kKeys / sizeof kKeys[0]; ++j)
			if (strcasecmp(kKeys[j].name, key) == 0)
				k = (int)j;
		/* The hook reads no other key. */
		if (k < 0)
			continue;
		const char*   name = kKeys[k].name;
		const KeyKind kind = kKeys[k].kind;
		/* The remaster lights the hangar itself. */
		if (kind == KEY_NOT_IMPORTED) {
			ReportUnsupported("the HangarObjects key %s", name);
			continue;
		}
		Text t    = { 0 };
		bool keep = true;
		if (kind == KEY_STRING) {
			text_add(&t, "%s: ", name);
			text_quoted(&t, value);
		} else if (!value[0]) {
			/* The key still hides the lower layers' value, and its default
			   applies. */
			text_add(&t, "%s: %s", name, kind == KEY_INT_LIST || kind == KEY_TYPE_LIST ? "[]" : "null");
		} else if (kind == KEY_INT_LIST || kind == KEY_TYPE_LIST) {
			bool dropped = false;
			ok           = list_value(m, name, kind, value, where, &t, &dropped);
			keep         = ok && !dropped;
		} else {
			long long number = 0;
			/* XWAU fails on a value that isn't a number. */
			if (!Xwau_StoiAutoInt(value, &number)) {
				keep = false;
			} else if (kind == KEY_TYPE) {
				char      text[300];
				const int type = object_type(m, number & 0xFFFF);
				if (type < 0 && xwau_has_model(m, number & 0xFFFF)) {
					ReportFatal(LABEL, "%s: %s: %s has no OpenXWA type", where, name,
								describe(m, number & 0xFFFF, text, sizeof text));
					keep = false;
				} else if (type < 0) {
					/* The index loads no model in XWAU either. */
					keep = false;
				}
				number = type;
			}
			if (keep)
				text_add(&t, "%s: %lld", name, number);
		}
		if (ok && keep)
			ok = text_take(&t, &out->sections[OBJECTS]);
		free(t.s);
	}
	XwauLines_Free(&seen);
	return ok;
}

/* The HangarMap lines: one entry per line, null where the hook skips it.
 * Returns false when out of memory. */
static bool map_section(const Models* m, const XwauLines* lines, const char* where, XwauLines* out) {
	bool ok = true;
	for (size_t i = 0; ok && i < lines->count; ++i) {
		const char* line   = lines->items[i];
		XwauLines   fields = { 0 };
		if (!(ok = Xwau_Tokenize(line, XWAU_RULES_CPP, &fields)))
			break;
		const size_t n     = fields.count;
		Text         entry = { 0 };
		/* XWAU skips a line with another number of fields, and fails on a
		   field that isn't a number. */
		if (n >= 6 && n <= 9) {
			/* The numbers: the first 6 fields, or the first 7, and the 9th. */
			long long numbers[8];
			size_t    count   = n >= 7 ? 7 : 6;
			bool      numeric = true;
			for (size_t f = 0; numeric && f < count; ++f)
				numeric = Xwau_StoiAutoInt(fields.items[f], &numbers[f]);
			if (numeric && n == 9)
				numeric = Xwau_StoiAutoInt(fields.items[8], &numbers[count++]);
			if (numeric) {
				long long index, markings, x, y, z, yaw, pitch;
				if (n == 6) {
					index    = numbers[0];
					markings = 0;
					x        = numbers[1];
					y        = numbers[2];
					z        = numbers[3];
					yaw      = numbers[4];
					pitch    = numbers[5];
				} else {
					index    = numbers[0];
					markings = numbers[1];
					x        = numbers[2];
					y        = numbers[3];
					z        = numbers[4];
					yaw      = numbers[5];
					pitch    = numbers[6];
				}
				const int type = object_type(m, index & 0xFFFF);
				char      text[300];
				/* An index that loads no model in XWAU either gives null. */
				if (type < 0 && xwau_has_model(m, index & 0xFFFF)) {
					ReportFatal(LABEL, "%s: line '%s': %s has no OpenXWA type", where, line,
								describe(m, index & 0xFFFF, text, sizeof text));
				} else if (type >= 0) {
					text_add(&entry, "[%d, %lld, %lld, %lld, %lld, %d, %d, ", type, markings, x, y, z,
							 (int)(int16_t)(uint16_t)(yaw & 0xFFFF),
							 (int)(int16_t)(uint16_t)(pitch & 0xFFFF));
					text_quoted(&entry, n >= 8 ? fields.items[7] : "Default");
					text_add(&entry, ", %d]", n == 9 ? numbers[7] != 0 : -1);
				}
			}
		}
		if (!entry.len)
			text_add(&entry, "null");
		ok = text_take(&entry, out);
		XwauLines_Free(&fields);
	}
	return ok;
}

/* A camera section: every key that the hook reads. Returns false when out
 * of memory. */
static bool camera_section(const XwauLines* lines, bool family, XwauLines* out) {
	const int*   keys  = family ? kFamCameraKeys : kCameraKeys;
	const size_t count = family ? sizeof kFamCameraKeys / sizeof kFamCameraKeys[0]
								: sizeof kCameraKeys / sizeof kCameraKeys[0];
	bool         ok    = true;
	for (size_t i = 0; ok && i < count; ++i) {
		long long xyz[3];
		for (int a = 0; a < 3; ++a) {
			char key[64];
			snprintf(key, sizeof key, "%sKey%d_%c", family ? "Fam" : "", keys[i], "XYZ"[a]);
			xyz[a] = int_value(lines, key);
		}
		Text t = { 0 };
		text_add(&t, "%d: [%lld, %lld, %lld]", keys[i], xyz[0], xyz[1], xyz[2]);
		ok = text_take(&t, out);
	}
	return ok;
}

/* The sections that `s` gives, or the ones of its IFF `iff` when it isn't
 * -1. Returns false when out of memory. */
static bool read_layer(const Models* m, const Source* s, const char* where, int iff, Layer* out) {
	memset(out, 0, sizeof *out);
	bool ok = true;
	for (int section = 0; ok && section < SECTION_COUNT; ++section) {
		char name[64], label[1200];
		if (iff < 0)
			snprintf(name, sizeof name, "%s", kSections[section]);
		else
			snprintf(name, sizeof name, "%s%d", kSections[section], iff);
		XwauLines lines = { 0 };
		if (!(ok = source_lines(s, name, section == OBJECTS, &lines)))
			break;
		snprintf(label, sizeof label, "%s [%s]", where, name);
		if (lines.count) {
			if (section == OBJECTS)
				ok = objects_section(m, &lines, label, out);
			else if (section == MAP || section == FAM_MAP)
				ok = map_section(m, &lines, label, &out->sections[section]);
			else
				ok = camera_section(&lines, section == FAM_CAMERA, &out->sections[section]);
		}
		XwauLines_Free(&lines);
	}
	return ok;
}

/* ---- Ships, crafts and missions ---------------------------------------------- */

/* The model names, as the first file has them on disk, of every
 * FlightModels .ini but default.ini, of every model with a <Model>Hangar.opt,
 * and of the .txt files of the hangar sections, [Camera] and [Size]. Each
 * entry is "<lower-case name>\t<name>", sorted. */
static bool model_names(const char* game, XwauLines* out) {
	static const char* const suffixes[] = { "hangarobjects",   "hangarmap", "hangarcamera", "famhangarmap",
											"famhangarcamera", "camera",    "size" };
	memset(out, 0, sizeof *out);
	char dir[1100];
	if (!Xwau_FindDir(game, "FlightModels", dir, sizeof dir))
		return true;
	size_t count   = 0;
	char** entries = ListDir(dir, true, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		const char*  entry = entries[i];
		const size_t len   = strlen(entry);
		char         lower[1024];
		LowerCopy(lower, sizeof lower, entry);
		/* The lengths of the names that this file gives. */
		size_t names[8];
		int    n = 0;
		if (EndsWithNoCase(lower, ".ini") && strcmp(lower, "default.ini") != 0) {
			names[n++] = len - 4;
		} else if (EndsWithNoCase(lower, "hangar.opt") && len > 10) {
			char rel[1100];
			if (PathFormat(rel, sizeof rel, "FlightModels/%.*s.opt", (int)(len - 10), entry) &&
				has_file(game, rel))
				names[n++] = len - 10;
		} else if (EndsWithNoCase(lower, ".txt")) {
			for (size_t s = 0; s < sizeof suffixes / sizeof suffixes[0]; ++s) {
				/* The shortest name that the suffix, digits and ".txt"
				   follow. */
				const size_t suffix_len = strlen(suffixes[s]);
				for (size_t p = 1; p + suffix_len + 4 <= len; ++p) {
					if (strncmp(lower + p, suffixes[s], suffix_len) != 0)
						continue;
					const char* c = lower + p + suffix_len;
					while (*c >= '0' && *c <= '9')
						++c;
					if (strcmp(c, ".txt") == 0) {
						names[n++] = p;
						break;
					}
				}
			}
		}
		for (int k = 0; ok && k < n; ++k) {
			bool known = false;
			for (size_t j = 0; !known && j < out->count; ++j)
				known = strncmp(out->items[j], lower, names[k]) == 0 && out->items[j][names[k]] == '\t';
			if (known)
				continue;
			Text t = { 0 };
			text_add(&t, "%.*s\t%.*s", (int)names[k], lower, (int)names[k], entry);
			ok = text_take(&t, out);
		}
	}
	FreeList(entries, count);
	if (ok && out->count)
		qsort(out->items, out->count, sizeof *out->items, CompareStrings);
	return ok;
}

/* A ship's layer, with its IFF layers and its hangar model. Returns false
 * when out of memory. */
static bool ship_layer(const Models* m, const char* game, const char* name, Layer* out) {
	char txt[1100], ini[1100];
	snprintf(txt, sizeof txt, "FlightModels/%s", name);
	snprintf(ini, sizeof ini, "FlightModels/%s.ini", name);
	Source s;
	source_init(&s, game, txt, ini, "");
	if (!read_layer(m, &s, name, -1, out))
		return false;
	bool iffs[256] = { false };
	for (int section = 0; section < SECTION_COUNT; ++section)
		source_iffs(&s, kSections[section], iffs);
	for (int iff = 0; iff < 256; ++iff) {
		if (!iffs[iff])
			continue;
		Layer* variant = (Layer*)calloc(1, sizeof *variant);
		if (!variant)
			return false;
		const bool read  = read_layer(m, &s, name, iff, variant);
		IffLayer*  entry = read && !layer_empty(variant) ? ARRAY_PUSH(out->iffs, out->iff_count) : NULL;
		if (entry) {
			entry->iff   = iff;
			entry->layer = variant;
			continue;
		}
		const bool empty = read && layer_empty(variant);
		layer_free(variant);
		free(variant);
		if (!empty)
			return false;
	}
	char lower[256], hangar[300];
	LowerCopy(lower, sizeof lower, name);
	snprintf(hangar, sizeof hangar, "%s%s", lower, strcmp(lower, "familybase") == 0 ? "" : "hangar");
	char opt[400];
	if (converted(m, hangar))
		snprintf(out->model, sizeof out->model, "%s", hangar);
	else if (PathFormat(opt, sizeof opt, "FlightModels/%s.opt", hangar) && has_file(game, opt))
		ReportFatal(LABEL, "%s: the hangar model %s.opt was not converted", name, hangar);
	return true;
}

/* A player craft's [Camera] and [Size], as "{ camera: ..., elevation: ... }",
 * or "" when it has neither. Returns false when out of memory. */
static bool craft_record(const char* game, const char* name, Text* out) {
	char      rel[1100];
	XwauLines camera = { 0 }, size = { 0 };
	bool      ok =
		PathFormat(rel, sizeof rel, "FlightModels/%sCamera.txt", name) && read_found(game, rel, "", &camera);
	if (ok && !camera.count)
		ok = PathFormat(rel, sizeof rel, "FlightModels/%s.ini", name) &&
			 read_found(game, rel, "Camera", &camera);
	ok = ok && PathFormat(rel, sizeof rel, "FlightModels/%sSize.txt", name) &&
		 read_found(game, rel, "", &size);
	if (ok && !size.count)
		ok = PathFormat(rel, sizeof rel, "FlightModels/%s.ini", name) && read_found(game, rel, "Size", &size);
	if (ok && (camera.count || size.count)) {
		text_add(out, "{ ");
		if (camera.count) {
			/* Each triple is the first of its prefixes that isn't all zero,
			   or the last one. */
			static const char* const prefixes[4][4] = {
				{ "" }, { "Inverted", "" }, { "Fam", "" }, { "FamInverted", "Fam", "Inverted", "" }
			};
			text_add(out, "camera: [");
			for (int t = 0; t < 4; ++t) {
				long long xyz[3] = { 0, 0, 0 };
				for (int p = 0; p < 4 && (p == 0 || prefixes[t][p]); ++p) {
					for (int a = 0; a < 3; ++a) {
						char key[64];
						snprintf(key, sizeof key, "%s%c", prefixes[t][p], "XYZ"[a]);
						xyz[a] = int_value(&camera, key);
					}
					if (xyz[0] || xyz[1] || xyz[2])
						break;
				}
				text_add(out, "%s[%lld, %lld, %lld]", t ? ", " : "", xyz[0], xyz[1], xyz[2]);
			}
			text_add(out, "]");
		}
		if (size.count) {
			const long long elevation = int_value(&size, "ClosedSFoilsElevation");
			long long       inverted  = int_value(&size, "ClosedSFoilsElevationInverted");
			if (!inverted)
				inverted = elevation;
			text_add(out, "%selevation: [%lld, %lld]", camera.count ? ", " : "", elevation, inverted);
		}
		text_add(out, " }");
	}
	XwauLines_Free(&camera);
	XwauLines_Free(&size);
	return ok && !out->failed;
}

/* A mission's own layer, from <mission>_<Section>.txt and [<Section>] of
 * <mission>.ini. Returns false when out of memory. */
static bool mission_layer(const Models* m, const char* game, const char* base, Layer* out) {
	char ini[1100];
	snprintf(ini, sizeof ini, "%s.ini", base);
	Source s;
	source_init(&s, game, base, ini, "_");
	if (!read_layer(m, &s, base, -1, out))
		return false;
	size_t size = 0;
	char*  text = s.ini[0] ? ReadWholeFile(s.ini, &size) : NULL;
	for (char *line = text, *next; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		Xwau_Trim(line, XWAU_RULES_CPP);
		const size_t len = strlen(line);
		for (int section = 0; section < SECTION_COUNT; ++section) {
			const size_t section_len = strlen(kSections[section]);
			if (len >= section_len + 4 && line[0] == '[' && line[len - 1] == ']' &&
				strncasecmp(line + 1, kSections[section], section_len) == 0 && line[1 + section_len] == '_')
				ReportUnsupported("the per-ship mission hangar section [%s_<ship>]", kSections[section]);
		}
	}
	free(text);
	char rel[1100];
	if (PathFormat(rel, sizeof rel, "%s_Hangar.opt", base) && has_file(game, rel))
		ReportUnsupported("a mission's own hangar model, <mission>_Hangar.opt");
	return true;
}

/* The ships of `ships` that a mission's [Objects] swaps replace: the
 * mission's own lines, else FlightModels/Objects.txt, else [Objects] of
 * FlightModels/default.ini, as the hook reads them. Returns false when out
 * of memory. */
static bool mission_ships(const char* game, const char* base, const XwauLines* ships, XwauLines* out) {
	XwauLines lines = { 0 };
	bool      ok    = XwauMissionFiles_ObjectLines(game, base, &lines);
	if (ok && !lines.count)
		ok = read_found(game, "FlightModels/Objects.txt", "", &lines);
	if (ok && !lines.count)
		ok = read_found(game, "FlightModels/default.ini", "Objects", &lines);
	XwauLines seen = { 0 };
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char key[512], value[1024], name[256], substitute[256];
		if (!key_value(lines.items[i], key, sizeof key, value, sizeof value) ||
			!swap_source(key, name, sizeof name))
			continue;
		bool known = false;
		for (size_t j = 0; !known && j < seen.count; ++j)
			known = Xwau_SameName(seen.items[j], key);
		if (known || !(ok = XwauLines_Add(&seen, key, strlen(key))))
			continue;
		if (!opt_target(value, substitute, sizeof substitute) || !has_file(game, value) ||
			strcmp(name, substitute) == 0)
			continue;
		bool listed = false;
		for (size_t j = 0; !listed && j < ships->count; ++j)
			listed = strcmp(ships->items[j], name) == 0 || strcmp(ships->items[j], substitute) == 0;
		if (listed) {
			Text t = { 0 };
			text_add(&t, "%s: %s", name, substitute);
			ok = text_take(&t, out);
		}
	}
	if (ok && out->count)
		qsort(out->items, out->count, sizeof *out->items, CompareStrings);
	XwauLines_Free(&seen);
	XwauLines_Free(&lines);
	return ok;
}

/* ---- Writing --------------------------------------------------------------- */

static void write_lines(FILE* f, int indent, const char* key, const XwauLines* lines, const char* item) {
	if (!lines->count)
		return;
	fprintf(f, "%*s%s:\n", indent, "", key);
	for (size_t i = 0; i < lines->count; ++i)
		fprintf(f, "%*s  %s%s\n", indent, "", item, lines->items[i]);
}

static void write_layer(FILE* f, const Layer* l, int indent) {
	if (l->model[0])
		fprintf(f, "%*smodel: %s\n", indent, "", l->model);
	write_lines(f, indent, "objects", &l->sections[OBJECTS], "");
	write_lines(f, indent, "swaps", &l->swaps, "");
	write_lines(f, indent, "map", &l->sections[MAP], "- ");
	write_lines(f, indent, "fam_map", &l->sections[FAM_MAP], "- ");
	write_lines(f, indent, "camera", &l->sections[CAMERA], "");
	write_lines(f, indent, "fam_camera", &l->sections[FAM_CAMERA], "");
	for (size_t i = 0; i < l->iff_count; ++i) {
		if (i == 0)
			fprintf(f, "%*siff:\n", indent, "");
		fprintf(f, "%*s  %d:\n", indent, "", l->iffs[i].iff);
		write_layer(f, l->iffs[i].layer, indent + 4);
	}
}

/* Replaces `path` with `temporary`. */
static bool replace_file(const char* temporary, const char* path) {
#ifdef _WIN32
	/* Windows' rename doesn't replace an existing file. */
	remove(path);
#endif
	if (rename(temporary, path) == 0)
		return true;
	Report(path, "cannot be written");
	remove(temporary);
	return false;
}

/* Makes `hangar` the `hangar` of the mission file `path`, in place of the
 * one it has, and creates the file when it doesn't exist and the layer isn't
 * empty. Sets *written when the file has a layer. Returns false when the
 * file can't be written. */
static bool write_mission_layer(const char* path, const char* base, const Layer* hangar, bool* written) {
	*written         = false;
	const bool empty = layer_empty(hangar);
	size_t     size  = 0;
	char*      text  = ReadWholeFile(path, &size);
	if (!text && empty)
		return true;
	/* The lines before and after the old `hangar`, with their line ends. */
	Text kept     = { 0 };
	bool skipping = false;
	for (const char *line = text, *next; line && line < text + size; line = next) {
		const char* nl = memchr(line, '\n', size - (size_t)(line - text));
		next           = nl ? nl + 1 : text + size;
		skipping       = strncmp(line, "hangar:", 7) == 0 || (skipping && line[0] == ' ');
		if (!skipping)
			text_add(&kept, "%.*s", (int)(next - line), line);
	}
	free(text);
	if (!kept.len)
		text_add(&kept, "# Made by the OpenXWA mod importer.\n");
	char        dir[1100], temporary[1100];
	const char* slash = strrchr(path, '/');
	bool        ok    = !kept.failed && PathFormat(dir, sizeof dir, "%.*s", (int)(slash - path), path) &&
						MakeDirs(dir) && PathFormat(temporary, sizeof temporary, "%s.tmp", path);
	FILE*       f     = ok ? fopen(temporary, "wb") : NULL;
	if (f) {
		fwrite(kept.s, 1, kept.len, f);
		if (!empty) {
			fprintf(f, "hangar:\n");
			write_layer(f, hangar, 2);
			if (hangar->ships.count) {
				fprintf(f, "  ships:\n");
				for (size_t i = 0; i < hangar->ships.count; ++i)
					fprintf(f, "    %s\n", hangar->ships.items[i]);
			}
		}
		ok = fclose(f) == 0 && replace_file(temporary, path);
	} else {
		Report(path, "cannot be written");
		ok = false;
	}
	free(kept.s);
	*written = ok && !empty;
	return ok;
}

/* Writes the missions' own layers into their mission files, and sets *count
 * to the number of files with a layer. */
static bool write_mission_layers(const Models* m, const char* game, const char* stock_game,
								 const XwauLines* ships, const char* mod, size_t* count) {
	*count  = 0;
	bool ok = true;
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names = { 0 };
		ok              = XwauMissions_Names(game, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n) {
			char  base[1100], tie[1200], path[1200], lower_dir[64], found[1200];
			Layer hangar = { 0 };
			snprintf(base, sizeof base, "%s/%s", kMissionDirs[d], names.items[n]);
			ok = mission_layer(m, game, base, &hangar) && mission_ships(game, base, ships, &hangar.ships);
			snprintf(tie, sizeof tie, "%s.tie", base);
			if (ok && !layer_empty(&hangar) && !Xwau_FindFile(game, tie, found, sizeof found) &&
				!Xwau_FindFile(stock_game, tie, found, sizeof found)) {
				/* Neither the mod nor the game has the mission. */
				layer_free(&hangar);
			}
			LowerCopy(lower_dir, sizeof lower_dir, kMissionDirs[d]);
			bool written = false;
			ok           = ok &&
						   PathFormat(path, sizeof path, "%s/flight/missions/%s/%s.yaml", mod, lower_dir,
									  names.items[n]) &&
						   write_mission_layer(path, base, &hangar, &written);
			*count += written;
			layer_free(&hangar);
		}
		XwauLines_Free(&names);
	}
	return ok;
}

bool XwauHangars_Write(const char* xwau, const XwauGameTables* stock, const XwauNames* game_names,
					   const char* stock_game, const char* mod) {
	Models* m = (Models*)malloc(sizeof *m);
	if (!m || !models_load(m, xwau, stock, game_names, mod)) {
		if (m)
			models_free(m);
		free(m);
		return false;
	}

	Layer  def = { 0 };
	Source s;
	bool   ok = true;
	source_init(&s, xwau, "FlightModels/", "FlightModels/default.ini", "");
	ok = read_layer(m, &s, "default", -1, &def);

	/* The ships and crafts, by lower-case name. */
	XwauLines names = { 0 }, ship_keys = { 0 }, crafts = { 0 };
	Layer*    ships      = NULL;
	size_t    ship_count = 0;
	ok                   = ok && model_names(xwau, &names);
	for (size_t i = 0; ok && i < names.count; ++i) {
		char*       tab  = strchr(names.items[i], '\t');
		const char* name = tab + 1;
		*tab             = 0;
		const char* key  = names.items[i];
		Layer       ship = { 0 };
		bool        kept = false;
		ok               = ship_layer(m, xwau, name, &ship);
		if (ok && !layer_empty(&ship)) {
			/* The ship's layer moves into the array. */
			Layer* entry = ARRAY_PUSH(ships, ship_count);
			ok           = entry && XwauLines_Add(&ship_keys, key, strlen(key));
			if (ok) {
				*entry = ship;
				kept   = true;
			}
		}
		if (!kept)
			layer_free(&ship);
		Text craft = { 0 };
		ok         = ok && craft_record(xwau, name, &craft);
		if (ok && craft.len) {
			Text line = { 0 };
			text_add(&line, "%s: %s", key, craft.s);
			ok = text_take(&line, &crafts);
		}
		free(craft.s);
	}

	char path[1100], temporary[1100], dir[1100];
	ok      = ok && PathFormat(dir, sizeof dir, "%s/flight", mod) && MakeDirs(dir) &&
			  PathFormat(path, sizeof path, "%s/hangars.yaml", dir) &&
			  PathFormat(temporary, sizeof temporary, "%s.tmp", path);
	FILE* f = ok ? fopen(temporary, "wb") : NULL;
	if (ok && !f) {
		Report(path, "cannot be written");
		ok = false;
	}
	size_t models = 0;
	if (f) {
		fputs("# Made by the OpenXWA mod importer.\n", f);
		if (!layer_empty(&def)) {
			fprintf(f, "default:\n");
			write_layer(f, &def, 2);
		}
		if (ship_count)
			fprintf(f, "ships:\n");
		for (size_t i = 0; i < ship_count; ++i) {
			fprintf(f, "  %s:\n", ship_keys.items[i]);
			write_layer(f, &ships[i], 4);
			models += ships[i].model[0] != 0;
		}
		if (crafts.count)
			fprintf(f, "crafts:\n");
		for (size_t i = 0; i < crafts.count; ++i)
			fprintf(f, "  %s\n", crafts.items[i]);
		ok = fclose(f) == 0 && replace_file(temporary, path);
	}
	if (ok)
		Report(NULL, "wrote %s: %zu ships (%zu hangar models), %zu crafts", path, ship_count, models,
			   crafts.count);

	if (ok && stock_game) {
		size_t count = 0;
		ok           = write_mission_layers(m, xwau, stock_game, &ship_keys, mod, &count);
		if (ok)
			Report(NULL, "wrote %zu mission hangar layers", count);
	}

	layer_free(&def);
	for (size_t i = 0; i < ship_count; ++i)
		layer_free(&ships[i]);
	free(ships);
	XwauLines_Free(&names);
	XwauLines_Free(&ship_keys);
	XwauLines_Free(&crafts);
	models_free(m);
	free(m);
	return ok;
}
