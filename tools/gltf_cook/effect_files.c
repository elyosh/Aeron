/*
 * The canonical written form of the model animation and effect library
 * files (effect_files.h).
 */
#include "effect_files.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* The members of an entry, in the format's order (animated_materials.md):
 * a file's, a material's, a flipbook's, the damage entry's, an
 * alternative's and a library image's. Other members follow, by name. */
static const char* const kOrder[] = {
	"version",   "state",    "layer",     "mode",         "library",          "kind",
	"baseColor", "emissive", "factor",    "frames",       "emissiveStrength", "unlit",
	"maxSize",   "crop",     "images",    "materials",    "flipbooks",        "damage",
	"energy",    "choice",   "placement", "alternatives", "stages",           "scale",
	"offset"
};
static const char* const kEnergies[] = { "shields", "tractor", "jamming" };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct Text {
	char*  s;
	size_t len, cap;
	bool   failed;
} Text;

static void put(Text* t, const char* s, size_t n) {
	if (t->failed)
		return;
	if (t->len + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 4096;
		while (t->len + n + 1 > cap)
			cap *= 2;
		char* grown = (char*)realloc(t->s, cap);
		if (!grown) {
			t->failed = true;
			return;
		}
		t->s   = grown;
		t->cap = cap;
	}
	memcpy(t->s + t->len, s, n);
	t->len += n;
	t->s[t->len] = '\0';
}

static void put_text(Text* t, const char* s) { put(t, s, strlen(s)); }

/* Words YAML (1.1 included) may read as a boolean or null. */
static const char* const kWords[] = { "y", "n", "yes", "no", "true", "false", "on", "off", "null" };

/* Whether string `s` reads back as itself unquoted: letters, digits, '_',
 * '.', '/' and '-', starting with a letter or '_', and not a word YAML
 * reads as something else. */
static bool plain(const char* s) {
	if (!isalpha((unsigned char)s[0]) && s[0] != '_')
		return false;
	for (const char* c = s; *c; ++c)
		if (!isalnum((unsigned char)*c) && !strchr("_./-", *c))
			return false;
	for (size_t i = 0; i < COUNT(kWords); ++i) {
		size_t k = 0;
		while (s[k] && kWords[i][k] && tolower((unsigned char)s[k]) == kWords[i][k])
			k++;
		if (!s[k] && !kWords[i][k])
			return false;
	}
	return true;
}

/* A string, unquoted when plain, else double-quoted with JSON's escapes
 * (which YAML's double-quoted scalars share). */
static void put_string(Text* t, const char* s) {
	if (plain(s)) {
		put_text(t, s);
		return;
	}
	put_text(t, "\"");
	for (; *s; ++s) {
		char escaped[8];
		if (*s == '"' || *s == '\\') {
			snprintf(escaped, sizeof escaped, "\\%c", *s);
			put_text(t, escaped);
		} else if ((unsigned char)*s < 0x20) {
			snprintf(escaped, sizeof escaped, "\\u%04x", (unsigned char)*s);
			put_text(t, escaped);
		} else {
			put(t, s, 1);
		}
	}
	put_text(t, "\"");
}

/* The shortest decimal, without exponent, that reads back as the same
 * 32-bit float; integers without decimals. */
static void put_number(Text* t, double value) {
	const float f = (float)value;
	char        s[128];
	if (f == 0.0f || (f == floorf(f) && fabsf(f) < 16777216.0f)) {
		snprintf(s, sizeof s, "%.0f", f == 0.0f ? 0.0 : (double)f);
	} else {
		for (int decimals = 1; decimals <= 60; ++decimals) {
			snprintf(s, sizeof s, "%.*f", decimals, (double)f);
			if (strtof(s, NULL) == f)
				break;
		}
	}
	put_text(t, s);
}

static int order_of(const char* name) {
	for (size_t i = 0; i < COUNT(kOrder); ++i)
		if (strcmp(name, kOrder[i]) == 0)
			return (int)i;
	return (int)COUNT(kOrder);
}

static int energy_of(const cJSON* item) {
	for (size_t i = 0; cJSON_IsString(item) && i < COUNT(kEnergies); ++i)
		if (strcmp(item->valuestring, kEnergies[i]) == 0)
			return (int)i;
	return (int)COUNT(kEnergies);
}

/* Members at their default, which the canonical form leaves out. */
static bool is_default(const cJSON* member) {
	return cJSON_IsNumber(member) &&
		   ((strcmp(member->string, "emissiveStrength") == 0 && (float)member->valuedouble == 1.0f) ||
			(strcmp(member->string, "unlit") == 0 && (float)member->valuedouble == 0.0f));
}

/* Members sorted by name (`by_name`, a map of materials or images) or in
 * the format's order. */
static bool g_by_name;

static int member_compare(const void* a, const void* b) {
	const cJSON* x = *(const cJSON* const*)a;
	const cJSON* y = *(const cJSON* const*)b;
	if (!g_by_name) {
		const int ox = order_of(x->string), oy = order_of(y->string);
		if (ox != oy)
			return ox < oy ? -1 : 1;
	}
	return strcmp(x->string, y->string);
}

static int energy_compare(const void* a, const void* b) {
	const int x = energy_of(*(const cJSON* const*)a), y = energy_of(*(const cJSON* const*)b);
	return (x > y) - (x < y);
}

/* `parent`'s children, sorted with `compare` (stable: ties by position). */
static const cJSON** sorted_children(const cJSON* parent, int (*compare)(const void*, const void*),
									 bool by_name, size_t* count) {
	*count = 0;
	const cJSON* c;
	cJSON_ArrayForEach(c, parent) { ++*count; }
	const cJSON** items = (const cJSON**)malloc((*count ? *count : 1) * sizeof *items);
	if (!items)
		return NULL;
	size_t n = 0;
	cJSON_ArrayForEach(c, parent) { items[n++] = c; }
	if (compare) {
		/* Insertion sort: stable, and entries are few. */
		g_by_name = by_name;
		for (size_t i = 1; i < n; ++i)
			for (size_t j = i; j > 0 && compare(&items[j - 1], &items[j]) > 0; --j) {
				const cJSON* swap = items[j - 1];
				items[j - 1]      = items[j];
				items[j]          = swap;
			}
	}
	return items;
}

static void put_value(Text* t, const cJSON* v, bool by_name);

static void put_members(Text* t, const cJSON* object, bool by_name) {
	size_t        n;
	const cJSON** items = sorted_children(object, member_compare, by_name, &n);
	if (!items) {
		t->failed = true;
		return;
	}
	bool first = true;
	for (size_t i = 0; i < n; ++i) {
		if (!by_name && is_default(items[i]))
			continue;
		put_text(t, first ? "{ " : ", ");
		first = false;
		put_string(t, items[i]->string);
		put_text(t, ": ");
		put_value(t, items[i], false);
	}
	put_text(t, first ? "{}" : " }");
	free(items);
}

static void put_value(Text* t, const cJSON* v, bool by_name) {
	if (cJSON_IsObject(v)) {
		put_members(t, v, by_name);
	} else if (cJSON_IsArray(v)) {
		const bool    energy = v->string && strcmp(v->string, "energy") == 0;
		size_t        n;
		const cJSON** items = sorted_children(v, energy ? energy_compare : NULL, false, &n);
		if (!items) {
			t->failed = true;
			return;
		}
		put_text(t, "[");
		for (size_t i = 0; i < n; ++i) {
			put_text(t, i ? ", " : "");
			put_value(t, items[i], false);
		}
		put_text(t, "]");
		free(items);
	} else if (cJSON_IsString(v)) {
		put_string(t, v->valuestring);
	} else if (cJSON_IsNumber(v)) {
		put_number(t, v->valuedouble);
	} else if (cJSON_IsBool(v)) {
		put_text(t, cJSON_IsTrue(v) ? "true" : "false");
	} else {
		put_text(t, "null");
	}
}

/* `map` (materials or images) one entry per line, in key order. */
static void put_map(Text* t, const char* name, const cJSON* map) {
	size_t        n;
	const cJSON** items = sorted_children(map, member_compare, true, &n);
	if (!items) {
		t->failed = true;
		return;
	}
	put_text(t, name);
	put_text(t, n ? ":\n" : ": {}\n");
	for (size_t i = 0; i < n; ++i) {
		put_text(t, "  ");
		put_string(t, items[i]->string);
		put_text(t, ": ");
		put_value(t, items[i], false);
		put_text(t, "\n");
	}
	free(items);
}

char* EffectFiles_Format(const cJSON* file, const char* header) {
	const cJSON* version   = cJSON_GetObjectItemCaseSensitive(file, "version");
	const cJSON* materials = cJSON_GetObjectItemCaseSensitive(file, "materials");
	const cJSON* library   = cJSON_GetObjectItemCaseSensitive(file, "library");
	const cJSON* images    = cJSON_GetObjectItemCaseSensitive(file, "images");
	if (!cJSON_IsObject(file) || !cJSON_IsNumber(version) ||
		!(cJSON_IsObject(materials) || (cJSON_IsString(library) && cJSON_IsObject(images))))
		return NULL;
	Text t = { 0 };
	put_text(&t, header ? header : "");
	put_text(&t, "version: ");
	put_number(&t, version->valuedouble);
	put_text(&t, "\n");
	if (cJSON_IsObject(materials)) {
		const cJSON* energy = cJSON_GetObjectItemCaseSensitive(file, "energy");
		if (energy) {
			put_text(&t, "energy: ");
			put_value(&t, energy, false);
			put_text(&t, "\n");
		}
		put_map(&t, "materials", materials);
	} else {
		put_text(&t, "library: ");
		put_string(&t, library->valuestring);
		put_text(&t, "\n");
		put_map(&t, "images", images);
	}
	if (t.failed) {
		free(t.s);
		return NULL;
	}
	return t.s;
}

/* The leading comment lines of the file `path`, or "" (malloc'd). */
static char* leading_comments(const char* path) {
	Text  t = { 0 };
	FILE* f = fopen(path, "rb");
	char  line[4096];
	while (f && fgets(line, sizeof line, f) && line[0] == '#') {
		put_text(&t, line);
		if (!strchr(line, '\n'))
			put_text(&t, "\n");
	}
	if (f)
		fclose(f);
	put_text(&t, "");
	if (t.failed) {
		free(t.s);
		return NULL;
	}
	return t.s ? t.s : (char*)calloc(1, 1);
}

bool EffectFiles_Write(const char* tool, const cJSON* file, const char* header, const char* path) {
	char* kept = header ? NULL : leading_comments(path);
	char* text = (header || kept) ? EffectFiles_Format(file, header ? header : kept) : NULL;
	free(kept);
	if (!text) {
		fprintf(stderr, "%s: %s: cannot format the file\n", tool, path);
		return false;
	}
	FILE*      f       = fopen(path, "wb");
	const bool written = f && fputs(text, f) >= 0;
	const bool closed  = f && fclose(f) == 0;
	free(text);
	if (!written || !closed)
		fprintf(stderr, "%s: cannot write %s\n", tool, path);
	return written && closed;
}
