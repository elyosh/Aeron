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

/* The sections of a layer, as the hook names them. */
enum { OBJECTS, MAP, CAMERA, FAM_MAP, FAM_CAMERA, SECTION_COUNT };

static const char* const kSections[SECTION_COUNT] = { "HangarObjects", "HangarMap", "HangarCamera",
													  "FamHangarMap", "FamHangarCamera" };

static const int kCameraKeys[]    = { 1, 2, 3, 6, 9 };
static const int kFamCameraKeys[] = { 1, 2, 3, 6, 7, 8, 9 };

/* The line of each map section that holds the crane arm. */
static const int kCraneLine[2] = { 1, 2 };

typedef enum KeyKind {
	KEY_INT,
	KEY_TYPE,
	KEY_SHUTTLE_TYPE, /* a model index, or 0 for the stock shuttle */
	KEY_STRING,
	KEY_INT_LIST,
	KEY_TYPE_LIST,
	KEY_NOT_IMPORTED
} KeyKind;

/* The HangarObjects keys. The keys of the two droids follow K_LOAD_DROID1
   in the same order. */
typedef enum Key {
	K_LOAD_SHUTTLE,
	K_SHUTTLE_MODEL_INDEX,
	K_SHUTTLE_MARKINGS,
	K_SHUTTLE_OBJECT_PROFILE,
	K_SHUTTLE_POSITION_X,
	K_SHUTTLE_POSITION_Y,
	K_SHUTTLE_POSITION_Z,
	K_SHUTTLE_ORIENTATION,
	K_IS_SHUTTLE_FLOOR_INVERTED,
	K_SHUTTLE_ANIMATION,
	K_SHUTTLE_ANIMATION_STRAIGHT_LINE,
	K_SHUTTLE_ANIMATION_ELEVATION,
	K_LOAD_DROIDS,
	K_DROIDS_POSITION_Z,
	K_IS_DROIDS_FLOOR_INVERTED,
	K_LOAD_DROID1,
	K_ROOF_CRANE_POSITION_X = K_LOAD_DROID1 + 14,
	K_ROOF_CRANE_POSITION_Y,
	K_ROOF_CRANE_POSITION_Z,
	K_ROOF_CRANE_AXIS,
	K_ROOF_CRANE_LOW_OFFSET,
	K_ROOF_CRANE_HIGH_OFFSET,
	K_IS_HANGAR_FLOOR_INVERTED,
	K_HANGAR_FLOOR_INVERTED_HEIGHT,
	K_HANGAR_IFF,
	K_PLAYER_ANIMATION_ELEVATION,
	K_PLAYER_ANIMATION_INVERTED_ELEVATION,
	K_PLAYER_ANIMATION_STRAIGHT_LINE,
	K_PLAYER_OFFSET_X,
	K_PLAYER_OFFSET_Y,
	K_PLAYER_OFFSET_Z,
	K_PLAYER_INVERTED_OFFSET_X,
	K_PLAYER_INVERTED_OFFSET_Y,
	K_PLAYER_INVERTED_OFFSET_Z,
	K_IS_PLAYER_FLOOR_INVERTED,
	K_PLAYER_MODEL_INDICES,
	K_PLAYER_OFFSETS_X,
	K_PLAYER_OFFSETS_Y,
	K_PLAYER_OFFSETS_Z,
	K_PLAYER_FLOOR_INVERTED_MODEL_INDICES,
	K_DRAW_SHADOWS,
	K_LIGHT_COLOR_INTENSITY,
	K_LIGHT_COLOR_RGB,
	KEY_COUNT
} Key;

/* The keys of a droid. The first droid's keys start at K_LOAD_DROID1, and
   the second droid's keys start 7 keys later. */
enum {
	DK_LOAD,
	DK_POSITION_Z,
	DK_IS_FLOOR_INVERTED,
	DK_UPDATE,
	DK_MODEL_INDEX,
	DK_MARKINGS,
	DK_OBJECT_PROFILE,
	DROID_KEYS
};

static const struct {
	const char* name;
	KeyKind     kind;
} kKeys[] = {
	{ "LoadShuttle", KEY_INT },
	{ "ShuttleModelIndex", KEY_SHUTTLE_TYPE },
	{ "ShuttleMarkings", KEY_INT },
	{ "ShuttleObjectProfile", KEY_STRING },
	{ "ShuttlePositionX", KEY_INT },
	{ "ShuttlePositionY", KEY_INT },
	{ "ShuttlePositionZ", KEY_INT },
	{ "ShuttleOrientation", KEY_INT },
	{ "IsShuttleFloorInverted", KEY_INT },
	{ "ShuttleAnimation", KEY_STRING },
	{ "ShuttleAnimationStraightLine", KEY_INT },
	{ "ShuttleAnimationElevation", KEY_INT },
	{ "LoadDroids", KEY_INT },
	{ "DroidsPositionZ", KEY_INT },
	{ "IsDroidsFloorInverted", KEY_INT },
	{ "LoadDroid1", KEY_INT },
	{ "Droid1PositionZ", KEY_INT },
	{ "IsDroid1FloorInverted", KEY_INT },
	{ "Droid1Update", KEY_INT },
	{ "Droid1ModelIndex", KEY_TYPE },
	{ "Droid1Markings", KEY_INT },
	{ "Droid1ObjectProfile", KEY_STRING },
	{ "LoadDroid2", KEY_INT },
	{ "Droid2PositionZ", KEY_INT },
	{ "IsDroid2FloorInverted", KEY_INT },
	{ "Droid2Update", KEY_INT },
	{ "Droid2ModelIndex", KEY_TYPE },
	{ "Droid2Markings", KEY_INT },
	{ "Droid2ObjectProfile", KEY_STRING },
	{ "HangarRoofCranePositionX", KEY_INT },
	{ "HangarRoofCranePositionY", KEY_INT },
	{ "HangarRoofCranePositionZ", KEY_INT },
	{ "HangarRoofCraneAxis", KEY_INT },
	{ "HangarRoofCraneLowOffset", KEY_INT },
	{ "HangarRoofCraneHighOffset", KEY_INT },
	{ "IsHangarFloorInverted", KEY_INT },
	{ "HangarFloorInvertedHeight", KEY_INT },
	{ "HangarIff", KEY_INT },
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
	{ "PlayerModelIndices", KEY_TYPE_LIST },
	{ "PlayerOffsetsX", KEY_INT_LIST },
	{ "PlayerOffsetsY", KEY_INT_LIST },
	{ "PlayerOffsetsZ", KEY_INT_LIST },
	{ "PlayerFloorInvertedModelIndices", KEY_TYPE_LIST },
	{ "DrawShadows", KEY_NOT_IMPORTED },
	{ "LightColorIntensity", KEY_NOT_IMPORTED },
	{ "LightColorRgb", KEY_NOT_IMPORTED },
};

typedef char kKeysMatchTheEnum[sizeof kKeys / sizeof kKeys[0] == KEY_COUNT ? 1 : -1];
typedef char kDroidKeysFit[K_LOAD_DROID1 + 2 * DROID_KEYS == K_ROOF_CRANE_POSITION_X ? 1 : -1];

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
	const char*           game;
	const XwauNames*      names;
	char**                glbs; /* the files in <mod>/flight/models */
	size_t                glb_count;
	const XwauGameTables* stock;
	XwauGameTables*       xwau;
	/* The model that OpenXWA loads for each object type. This is the `model`
	   of its object type file, or "" when it loads the model of its stock
	   row. */
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

/* Whether row `index` of XWAU's object list gives a model file. */
static bool xwau_has_model(const Models* m, long long index) {
	return index >= 0 && index < XWAU_GAME_OBJECT_TYPES && XwauGameTables_RowModel(m->xwau, (int)index)[0];
}

/* Formats "model index <n> (<XWAU's model>)" for the reports. */
static const char* describe(const Models* m, long long index, char* out, size_t cap) {
	snprintf(out, cap, "model index %lld (%s)", index,
			 index >= 0 && index < XWAU_GAME_OBJECT_TYPES ? XwauGameTables_RowModel(m->xwau, (int)index)
														  : "");
	return out;
}

/* Returns the OpenXWA object type for XWAU's model index `index`, or -1 when
 * that row of XWAU's object list gives no model file. When the row gives a
 * model that no OpenXWA type loads, the import fails. */
static int import_type(const Models* m, long long index, const char* where, const char* what) {
	char      text[300];
	const int type = object_type(m, index & 0xFFFF);
	if (type < 0 && xwau_has_model(m, index & 0xFFFF))
		ReportFatal(LABEL, "%s: %s: %s has no OpenXWA type", where, what,
					describe(m, index & 0xFFFF, text, sizeof text));
	return type;
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
	m->game  = game;
	m->names = game_names;
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

/* A layer reads its settings from the files
 * <txt_base><separator><Section>.txt and from the sections [<Section>] of
 * the .ini `ini`. */
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

/* Gets the lines of `section`. With `append`, these are the lines of the
 * .txt file followed by the lines of the .ini section. Otherwise they are the
 * lines of the .txt file, or of the .ini section when the .txt file has
 * none. Returns false when out of memory. */
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

/* Marks in `found` the IFF numbers that `s` has for `section`. They come
 * from the .ini headers [<Section><N>] and from the files
 * <base><Section><N>.txt. */
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

/* The value of a HangarObjects key in one layer. */
typedef struct RawValue {
	bool set;
	/* The line has no value. The key hides the lower layers' value, and its
	   default applies. */
	bool       empty;
	long long  number;    /* the object type for KEY_TYPE and KEY_SHUTTLE_TYPE, 0 for the stock shuttle */
	char       text[256]; /* KEY_STRING */
	long long* list;      /* the object types for KEY_TYPE_LIST, -1 for a row without a model file */
	size_t     count;
} RawValue;

/* A map line. A line that the hook skips places nothing. */
typedef struct MapLine {
	bool      placed;
	int       type;
	long long markings, x, y, z, heading, pitch;
	char      profile[256];
	int       inverted; /* -1 to use the hangar floor's side */
} MapLine;

typedef struct Layout {
	bool     present;
	MapLine* lines;
	size_t   count;
} Layout;

typedef struct Layer Layer;

typedef struct IffLayer {
	int    iff;
	Layer* layer;
} IffLayer;

/* The settings of one source. */
struct Layer {
	char      where[300];
	char      model[300];
	RawValue  values[KEY_COUNT];
	Layout    layouts[2];
	bool      camera_present[2];
	long long cameras[2][10][3];
	XwauLines swaps; /* "<model>: \"<model>\"" */
	XwauLines ships; /* for a mission, sorted "<ship>: <ship>" lines */
	IffLayer* iffs;
	size_t    iff_count;
};

static bool layer_empty(const Layer* l) {
	bool empty = !l->model[0] && !l->swaps.count && !l->ships.count && !l->iff_count &&
				 !l->layouts[0].present && !l->layouts[1].present && !l->camera_present[0] &&
				 !l->camera_present[1];
	for (int k = 0; empty && k < KEY_COUNT; ++k)
		empty = !l->values[k].set;
	return empty;
}

static void layer_free(Layer* l) {
	for (int k = 0; k < KEY_COUNT; ++k)
		free(l->values[k].list);
	free(l->layouts[0].lines);
	free(l->layouts[1].lines);
	XwauLines_Free(&l->swaps);
	XwauLines_Free(&l->ships);
	for (size_t i = 0; i < l->iff_count; ++i) {
		layer_free(l->iffs[i].layer);
		free(l->iffs[i].layer);
	}
	free(l->iffs);
	memset(l, 0, sizeof *l);
}

/* Splits a line "<key> = <value>" into its trimmed key and value. Returns
 * false when the line has no '=' or the key is empty. */
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

/* Returns the number of `key` in `lines`, like GetFileKeyValueInt with a
 * default of 0. Returns 0 when the key is missing or its value isn't a
 * number. */
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

/* A HangarObjects swap line. Adds "<source>: \"<target>\"" to `swaps`. Returns
 * false when out of memory. */
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

/* Reads a list key's numbers into `out`. Model indices become object types,
 * or -1 for the ones that load no model in XWAU either. Returns false when
 * out of memory, and leaves `out` unset when a field isn't a number, since
 * XWAU fails on it. */
static bool read_list(const Models* m, int key, const char* value, const char* where, RawValue* out) {
	XwauLines tokens = { 0 };
	if (!Xwau_Tokenize(value, XWAU_RULES_CPP, &tokens))
		return false;
	long long* numbers = (long long*)calloc(tokens.count + 1, sizeof *numbers);
	if (!numbers) {
		XwauLines_Free(&tokens);
		return false;
	}
	bool numeric = true;
	for (size_t i = 0; numeric && i < tokens.count; ++i)
		numeric = Xwau_StoiAutoInt(tokens.items[i], &numbers[i]);
	if (numeric) {
		if (kKeys[key].kind == KEY_TYPE_LIST)
			for (size_t i = 0; i < tokens.count; ++i)
				numbers[i] = import_type(m, numbers[i], where, kKeys[key].name);
		out->set   = true;
		out->list  = numbers;
		out->count = tokens.count;
	} else {
		free(numbers);
	}
	XwauLines_Free(&tokens);
	return true;
}

/* Reads the HangarObjects lines. The first line of each key wins. Returns
 * false when out of memory. */
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
		for (int j = 0; k < 0 && j < KEY_COUNT; ++j)
			if (strcasecmp(kKeys[j].name, key) == 0)
				k = j;
		/* The hook reads no other key. */
		if (k < 0)
			continue;
		RawValue*     raw  = &out->values[k];
		const KeyKind kind = kKeys[k].kind;
		/* The remaster lights the hangar and draws its shadows itself. */
		if (kind == KEY_NOT_IMPORTED) {
			ReportUnsupported("the HangarObjects key %s", kKeys[k].name);
		} else if (kind == KEY_STRING) {
			raw->set   = true;
			raw->empty = !value[0];
			snprintf(raw->text, sizeof raw->text, "%s", value);
		} else if (!value[0]) {
			raw->set   = true;
			raw->empty = true;
		} else if (kind == KEY_INT_LIST || kind == KEY_TYPE_LIST) {
			ok = read_list(m, k, value, where, raw);
		} else if (Xwau_StoiAutoInt(value, &raw->number)) {
			/* XWAU fails on a value that isn't a number, so such a key is
			   left out. */
			raw->set = true;
			if (kind == KEY_SHUTTLE_TYPE && raw->number == 0)
				continue;
			if (kind == KEY_TYPE || kind == KEY_SHUTTLE_TYPE) {
				raw->number = import_type(m, raw->number, where, kKeys[k].name);
				/* A row without a model file is left out. */
				raw->set = raw->number >= 0;
			}
		}
	}
	XwauLines_Free(&seen);
	return ok;
}

/* The HangarMap lines. Returns false when out of memory. */
static bool map_section(const Models* m, const XwauLines* lines, const char* where, Layout* out) {
	out->present = true;
	out->lines   = (MapLine*)calloc(lines->count + 1, sizeof *out->lines);
	if (!out->lines)
		return false;
	out->count = lines->count;
	bool ok    = true;
	for (size_t i = 0; ok && i < lines->count; ++i) {
		const char* line   = lines->items[i];
		MapLine*    entry  = &out->lines[i];
		XwauLines   fields = { 0 };
		if (!(ok = Xwau_Tokenize(line, XWAU_RULES_CPP, &fields)))
			break;
		const size_t n = fields.count;
		/* XWAU skips a line with another number of fields, and fails on a
		   field that isn't a number. */
		if (n >= 6 && n <= 9) {
			/* The numbers are the first 6 or 7 fields, and the 9th field. */
			long long numbers[8];
			size_t    count   = n >= 7 ? 7 : 6;
			bool      numeric = true;
			for (size_t f = 0; numeric && f < count; ++f)
				numeric = Xwau_StoiAutoInt(fields.items[f], &numbers[f]);
			if (numeric && n == 9)
				numeric = Xwau_StoiAutoInt(fields.items[8], &numbers[count++]);
			if (numeric) {
				/* A line of 6 fields has no markings. */
				const size_t first = n == 6 ? 1 : 2;
				char         what[1100];
				snprintf(what, sizeof what, "line '%s'", line);
				entry->type     = import_type(m, numbers[0], where, what);
				entry->placed   = entry->type >= 0;
				entry->markings = n == 6 ? 0 : numbers[1];
				entry->x        = numbers[first];
				entry->y        = numbers[first + 1];
				entry->z        = numbers[first + 2];
				entry->heading  = numbers[first + 3];
				entry->pitch    = numbers[first + 4];
				snprintf(entry->profile, sizeof entry->profile, "%s", n >= 8 ? fields.items[7] : "Default");
				entry->inverted = n == 9 ? numbers[7] != 0 : -1;
			}
		}
		XwauLines_Free(&fields);
	}
	return ok;
}

/* Reads a camera section, with every key that the hook reads. */
static void camera_section(const XwauLines* lines, bool family, Layer* out) {
	const int*   keys  = family ? kFamCameraKeys : kCameraKeys;
	const size_t count = family ? sizeof kFamCameraKeys / sizeof kFamCameraKeys[0]
								: sizeof kCameraKeys / sizeof kCameraKeys[0];
	out->camera_present[family] = true;
	for (size_t i = 0; i < count; ++i) {
		for (int a = 0; a < 3; ++a) {
			char key[64];
			snprintf(key, sizeof key, "%sKey%d_%c", family ? "Fam" : "", keys[i], "XYZ"[a]);
			out->cameras[family][keys[i]][a] = int_value(lines, key);
		}
	}
}

/* Reads the sections of `s`, or the sections of its IFF `iff` when `iff`
 * isn't -1. Returns false when out of memory. */
static bool read_layer(const Models* m, const Source* s, const char* where, int iff, Layer* out) {
	memset(out, 0, sizeof *out);
	if (iff < 0)
		snprintf(out->where, sizeof out->where, "%s", where);
	else
		snprintf(out->where, sizeof out->where, "%s IFF %d", where, iff);
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
				ok = map_section(m, &lines, label, &out->layouts[section == FAM_MAP]);
			else
				camera_section(&lines, section == FAM_CAMERA, out);
		}
		XwauLines_Free(&lines);
	}
	return ok;
}

/* ---- Ships, crafts and missions ---------------------------------------------- */

/* Lists the model names. They come from every FlightModels .ini except
 * default.ini, from every model with a <Model>Hangar.opt, and from the .txt
 * files of the hangar sections, [Camera] and [Size]. A name is spelled as in
 * the first file that gives it. The entries are "<lower-case name>\t<name>",
 * sorted. */
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
				/* Find the shortest name that is followed by the suffix,
				   digits and ".txt". */
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

/* Reads a ship's layer, with its IFF layers and its hangar model. Returns
 * false when out of memory. */
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

/* A player craft's [Camera] and [Size]. */
typedef struct Craft {
	char      key[256];
	bool      has_size;
	long long rest[2]; /* on the normal and the inverted floor */
	bool      has_camera;
	long long camera[4][3];
} Craft;

/* Reads a player craft's [Camera] and [Size] into `out`. Returns false when
 * out of memory. */
static bool craft_record(const char* game, const char* name, Craft* out) {
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
	out->has_size   = ok && size.count;
	out->has_camera = ok && camera.count;
	if (out->has_size) {
		out->rest[0] = int_value(&size, "ClosedSFoilsElevation");
		out->rest[1] = int_value(&size, "ClosedSFoilsElevationInverted");
		if (!out->rest[1])
			out->rest[1] = out->rest[0];
	}
	if (out->has_camera) {
		/* Each X/Y/Z triple comes from the first prefix in its row of
		   `prefixes` whose three values aren't all zero, else from the last prefix. */
		static const char* const prefixes[4][4] = {
			{ "" }, { "Inverted", "" }, { "Fam", "" }, { "FamInverted", "Fam", "Inverted", "" }
		};
		for (int t = 0; t < 4; ++t) {
			long long* xyz = out->camera[t];
			for (int p = 0; p < 4 && (p == 0 || prefixes[t][p]); ++p) {
				for (int a = 0; a < 3; ++a) {
					char key[64];
					snprintf(key, sizeof key, "%s%c", prefixes[t][p], "XYZ"[a]);
					xyz[a] = int_value(&camera, key);
				}
				if (xyz[0] || xyz[1] || xyz[2])
					break;
			}
		}
	}
	XwauLines_Free(&camera);
	XwauLines_Free(&size);
	return ok;
}

static void write_craft(FILE* f, const Craft* craft) {
	static const char* const cameras[4] = { "hangar", "hangar_inverted", "family_base",
											"family_base_inverted" };
	fprintf(f, "  %s:\n", craft->key);
	if (craft->has_size) {
		fputs("    rest_height_m: {normal: ", f);
		Xwau_WriteMetres(f, (int)craft->rest[0]);
		fputs(", inverted: ", f);
		Xwau_WriteMetres(f, (int)craft->rest[1]);
		fputs("}\n", f);
	}
	if (craft->has_camera) {
		fputs("    player_camera_m:\n", f);
		for (int t = 0; t < 4; ++t) {
			fprintf(f, "      %s: ", cameras[t]);
			Xwau_WriteMetres3(f, (int)craft->camera[t][0], (int)craft->camera[t][1],
							  (int)craft->camera[t][2]);
			fputc('\n', f);
		}
	}
}

/* Reads a mission's own layer from <mission>_<Section>.txt and from
 * [<Section>] of <mission>.ini. Returns false when out of memory. */
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

/* Finds the ships of `ships` that a mission's [Objects] swaps replace. Like
 * the hook, it reads the swaps from the mission's own lines, else from
 * FlightModels/Objects.txt, else from [Objects] of FlightModels/default.ini.
 * Returns false when out of memory. */
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

/* The layers below a layer, in the order the game looks them up. */
typedef struct Chain {
	const Layer* layers[3];
	size_t       count;
} Chain;

/* The layers that can be below a layer: one chain for the default, ship and
 * IFF layers, and one for each possible command ship for a mission's
 * layer. */
typedef struct Below {
	const Chain* chains;
	size_t       count;
} Below;

/* The value of `key` in `l`, else in the first layer of `chain` that sets
 * it, or NULL. */
static const RawValue* chain_find(const Layer* l, const Chain* chain, int key) {
	if (l->values[key].set)
		return &l->values[key];
	for (size_t i = 0; i < chain->count; ++i)
		if (chain->layers[i]->values[key].set)
			return &chain->layers[i]->values[key];
	return NULL;
}

/* The number of `key` from `l` and `chain`, or `fallback` when no layer
 * sets it or its line has no value. */
static long long chain_int(const Layer* l, const Chain* chain, int key, long long fallback) {
	const RawValue* v = chain_find(l, chain, key);
	return v && !v->empty ? v->number : fallback;
}

typedef enum DroidValue { DROID_ENABLED, DROID_HEIGHT, DROID_FLOOR } DroidValue;

/* Returns a droid setting that the hook combines from a key for both droids
 * and a key for each droid. The result is the value that the game would
 * find with `l` and `chain`. */
static long long droid_value(const Layer* l, const Chain* chain, int droid, DroidValue what) {
	const int first = K_LOAD_DROID1 + droid * DROID_KEYS;
	switch (what) {
		case DROID_ENABLED:
			return chain_int(l, chain, K_LOAD_DROIDS, 1) == 1 && chain_int(l, chain, first + DK_LOAD, 1) == 1;
		case DROID_HEIGHT: {
			const RawValue* v = chain_find(l, chain, first + DK_POSITION_Z);
			return v && !v->empty ? v->number : chain_int(l, chain, K_DROIDS_POSITION_Z, 0);
		}
		case DROID_FLOOR:
			return chain_int(l, chain, K_IS_DROIDS_FLOOR_INVERTED, 0) != 0 ||
				   chain_int(l, chain, first + DK_IS_FLOOR_INVERTED, 0) != 0;
	}
	return 0;
}

/* Gets the droid setting with each chain of layers that can be below `l`.
 * When the result differs between chains, the import fails, since the file
 * can only hold one value. */
static long long droid_value_below(const Layer* l, const Below* below, int droid, DroidValue what,
								   const char* name) {
	const long long value = droid_value(l, &below->chains[0], droid, what);
	for (size_t i = 1; i < below->count; ++i) {
		if (droid_value(l, &below->chains[i], droid, what) != value) {
			ReportFatal(LABEL,
						"%s: droid %d: %s depends on the command ship's settings, and the import can only "
						"write one value",
						l->where, droid + 1, name);
			break;
		}
	}
	return value;
}

/* A section of the output. Its header is written before its first key. */
typedef struct Section {
	FILE*           f;
	int             indent;
	const char*     name;
	struct Section* parent;
	bool            open;
} Section;

static void section_open(Section* s) {
	if (s->open)
		return;
	if (s->parent)
		section_open(s->parent);
	fprintf(s->f, "%*s%s:\n", s->indent, "", s->name);
	s->open = true;
}

/* Starts the line of `key` in `s`. The caller writes the value and the line
 * end. */
static void section_key(Section* s, const char* key) {
	section_open(s);
	fprintf(s->f, "%*s  %s: ", s->indent, "", key);
}

/* The number of `key` in `l`, or `fallback` when its line has no value. */
static long long layer_int(const Layer* l, int key, long long fallback) {
	return l->values[key].empty ? fallback : l->values[key].number;
}

static void write_floor(FILE* f, long long inverted) { fputs(inverted ? "inverted" : "normal", f); }

static void write_profile(FILE* f, const RawValue* v) { Xwau_WriteString(f, v->empty ? "Default" : v->text); }

static void write_type(FILE* f, const Models* m, long long type) {
	fputs(XwauNames_ObjectType(m->names, (int)type), f);
}

/* Writes the lengths of `keys` as metres, `[a, b]` or `[a, b, c]`, when `l`
 * sets them. A line without a value gives the key's default. A layer that
 * sets only some of them makes the import fail, since the file holds them
 * together. */
static void write_group(Section* s, const Layer* l, const char* name, const int* keys,
						const long long* defaults, int count) {
	int set = 0;
	for (int i = 0; i < count; ++i)
		set += l->values[keys[i]].set;
	if (!set)
		return;
	if (set != count) {
		ReportFatal(LABEL,
					"%s: [HangarObjects] sets only some of %s%s%s and %s, and the import can only write them "
					"together",
					l->where, kKeys[keys[0]].name, count > 2 ? ", " : "",
					count > 2 ? kKeys[keys[1]].name : "", kKeys[keys[count - 1]].name);
		return;
	}
	section_key(s, name);
	fputc('[', s->f);
	for (int i = 0; i < count; ++i) {
		if (i)
			fputs(", ", s->f);
		Xwau_WriteMetres(s->f, (int)layer_int(l, keys[i], defaults[i]));
	}
	fputs("]\n", s->f);
}

/* Writes a length when `l` sets `key`. */
static void write_metres(Section* s, const Layer* l, const char* name, int key, long long fallback) {
	if (!l->values[key].set)
		return;
	section_key(s, name);
	Xwau_WriteMetres(s->f, (int)layer_int(l, key, fallback));
	fputc('\n', s->f);
}

/* Fails the import on a line without a value whose default comes from
 * another setting, which the file can't express. */
static bool check_derived(const Layer* l, int key) {
	if (l->values[key].set && l->values[key].empty) {
		ReportFatal(
			LABEL,
			"%s: [HangarObjects] %s has no value, and the import can't write the default it then takes",
			l->where, kKeys[key].name);
		return false;
	}
	return true;
}

static void write_hangar(FILE* f, const Layer* l, int indent) {
	Section s = { f, indent, "hangar", NULL, false };
	if (l->values[K_IS_HANGAR_FLOOR_INVERTED].set) {
		section_key(&s, "floor");
		write_floor(f, layer_int(l, K_IS_HANGAR_FLOOR_INVERTED, 0) != 0);
		fputc('\n', f);
	}
	write_metres(&s, l, "inverted_floor_height_m", K_HANGAR_FLOOR_INVERTED_HEIGHT, 0);
	if (l->values[K_HANGAR_IFF].set) {
		const long long markings = layer_int(l, K_HANGAR_IFF, -1) & 0xFF;
		section_key(&s, "markings");
		if (markings == 0xFF)
			fputs("command_ship\n", f);
		else
			fprintf(f, "%lld\n", markings);
	}
}

static void write_shuttle(FILE* f, const Models* m, const Layer* l, int indent) {
	static const int       position[]  = { K_SHUTTLE_POSITION_X, K_SHUTTLE_POSITION_Y };
	static const long long positions[] = { 0x467, 0x3BF };
	Section                s           = { f, indent, "shuttle", NULL, false };
	if (l->values[K_LOAD_SHUTTLE].set) {
		section_key(&s, "enabled");
		fputs(layer_int(l, K_LOAD_SHUTTLE, 1) == 1 ? "true\n" : "false\n", f);
	}
	if (l->values[K_SHUTTLE_MODEL_INDEX].set) {
		section_key(&s, "type");
		if (layer_int(l, K_SHUTTLE_MODEL_INDEX, 0) == 0)
			fputs("default", f);
		else
			write_type(f, m, l->values[K_SHUTTLE_MODEL_INDEX].number);
		fputc('\n', f);
	}
	if (l->values[K_SHUTTLE_MARKINGS].set) {
		section_key(&s, "markings");
		fprintf(f, "%lld\n", layer_int(l, K_SHUTTLE_MARKINGS, 0) & 0xFF);
	}
	if (l->values[K_SHUTTLE_OBJECT_PROFILE].set) {
		section_key(&s, "profile");
		write_profile(f, &l->values[K_SHUTTLE_OBJECT_PROFILE]);
		fputc('\n', f);
	}
	write_group(&s, l, "position_m", position, positions, 2);
	write_metres(&s, l, "height_m", K_SHUTTLE_POSITION_Z, 0);
	if (l->values[K_SHUTTLE_ORIENTATION].set) {
		section_key(&s, "heading_degrees");
		Xwau_WriteDegrees(f, (int)(layer_int(l, K_SHUTTLE_ORIENTATION, 0xA880) & 0xFFFF));
		fputc('\n', f);
	}
	if (l->values[K_IS_SHUTTLE_FLOOR_INVERTED].set) {
		section_key(&s, "floor");
		write_floor(f, layer_int(l, K_IS_SHUTTLE_FLOOR_INVERTED, 0) != 0);
		fputc('\n', f);
	}
	Section path = { f, indent + 2, "path", &s, false };
	if (l->values[K_SHUTTLE_ANIMATION].set) {
		const char* side = l->values[K_SHUTTLE_ANIMATION].text;
		section_key(&path, "exit_side");
		fputs(strcasecmp(side, "Top") == 0      ? "top\n"
			  : strcasecmp(side, "Bottom") == 0 ? "bottom\n"
												: "right\n",
			  f);
	}
	write_metres(&path, l, "straight_m", K_SHUTTLE_ANIMATION_STRAIGHT_LINE, 0);
	write_metres(&path, l, "rise_m", K_SHUTTLE_ANIMATION_ELEVATION, 0);
}

/* Writes ", " before every value of a flow map but the first. */
static void flow_key(FILE* f, bool* first, const char* key) {
	fprintf(f, "%s%s: ", *first ? "" : ", ", key);
	*first = false;
}

/* Writes the droids. The hook combines `enabled`, `height_m` and `floor`
 * from a key for both droids and a key for each droid. When a layer sets
 * either key, the import writes the combined value, using the layers below
 * it. */
static void write_droids(FILE* f, const Models* m, const Layer* l, const Below* below, int indent) {
	bool has[2] = { false, false };
	for (int d = 0; d < 2; ++d) {
		const int first = K_LOAD_DROID1 + d * DROID_KEYS;
		has[d]          = l->values[K_LOAD_DROIDS].set || l->values[K_DROIDS_POSITION_Z].set ||
						  l->values[K_IS_DROIDS_FLOOR_INVERTED].set;
		for (int k = 0; k < DROID_KEYS; ++k)
			has[d] = has[d] || l->values[first + k].set;
	}
	if (!has[0] && !has[1])
		return;
	fprintf(f, "%*sdroids:\n", indent, "");
	for (int d = 0; d < (has[1] ? 2 : 1); ++d) {
		const int first = K_LOAD_DROID1 + d * DROID_KEYS;
		bool      start = true;
		fprintf(f, "%*s  - {", indent, "");
		if (l->values[K_LOAD_DROIDS].set || l->values[first + DK_LOAD].set) {
			flow_key(f, &start, "enabled");
			fputs(droid_value_below(l, below, d, DROID_ENABLED, "enabled") ? "true" : "false", f);
		}
		if (l->values[first + DK_MODEL_INDEX].set) {
			flow_key(f, &start, "type");
			write_type(f, m,
					   l->values[first + DK_MODEL_INDEX].empty ? 311 + d
															   : l->values[first + DK_MODEL_INDEX].number);
		}
		if (l->values[first + DK_MARKINGS].set) {
			flow_key(f, &start, "markings");
			fprintf(f, "%lld", layer_int(l, first + DK_MARKINGS, 0) & 0xFF);
		}
		if (l->values[first + DK_OBJECT_PROFILE].set) {
			flow_key(f, &start, "profile");
			write_profile(f, &l->values[first + DK_OBJECT_PROFILE]);
		}
		if (l->values[K_DROIDS_POSITION_Z].set || l->values[first + DK_POSITION_Z].set) {
			flow_key(f, &start, "height_m");
			Xwau_WriteMetres(f, (int)droid_value_below(l, below, d, DROID_HEIGHT, "height_m"));
		}
		if (l->values[first + DK_UPDATE].set) {
			flow_key(f, &start, "moves");
			fputs(layer_int(l, first + DK_UPDATE, 1) != 0 ? "true" : "false", f);
		}
		if (l->values[K_IS_DROIDS_FLOOR_INVERTED].set || l->values[first + DK_IS_FLOOR_INVERTED].set) {
			flow_key(f, &start, "floor");
			write_floor(f, droid_value_below(l, below, d, DROID_FLOOR, "floor"));
		}
		fputs("}\n", f);
	}
}

static void write_roof_crane(FILE* f, const Layer* l, int indent) {
	static const int       position[]  = { K_ROOF_CRANE_POSITION_X, K_ROOF_CRANE_POSITION_Y,
										   K_ROOF_CRANE_POSITION_Z };
	static const long long positions[] = { -1400, 786, -282 };
	static const int       travel[]    = { K_ROOF_CRANE_LOW_OFFSET, K_ROOF_CRANE_HIGH_OFFSET };
	static const long long travels[]   = { 0, 2800 };
	Section                s           = { f, indent, "roof_crane", NULL, false };
	write_group(&s, l, "position_m", position, positions, 3);
	if (l->values[K_ROOF_CRANE_AXIS].set) {
		const long long axis = layer_int(l, K_ROOF_CRANE_AXIS, 0);
		section_key(&s, "axis");
		fputs(axis == 1 ? "y\n" : axis == 2 ? "z\n" : "x\n", f);
	}
	write_group(&s, l, "travel_m", travel, travels, 2);
}

/* Writes the object types of a list key, without the ones that load no
 * model. */
static void write_type_list(FILE* f, const Models* m, const RawValue* v) {
	bool first = true;
	fputc('[', f);
	for (size_t i = 0; i < v->count; ++i) {
		if (v->list[i] < 0)
			continue;
		fputs(first ? "" : ", ", f);
		write_type(f, m, v->list[i]);
		first = false;
	}
	fputs("]\n", f);
}

/* The per-craft offsets of PlayerModelIndices and PlayerOffsetsX/Y/Z. The
 * hook uses them only when the four lists have the same length, and keeps
 * the first entry of a craft. */
static void write_craft_offsets(Section* s, const Models* m, const Layer* l) {
	static const int keys[] = { K_PLAYER_MODEL_INDICES, K_PLAYER_OFFSETS_X, K_PLAYER_OFFSETS_Y,
								K_PLAYER_OFFSETS_Z };
	int              set    = 0;
	for (int i = 0; i < 4; ++i)
		set += l->values[keys[i]].set;
	if (!set)
		return;
	if (set != 4) {
		ReportFatal(
			LABEL,
			"%s: [HangarObjects] sets only some of PlayerModelIndices and PlayerOffsetsX/Y/Z, and the "
			"import can only write them together",
			l->where);
		return;
	}
	const RawValue* types = &l->values[K_PLAYER_MODEL_INDICES];
	bool            same  = true;
	for (int i = 1; i < 4; ++i)
		same = same && l->values[keys[i]].count == types->count;
	section_key(s, "craft_offsets_m");
	fputc('{', s->f);
	bool first = true;
	for (size_t i = 0; same && i < types->count; ++i) {
		bool repeated = false;
		for (size_t j = 0; j < i; ++j)
			repeated = repeated || types->list[j] == types->list[i];
		if (types->list[i] < 0 || repeated)
			continue;
		fputs(first ? "" : ", ", s->f);
		write_type(s->f, m, types->list[i]);
		fputs(": ", s->f);
		Xwau_WriteMetres3(s->f, (int)l->values[K_PLAYER_OFFSETS_X].list[i],
						  (int)l->values[K_PLAYER_OFFSETS_Y].list[i],
						  (int)l->values[K_PLAYER_OFFSETS_Z].list[i]);
		first = false;
	}
	fputs("}\n", s->f);
}

static void write_player(FILE* f, const Models* m, const Layer* l, int indent) {
	static const int       offset[]   = { K_PLAYER_OFFSET_X, K_PLAYER_OFFSET_Y, K_PLAYER_OFFSET_Z };
	static const int       inverted[] = { K_PLAYER_INVERTED_OFFSET_X, K_PLAYER_INVERTED_OFFSET_Y,
										  K_PLAYER_INVERTED_OFFSET_Z };
	static const long long zeros[]    = { 0, 0, 0 };
	Section                s          = { f, indent, "player", NULL, false };
	write_group(&s, l, "offset_m", offset, zeros, 3);
	if (check_derived(l, inverted[0]) && check_derived(l, inverted[1]) && check_derived(l, inverted[2]))
		write_group(&s, l, "inverted_offset_m", inverted, zeros, 3);
	write_metres(&s, l, "rise_m", K_PLAYER_ANIMATION_ELEVATION, 0);
	if (check_derived(l, K_PLAYER_ANIMATION_INVERTED_ELEVATION))
		write_metres(&s, l, "inverted_rise_m", K_PLAYER_ANIMATION_INVERTED_ELEVATION, 0);
	write_metres(&s, l, "straight_m", K_PLAYER_ANIMATION_STRAIGHT_LINE, 0);
	if (l->values[K_IS_PLAYER_FLOOR_INVERTED].set) {
		section_key(&s, "floor");
		write_floor(f, layer_int(l, K_IS_PLAYER_FLOOR_INVERTED, 0) != 0);
		fputc('\n', f);
	}
	if (l->values[K_PLAYER_FLOOR_INVERTED_MODEL_INDICES].set) {
		section_key(&s, "inverted_floor_crafts");
		write_type_list(f, m, &l->values[K_PLAYER_FLOOR_INVERTED_MODEL_INDICES]);
	}
	write_craft_offsets(&s, m, l);
}

/* Writes a layout object as a flow map. */
static void write_map_line(FILE* f, const Models* m, const MapLine* line) {
	fputs("{type: ", f);
	write_type(f, m, line->type);
	if (line->markings & 0xFF)
		fprintf(f, ", markings: %lld", line->markings & 0xFF);
	fputs(", position_m: ", f);
	/* A z of 0x7FFFFFFF puts the object on the floor. */
	if (line->z == 0x7FFFFFFF) {
		fputc('[', f);
		Xwau_WriteMetres(f, (int)line->x);
		fputs(", ", f);
		Xwau_WriteMetres(f, (int)line->y);
		fputc(']', f);
	} else {
		Xwau_WriteMetres3(f, (int)line->x, (int)line->y, (int)line->z);
	}
	fputs(", heading_degrees: ", f);
	Xwau_WriteDegrees(f, (int)(line->heading & 0xFFFF));
	if (line->pitch & 0xFFFF) {
		fputs(", pitch_degrees: ", f);
		Xwau_WriteDegrees(f, (int)(line->pitch & 0xFFFF));
	}
	if (strcmp(line->profile, "Default") != 0) {
		fputs(", profile: ", f);
		Xwau_WriteString(f, line->profile);
	}
	if (line->inverted >= 0) {
		fputs(", floor: ", f);
		write_floor(f, line->inverted);
	}
	fputc('}', f);
}

/* Writes the crane arm and the parked objects of a map section. The hook
 * uses a fixed line as the crane arm. When that line places nothing, the
 * hangar has no crane arm. */
static void write_layout(FILE* f, const Models* m, const Layout* layout, int family, int indent) {
	if (!layout->present)
		return;
	const int crane = kCraneLine[family];
	if ((size_t)crane < layout->count && layout->lines[crane].placed) {
		fprintf(f, "%*scrane_arm: ", indent, "");
		write_map_line(f, m, &layout->lines[crane]);
		fputc('\n', f);
	}
	size_t parked = 0;
	for (size_t i = 0; i < layout->count; ++i)
		parked += (int)i != crane && layout->lines[i].placed;
	/* An empty list still hides the lower layers' layout. */
	fprintf(f, "%*sparked:%s\n", indent, "", parked ? "" : " []");
	for (size_t i = 0; i < layout->count; ++i) {
		if ((int)i == crane || !layout->lines[i].placed)
			continue;
		fprintf(f, "%*s  - ", indent, "");
		write_map_line(f, m, &layout->lines[i]);
		fputc('\n', f);
	}
}

static void write_cameras(FILE* f, const Layer* l, int family, int indent) {
	if (!l->camera_present[family])
		return;
	const int*   keys  = family ? kFamCameraKeys : kCameraKeys;
	const size_t count = family ? sizeof kFamCameraKeys / sizeof kFamCameraKeys[0]
								: sizeof kCameraKeys / sizeof kCameraKeys[0];
	fprintf(f, "%*scameras:\n", indent, "");
	for (size_t i = 0; i < count; ++i) {
		const long long* xyz = l->cameras[family][keys[i]];
		fprintf(f, "%*s  key_%d: ", indent, "", keys[i]);
		Xwau_WriteMetres3(f, (int)xyz[0], (int)xyz[1], (int)xyz[2]);
		fputc('\n', f);
	}
}

static void write_layer(FILE* f, const Models* m, const Layer* l, const Below* below, int indent) {
	if (l->model[0])
		fprintf(f, "%*shangar_model: %s\n", indent, "", l->model);
	write_hangar(f, l, indent);
	write_shuttle(f, m, l, indent);
	write_droids(f, m, l, below, indent);
	write_roof_crane(f, l, indent);
	write_player(f, m, l, indent);
	if (l->swaps.count) {
		fprintf(f, "%*sswaps:\n", indent, "");
		for (size_t i = 0; i < l->swaps.count; ++i)
			fprintf(f, "%*s  %s\n", indent, "", l->swaps.items[i]);
	}
	write_layout(f, m, &l->layouts[0], 0, indent);
	write_cameras(f, l, 0, indent);
	if (l->layouts[1].present || l->camera_present[1]) {
		fprintf(f, "%*sfamily_base:\n", indent, "");
		write_layout(f, m, &l->layouts[1], 1, indent + 2);
		write_cameras(f, l, 1, indent + 2);
	}
	for (size_t i = 0; i < l->iff_count; ++i) {
		if (i == 0)
			fprintf(f, "%*sby_iff:\n", indent, "");
		fprintf(f, "%*s  %d:\n", indent, "", l->iffs[i].iff);
		/* An IFF layer has its ship's layer below it. */
		Chain chain = { { l }, 1 };
		for (size_t c = 0; c < below->chains[0].count; ++c)
			chain.layers[chain.count++] = below->chains[0].layers[c];
		const Below iff_below = { &chain, 1 };
		write_layer(f, m, l->iffs[i].layer, &iff_below, indent + 4);
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

/* Replaces the `hangar` of the mission file `path` with `hangar`. Creates
 * the file when it doesn't exist and the layer isn't empty. Sets *written
 * when the file has a layer. Returns false when the file can't be
 * written. */
static bool write_mission_layer(const Models* m, const char* path, const Layer* hangar, const Below* below,
								bool* written) {
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
			write_layer(f, m, hangar, below, 2);
			if (hangar->ships.count) {
				fprintf(f, "  use_ship_settings:\n");
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
 * to the number of files with a layer. `below` holds the layers that can be
 * below a mission's layer. */
static bool write_mission_layers(const Models* m, const char* game, const char* stock_game,
								 const XwauLines* ships, const Below* below, const char* mod, size_t* count) {
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
						   write_mission_layer(m, path, &hangar, below, &written);
			*count += written;
			layer_free(&hangar);
		}
		XwauLines_Free(&names);
	}
	return ok;
}

/* Builds the chains of layers that can be below a mission's layer: the
 * default layer alone, each ship's layer above the default layer, and each
 * IFF layer above its ship's layer. Returns NULL when out of memory. */

static Chain* mission_chains(const Layer* def, const Layer* ships, size_t ship_count, size_t* count) {
	size_t total = 1 + ship_count;
	for (size_t i = 0; i < ship_count; ++i)
		total += ships[i].iff_count;
	Chain* chains = (Chain*)calloc(total, sizeof *chains);
	if (!chains)
		return NULL;
	size_t n    = 0;
	chains[n++] = (Chain) { { def }, 1 };
	for (size_t i = 0; i < ship_count; ++i) {
		chains[n++] = (Chain) { { &ships[i], def }, 2 };
		for (size_t j = 0; j < ships[i].iff_count; ++j)
			chains[n++] = (Chain) { { ships[i].iffs[j].layer, &ships[i], def }, 3 };
	}
	*count = n;
	return chains;
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
	XwauLines names = { 0 }, ship_keys = { 0 };
	Craft*    crafts      = NULL;
	size_t    craft_count = 0;
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
		Craft craft = { 0 };
		snprintf(craft.key, sizeof craft.key, "%s", key);
		ok = ok && craft_record(xwau, name, &craft);
		if (ok && (craft.has_size || craft.has_camera)) {
			Craft* entry = ARRAY_PUSH(crafts, craft_count);
			ok           = entry != NULL;
			if (ok)
				*entry = craft;
		}
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
	size_t      models        = 0;
	const Chain nothing       = { { NULL }, 0 };
	const Chain default_only  = { { &def }, 1 };
	const Below below_default = { &nothing, 1 };
	const Below below_ship    = { &default_only, 1 };
	if (f) {
		fputs("# Made by the OpenXWA mod importer.\n", f);
		if (!layer_empty(&def)) {
			fprintf(f, "default:\n");
			write_layer(f, m, &def, &below_default, 2);
		}
		if (ship_count)
			fprintf(f, "ships:\n");
		for (size_t i = 0; i < ship_count; ++i) {
			fprintf(f, "  %s:\n", ship_keys.items[i]);
			write_layer(f, m, &ships[i], &below_ship, 4);
			models += ships[i].model[0] != 0;
		}
		if (craft_count)
			fprintf(f, "crafts:\n");
		for (size_t i = 0; i < craft_count; ++i)
			write_craft(f, &crafts[i]);
		ok = fclose(f) == 0 && replace_file(temporary, path);
	}
	if (ok)
		Report(NULL, "wrote %s: %zu ships (%zu hangar models), %zu crafts", path, ship_count, models,
			   craft_count);

	if (ok && stock_game) {
		size_t      chain_count = 0, count = 0;
		Chain*      chains = mission_chains(&def, ships, ship_count, &chain_count);
		const Below below  = { chains, chain_count };
		ok = chains && write_mission_layers(m, xwau, stock_game, &ship_keys, &below, mod, &count);
		free(chains);
		if (ok)
			Report(NULL, "wrote %zu mission hangar layers", count);
	}

	layer_free(&def);
	for (size_t i = 0; i < ship_count; ++i)
		layer_free(&ships[i]);
	free(ships);
	XwauLines_Free(&names);
	XwauLines_Free(&ship_keys);
	free(crafts);
	models_free(m);
	free(m);
	return ok;
}
