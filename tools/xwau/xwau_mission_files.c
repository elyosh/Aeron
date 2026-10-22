#include "xwau_mission_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "opt.h"
#include "util.h"
#include "xwau_concourse.h"
#include "xwau_game_tables.h"
#include "xwau_mission_settings.h"
#include "xwau_mission_tie.h"
#include "xwau_objects.h"
#include "xwau_skins.h"
#include "xwau_stats_profiles.h"

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

typedef struct Tie {
	XwauTieFlightGroup* groups;
	size_t              count;
} Tie;

/* ---- Object profiles ------------------------------------------------------ */

typedef struct Profile {
	long long fg;
	char*     profile;
} Profile;

static int compare_profiles(const void* a, const void* b) {
	const long long x = ((const Profile*)a)->fg, y = ((const Profile*)b)->fg;
	return x < y ? -1 : x > y;
}

/* Reads the [Objects] lines of mission `base`: <mission>_Objects.txt, or
 * else the [Objects] section of <mission>.ini. Returns false when out of
 * memory. */
static bool object_lines(const char* game, const char* base, XwauLines* lines) {
	char rel[1100], path[1024];
	if (!PathFormat(rel, sizeof rel, "%s_Objects.txt", base) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, lines))
		return false;
	if (lines->count)
		return true;
	return PathFormat(rel, sizeof rel, "%s.ini", base) &&
		   XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Objects",
						  XWAU_RULES_CPP, lines);
}

/* Reads the `ObjectProfile_fg_<N> = <profile>` lines of mission `base`,
 * sorted by flight group. */
static bool profiles(const char* game, const char* base, Profile** out, size_t* count) {
	*out   = NULL;
	*count = 0;
	XwauLines         lines;
	bool              ok        = object_lines(game, base, &lines);
	static const char kPrefix[] = "objectprofile_", kFgPrefix[] = "objectprofile_fg_";
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char  line[4096];
		char* equals;
		snprintf(line, sizeof line, "%s", lines.items[i]);
		if (!(equals = strchr(line, '=')))
			continue;
		*equals           = 0;
		const char* key   = Xwau_Trim(line, XWAU_RULES_CPP);
		const char* value = Xwau_Trim(equals + 1, XWAU_RULES_CPP);
		if (strncasecmp(key, kPrefix, sizeof kPrefix - 1) != 0)
			continue;
		const char* digits = key + sizeof kFgPrefix - 1;
		bool        by_fg  = strncasecmp(key, kFgPrefix, sizeof kFgPrefix - 1) == 0 && *digits &&
							 strspn(digits, "0123456789") == strlen(digits);
		if (!by_fg) {
			Report(base, "'%s' does not select a flight group, so it is not imported", key);
			continue;
		}
		long long fg = 0;
		for (const char* d = digits; *d; ++d)
			if (fg < 1000000000)
				fg = fg * 10 + (*d - '0');
		bool seen = false;
		for (size_t k = 0; !seen && k < *count; ++k)
			seen = (*out)[k].fg == fg;
		if (seen || !*value)
			continue;
		char*    profile = StrDup(value);
		Profile* added   = profile ? ARRAY_PUSH(*out, *count) : NULL;
		if (!(ok = added != NULL)) {
			free(profile);
			break;
		}
		added->profile = profile;
		added->fg      = fg;
	}
	XwauLines_Free(&lines);
	if (*count)
		qsort(*out, *count, sizeof **out, compare_profiles);
	return ok;
}

static void free_profiles(Profile* list, size_t count) {
	for (size_t i = 0; i < count; ++i)
		free(list[i].profile);
	free(list);
}

/* ---- Model swaps ---------------------------------------------------------- */

typedef struct Swap {
	char source[256], target[256]; /* model keys */
} Swap;

/* The model keys that the game loads: the rows of XWAU's FlightModels/
 * Spacecraft0.LST and Equipment0.LST, and the cockpit and exterior of each. */
static bool list_keys(const char* game, XwauLines* out) {
	memset(out, 0, sizeof *out);
	static const char* const kLists[]    = { "FlightModels/Spacecraft0.LST", "FlightModels/Equipment0.LST" };
	static const char* const kSuffixes[] = { "", "cockpit", "exterior" };
	bool                     ok          = true;
	for (size_t l = 0; ok && l < 2; ++l) {
		char      path[1024];
		XwauLines rows;
		if (!XwauLines_Read(Xwau_FindFile(game, kLists[l], path, sizeof path) ? path : NULL, "",
							XWAU_RULES_CPP, &rows))
			return false;
		for (size_t i = 0; ok && i < rows.count; ++i) {
			char key[256];
			if (!Xwau_ModelKey(rows.items[i], key, sizeof key))
				continue;
			for (size_t s = 0; ok && s < 3; ++s) {
				char full[300];
				snprintf(full, sizeof full, "%s%s", key, kSuffixes[s]);
				ok = XwauLines_Add(out, full, strlen(full));
			}
		}
		XwauLines_Free(&rows);
	}
	if (!ok)
		XwauLines_Free(out);
	return ok;
}

/* Reads the `FlightModels\<A>.opt = FlightModels\<B>.opt` lines of mission
 * `base`, as hook_mission_objects reads them: the game passes the path of
 * each model it loads, the first line with that path wins, in any case, and
 * the model is swapped when <B> exists. A swap is kept only when `models`
 * has <b>.glb. A source that the game doesn't load is reported, and kept. */
static bool swaps(const char* game, const char* base, const char* models, const XwauLines* keys, Swap** out,
				  size_t* count) {
	*out   = NULL;
	*count = 0;
	XwauLines lines, seen_paths = { 0 };
	bool      ok = object_lines(game, base, &lines);
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char  line[4096];
		char* equals;
		snprintf(line, sizeof line, "%s", lines.items[i]);
		if (!(equals = strchr(line, '=')))
			continue;
		*equals           = 0;
		const char* key   = Xwau_Trim(line, XWAU_RULES_CPP);
		const char* value = Xwau_Trim(equals + 1, XWAU_RULES_CPP);
		if (!EndsWithNoCase(key, ".opt"))
			continue;
		/* The first line with this path wins, even when it is dropped. */
		bool seen = false;
		for (size_t k = 0; !seen && k < seen_paths.count; ++k)
			seen = Xwau_SameName(seen_paths.items[k], key);
		if (seen)
			continue;
		if (!(ok = XwauLines_Add(&seen_paths, key, strlen(key))))
			break;
		Swap swap;
		if (strncasecmp(key, "FlightModels\\", 13) != 0 ||
			!Xwau_ModelKey(key, swap.source, sizeof swap.source)) {
			Report(base, "[Objects]: the game never loads '%s', so its swap does nothing", key);
			continue;
		}
		char rel[1024], path[1300];
		snprintf(rel, sizeof rel, "%s", value);
		for (char* c = rel; *c; ++c)
			if (*c == '\\')
				*c = '/';
		if (!*value || !Xwau_FindFile(game, rel, path, sizeof path)) {
			Report(base, "[Objects]: '%s' doesn't exist, so XWAU keeps %s", value, key);
			continue;
		}
		if (!Xwau_ModelKey(value, swap.target, sizeof swap.target))
			continue;
		if (!PathFormat(path, sizeof path, "%s/%s.glb", models, swap.target) || !IsFile(path)) {
			Report(base, "[Objects]: %s has no GLB in the mod folder, so the swap of %s is dropped",
				   swap.target, swap.source);
			continue;
		}
		bool known = false;
		for (size_t k = 0; !known && k < keys->count; ++k)
			known = strcmp(keys->items[k], swap.source) == 0;
		if (!known)
			Report(base, "[Objects]: %s isn't a model of the lists, so its swap may do nothing", swap.source);
		if (XwauObjects_HasExeCraftValues(game, swap.source) ||
			XwauObjects_HasExeCraftValues(game, swap.target))
			Report(base,
				   "[Objects]: %s or %s has [ExeCraftValues], so XWAU gives the craft other values in this "
				   "mission; the object type files keep the values of the list's model",
				   swap.source, swap.target);
		Swap* added = ARRAY_PUSH(*out, *count);
		if ((ok = added != NULL))
			*added = swap;
	}
	XwauLines_Free(&lines);
	XwauLines_Free(&seen_paths);
	if (!ok) {
		free(*out);
		*out   = NULL;
		*count = 0;
	}
	return ok;
}

/* ---- Craft names ---------------------------------------------------------- */

/* hook_mission_tie keeps 218 craft records. */
#define CRAFT_NAME_RECORDS 218

/* The names of craft record `record`, from the "craft, N, key, value"
 * lines. NULL is "not set". */
typedef struct CraftName {
	int       record;
	char      model[256];
	char *    ship_list, *first_ship_list, *name, *short_name, *plural;
	bool      has_icon;
	long long icon[4];
} CraftName;

static void free_craft_names(CraftName* list, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		free(list[i].ship_list);
		free(list[i].first_ship_list);
		free(list[i].name);
		free(list[i].short_name);
		free(list[i].plural);
	}
	free(list);
}

/* Replaces `*field` with a copy of `value`. Returns false when out of
 * memory. */
static bool set_text(char** field, const char* value) {
	char* copy = StrDup(value);
	if (!copy)
		return false;
	free(*field);
	*field = copy;
	return true;
}

/* Reads the craft lines of mission `base`, as hook_mission_tie applies
 * them: in their order, so the last line for a value wins, for records 0
 * to 217. Each record is then given its model: the list row of the first
 * object type of XWAU's tables that uses it. */
static bool craft_names(const XwauLines* lines, const XwauGameTables* tables, const Tie* tie,
						const char* base, CraftName** out, size_t* count) {
	*out    = NULL;
	*count  = 0;
	bool ok = true;
	for (size_t i = 0; ok && i < lines->count; ++i) {
		XwauLines words;
		if (!(ok = Xwau_Tokenize(lines->items[i], XWAU_RULES_CPP, &words)))
			break;
		long long   record = 0;
		const char* key    = words.count >= 4 ? words.items[2] : "";
		const bool  icon   = Xwau_SameName(key, "mapicon");
		if (words.count < 4 || !Xwau_Stoi(words.items[1], &record) || record < 0 ||
			record >= CRAFT_NAME_RECORDS || (icon && words.count < 7)) {
			Report(base, "mission_tie: XWAU ignores or fails on the line '%s'", lines->items[i]);
			XwauLines_Free(&words);
			continue;
		}
		CraftName* entry = NULL;
		for (size_t k = 0; !entry && k < *count; ++k)
			if ((*out)[k].record == (int)record)
				entry = &(*out)[k];
		if (!entry && (entry = ARRAY_PUSH(*out, *count)) != NULL)
			entry->record = (int)record;
		const char* value = words.items[3];
		if (!entry) {
			ok = false;
		} else if (Xwau_SameName(key, "name")) {
			/* The briefing reads the first name line, the flight the last. */
			ok = (entry->first_ship_list || set_text(&entry->first_ship_list, value)) &&
				 set_text(&entry->ship_list, value);
		} else if (Xwau_SameName(key, "specname")) {
			ok = set_text(&entry->name, value);
		} else if (Xwau_SameName(key, "shortname")) {
			ok = set_text(&entry->short_name, value);
		} else if (Xwau_SameName(key, "pluralname")) {
			ok = set_text(&entry->plural, value);
		} else if (icon) {
			bool numbers = true;
			for (int k = 0; k < 4; ++k)
				numbers = numbers && Xwau_Stoi(words.items[3 + k], &entry->icon[k]);
			if (numbers)
				entry->has_icon = true;
			else
				Report(base, "mission_tie: '%s' has a value that isn't a number, which XWAU would fail on",
					   lines->items[i]);
		}
		XwauLines_Free(&words);
	}
	size_t kept = 0;
	for (size_t i = 0; ok && i < *count; ++i) {
		CraftName* entry = &(*out)[i];
		int        type  = -1;
		for (int t = 0; type < 0 && tables && t < XWAU_GAME_OBJECT_TYPES; ++t)
			if (XwauGameTables_Type(tables, t).record == entry->record)
				type = t;
		const char* model = type >= 0 ? XwauGameTables_Model(tables, type) : "";
		bool        twice = false;
		for (size_t k = 0; !twice && k < kept; ++k)
			twice = strcmp((*out)[k].model, model) == 0;
		if (!model[0] || twice) {
			Report(base,
				   model[0]
					   ? "mission_tie: craft record %d has the model of an earlier one, %s; its names are "
						 "dropped"
					   : "mission_tie: no object type with a model uses craft record %d; its names are "
						 "dropped",
				   entry->record, model);
			continue;
		}
		snprintf(entry->model, sizeof entry->model, "%s", model);
		bool used = false;
		for (size_t g = 0; !used && g < tie->count; ++g) {
			const int craft = tie->groups[g].craft;
			used            = craft >= 0 && craft < XWAU_GAME_CRAFT_TYPES &&
							  XwauGameTables_Type(tables, tables->craft[craft]).record == entry->record;
		}
		if (!used)
			Report(base, "mission_tie: no flight group uses %s, so its names don't show", model);
		const char* const texts[] = { entry->ship_list, entry->name, entry->short_name, entry->plural };
		for (size_t t = 0; t < sizeof texts / sizeof texts[0]; ++t) {
			bool ascii = true;
			for (const unsigned char* c = (const unsigned char*)(texts[t] ? texts[t] : ""); *c; ++c)
				ascii = ascii && *c < 0x80;
			if (!ascii)
				Report(base,
					   "mission_tie: the name '%s' of %s isn't ASCII, so the game may show it differently",
					   texts[t], model);
		}
		if (entry->first_ship_list && strcmp(entry->first_ship_list, entry->ship_list) != 0)
			Report(base,
				   "mission_tie: %s has two names; the briefing shows '%s' in XWAU and the game '%s', which "
				   "OpenXWA shows in both",
				   model, entry->first_ship_list, entry->ship_list);
		if (kept != i) {
			CraftName moved = *entry;
			(*out)[i]       = (*out)[kept];
			(*out)[kept]    = moved;
		}
		++kept;
	}
	if (!ok) {
		free_craft_names(*out, *count);
		*out   = NULL;
		*count = 0;
		return false;
	}
	/* The dropped ones are after `kept`. */
	for (size_t i = kept; i < *count; ++i) {
		free((*out)[i].ship_list);
		free((*out)[i].first_ship_list);
		free((*out)[i].name);
		free((*out)[i].short_name);
		free((*out)[i].plural);
	}
	*count = kept;
	return true;
}

static void write_craft_names(FILE* f, const CraftName* list, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		const CraftName* entry = &list[i];
		fprintf(f, "%s  %s: {", i ? "" : "craft_names:\n", entry->model);
		const char* sep = " ";

		const struct {
			const char* key;
			const char* value;
		} texts[] = { { "ship_list", entry->ship_list },
					  { "name", entry->name },
					  { "short_name", entry->short_name },
					  { "plural", entry->plural } };

		for (size_t t = 0; t < sizeof texts / sizeof texts[0]; ++t) {
			if (!texts[t].value)
				continue;
			fprintf(f, "%s%s: ", sep, texts[t].key);
			Xwau_WriteString(f, texts[t].value);
			sep = ", ";
		}
		if (entry->has_icon)
			fprintf(f, "%smap_icon: [%lld, %lld, %lld, %lld]", sep, entry->icon[0], entry->icon[1],
					entry->icon[2], entry->icon[3]);
		fputs(" }\n", f);
	}
}

/* ---- Skins ---------------------------------------------------------------- */

typedef struct SkinModel {
	char          key[256]; /* the model's name in lower case */
	int           states;
	XwauSkinSet   skins;
	XwauMarkings* defaults; /* markings in the global context */
	size_t        default_count;
} SkinModel;

typedef struct SkinModels {
	SkinModel*    models;
	size_t        count;
	XwauMarkings* scratch; /* XWAU_SKINS_MAX_MARKINGS */
} SkinModels;

static int compare_skin_models(const void* a, const void* b) {
	return strcmp(((const SkinModel*)a)->key, ((const SkinModel*)b)->key);
}

static void free_skin_models(SkinModels* s) {
	for (size_t i = 0; i < s->count; ++i) {
		XwauSkins_FreeNames(&s->models[i].skins);
		free(s->models[i].defaults);
	}
	free(s->models);
	free(s->scratch);
	memset(s, 0, sizeof *s);
}

/* Loads the OPTs in FlightModels that have skins, sorted by name. */
static bool load_skin_models(const char* game, const XwauMissions* missions, SkinModels* out) {
	memset(out, 0, sizeof *out);
	out->scratch = (XwauMarkings*)malloc(XWAU_SKINS_MAX_MARKINGS * sizeof *out->scratch);
	bool ok      = out->scratch != NULL;
	char dir[1024];
	if (Xwau_FindDir(game, "FlightModels", dir, sizeof dir)) {
		size_t count   = 0;
		char** entries = ListDir(dir, true, &count);
		for (size_t i = 0; ok && i < count; ++i) {
			const size_t len = strlen(entries[i]);
			char         key[256];
			if (len <= 4 || strcasecmp(entries[i] + len - 4, ".opt") != 0 || len - 4 >= sizeof key)
				continue;
			LowerCopy(key, len - 3, entries[i]);
			char path[2048];
			if (!PathFormat(path, sizeof path, "%s/%s", dir, entries[i]) || !IsFile(path))
				continue;
			SkinModel model = { 0 };
			snprintf(model.key, sizeof model.key, "%s", key);
			ok = XwauSkins_Names(game, key, &model.skins);
			if (!ok || !model.skins.exists) {
				XwauSkins_FreeNames(&model.skins);
				continue;
			}
			opt_error_t err;
			opt_file_t* opt = opt_load_file(path, &err);
			if (!opt) {
				Report(path, "%s, so its skins are left out", err.msg);
				XwauSkins_FreeNames(&model.skins);
				continue;
			}
			model.states = XwauSkins_StateCount(opt);
			opt_free(opt);
			ok = XwauSkins_Markings(XwauMissions_SkinContext(missions, 0), key, model.states, &model.skins,
									out->scratch, &model.default_count);
			model.defaults   = ok ? (XwauMarkings*)malloc((model.default_count ? model.default_count : 1) *
														  sizeof *model.defaults)
								  : NULL;
			SkinModel* added = model.defaults ? ARRAY_PUSH(out->models, out->count) : NULL;
			ok               = added != NULL;
			if (!ok) {
				XwauSkins_FreeNames(&model.skins);
				free(model.defaults);
				break;
			}
			memcpy(model.defaults, out->scratch, model.default_count * sizeof *model.defaults);
			*added = model;
		}
		FreeList(entries, count);
	}
	if (ok && out->count)
		qsort(out->models, out->count, sizeof *out->models, compare_skin_models);
	return ok;
}

/* A markings value whose variant is not the model's default one. */
typedef struct SkinChoice {
	size_t model;
	size_t markings;
	char   variant[1100]; /* "" for the model as modelled */
} SkinChoice;

/* Lists the markings values that the skin lines `lines` change, by model
 * and then by markings value. */
static bool skin_choices(const SkinModels* s, const XwauLines* lines, const char* base, SkinChoice** out,
						 size_t* count) {
	*out    = NULL;
	*count  = 0;
	bool ok = true;
	for (size_t m = 0; ok && m < s->count; ++m) {
		const SkinModel* model = &s->models[m];
		size_t           n     = 0;
		ok = XwauSkins_Markings(lines, model->key, model->states, &model->skins, s->scratch, &n);
		for (size_t i = 0; ok && i < n; ++i) {
			const XwauMarkings  none   = { false, 0, "" };
			const XwauMarkings* now    = &s->scratch[i];
			const XwauMarkings* before = i < model->default_count ? &model->defaults[i] : &none;
			if ((now->skinned == before->skinned && now->state == before->state &&
				 strcmp(now->list, before->list) == 0) ||
				(!now->skinned && !before->skinned))
				continue;
			SkinChoice* entry = ARRAY_PUSH(*out, *count);
			if (!(ok = entry != NULL))
				break;
			entry->model    = m;
			entry->markings = i;
			if (now->skinned) {
				snprintf(entry->variant, sizeof entry->variant, "%d|%s", now->state, now->list);
			} else {
				/* State 0 is the model as modelled. The other states are variants. */
				if (now->state)
					snprintf(entry->variant, sizeof entry->variant, "State %d", now->state);
				Report(base, "%s markings %zu has no skin but its default has one, so %s is used", model->key,
					   i, entry->variant[0] ? entry->variant : "the model as modelled");
			}
		}
	}
	return ok;
}

/* ---- Art ------------------------------------------------------------------ */

/* Reads the DATs that mission `base` adds (hook_backdrops): the lines of
 * <base>_Resdata.txt, else of the [Resdata] section of <base>.ini. Each one
 * that exists is written as the game names its mission art (mission_art.h):
 * its file name without the extension, in lower case, with any character
 * other than a letter, a digit, '_' or '-' made '_'. A name is listed once. */
static bool resdata_names(const char* game, const char* base, XwauLines* out) {
	memset(out, 0, sizeof *out);
	char      rel[1200], path[1024];
	XwauLines lines = { 0 };
	bool      ok =
		PathFormat(rel, sizeof rel, "%s_Resdata.txt", base) &&
		XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, &lines);
	if (ok && !lines.count) {
		XwauLines_Free(&lines);
		ok = PathFormat(rel, sizeof rel, "%s.ini", base) &&
			 XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Resdata",
							XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char line[512], found[1024], name[64];
		snprintf(line, sizeof line, "%s", lines.items[i]);
		const char* dat = Xwau_Trim(line, XWAU_RULES_CPP);
		if (!dat[0] || !Xwau_FindFile(game, dat, found, sizeof found))
			continue;
		const char* file = strrchr(found, '/');
		const char* dot  = strrchr(file ? file + 1 : found, '.');
		size_t      n    = 0;
		for (const char* c = file ? file + 1 : found; *c && c != dot && n + 1 < sizeof name; ++c) {
			const char lower = Xwau_Lower(*c);
			name[n++] = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-'
							? lower
							: '_';
		}
		name[n]   = 0;
		bool seen = false;
		for (size_t k = 0; !seen && k < out->count; ++k)
			seen = strcmp(out->items[k], name) == 0;
		if (!seen && n)
			ok = XwauLines_Add(out, name, n);
	}
	XwauLines_Free(&lines);
	return ok;
}

/* ---- Headlights ----------------------------------------------------------- */

/* Whether mission `base` turns the headlights on: `Enabled` of the
 * [Headlights] section of <mission>.ini, which ddraw reads when the mission
 * changes, read as std::stoi(value, 0, 0) reads it. A value that isn't a
 * number stops XWAU; it is reported, and the headlights stay off. Without
 * XWAU's ddraw.dll, the section does nothing. */
static bool headlights(const char* game, const char* base, bool* out) {
	*out = false;
	char rel[1100], path[1024];
	if (!Xwau_FindFile(game, "ddraw.dll", path, sizeof path))
		return true;
	XwauLines lines;
	if (!PathFormat(rel, sizeof rel, "%s.ini", base) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Headlights",
						XWAU_RULES_CPP, &lines))
		return false;
	char      text[256];
	long long value = 0;
	XwauLines_Value(&lines, "Enabled", XWAU_RULES_CPP, text, sizeof text);
	XwauLines_Free(&lines);
	if (text[0] && !Xwau_StoiAuto(text, &value))
		Report(base, "[Headlights]: Enabled: '%s' isn't a number, which XWAU would fail on; it is left out",
			   text);
	*out = value != 0;
	return true;
}

/* ---- Flight-group values -------------------------------------------------- */

typedef struct Totals {
	size_t files, profiles, profiles_dropped, skins, articulations, articulations_dropped, resdata, values,
		values_dropped, craft_markings, craft_markings_dropped, settings, swaps, craft_names, front_planets,
		headlights, stats_profiles;
} Totals;

/* Whether the flight group values of `entry` set anything. */
static bool has_values(const XwauMissionTieFlightGroup* entry) {
	return entry->has_markings || entry->has_iff || entry->has_pilot_voice;
}

/* Whether `name` holds a byte of 0x80 or more. The game compares the bytes of
 * the .TIE, and the file is written in UTF-8, so such a name can't be
 * checked. */
static bool non_ascii(const char* name) {
	for (const unsigned char* c = (const unsigned char*)name; *c; ++c)
		if (*c >= 0x80)
			return true;
	return false;
}

/* Whether the .TIE that plays has flight group `fg`. XWAU only reads flight
 * groups 0 to 191. */
static bool tie_has(const Tie* tie, int fg) { return fg >= 0 && fg < 192 && (size_t)fg < tie->count; }

static int compare_craft_markings(const void* a, const void* b) {
	const XwauMissionTieCraftMarkings* x = (const XwauMissionTieCraftMarkings*)a;
	const XwauMissionTieCraftMarkings* y = (const XwauMissionTieCraftMarkings*)b;
	if (x->fg != y->fg)
		return (x->fg > y->fg) - (x->fg < y->fg);
	return (x->craft > y->craft) - (x->craft < y->craft);
}

/* Drops, with a report, the values of the flight groups that the .TIE that
 * plays doesn't have, and cuts the values that XWAU cuts to one byte. The
 * markings of one craft are sorted by flight group and craft, and the dropped
 * ones get flight group -1. Returns the number of flight groups with values. */
static size_t check_values(XwauMissionTie* values, const Tie* tie, const char* base, Totals* totals) {
	for (size_t i = 0; i < values->fg_count; ++i) {
		XwauMissionTieFlightGroup* entry = &values->fgs[i];
		if (!has_values(entry))
			continue;
		if (!tie_has(tie, entry->fg)) {
			if (entry->fg >= 0 && entry->fg < 192)
				Report(base, "mission_tie: the mission has no flight group %d, so its values are dropped",
					   entry->fg);
			entry->has_markings = entry->has_iff = entry->has_pilot_voice = false;
			++totals->values_dropped;
			continue;
		}
		if (entry->has_markings && (entry->markings < 0 || entry->markings > 255)) {
			Report(base, "mission_tie: flight group %d: markings %d don't fit in a byte; XWAU keeps %d",
				   entry->fg, entry->markings, entry->markings & 0xFF);
			entry->markings &= 0xFF;
		}
		if (entry->has_iff && (entry->iff < 0 || entry->iff > 255)) {
			Report(base, "mission_tie: flight group %d: iff %d doesn't fit in a byte; XWAU keeps %d",
				   entry->fg, entry->iff, entry->iff & 0xFF);
			entry->iff &= 0xFF;
		}
	}
	for (size_t i = 0; i < values->craft_marking_count; ++i) {
		XwauMissionTieCraftMarkings* entry = &values->craft_markings[i];
		if (!tie_has(tie, entry->fg)) {
			if (entry->fg >= 0 && entry->fg < 192)
				Report(base,
					   "mission_tie: the mission has no flight group %d, so the markings of its craft %d "
					   "are dropped",
					   entry->fg, entry->craft);
			entry->fg = -1;
			++totals->craft_markings_dropped;
			continue;
		}
		/* The craft's number in its wave is never negative, nor more than 255
		   in the game. */
		if (entry->craft < 0 || entry->craft > 255) {
			Report(base, "mission_tie: flight group %d has no craft %d, so its markings are dropped",
				   entry->fg, entry->craft);
			entry->fg = -1;
			++totals->craft_markings_dropped;
			continue;
		}
		/* Kept: a skirmish can give the flight group more craft. */
		if (entry->craft >= tie->groups[entry->fg].craft_count)
			Report(base,
				   "mission_tie: flight group %d has %d craft in each wave, so the markings of its craft %d "
				   "do nothing unless it gets more",
				   entry->fg, tie->groups[entry->fg].craft_count, entry->craft);
		if (entry->markings < 0 || entry->markings > 255) {
			Report(base,
				   "mission_tie: flight group %d, craft %d: markings %d don't fit in a byte; XWAU keeps %d",
				   entry->fg, entry->craft, entry->markings, entry->markings & 0xFF);
			entry->markings &= 0xFF;
		}
	}
	if (values->craft_marking_count > 1)
		qsort(values->craft_markings, values->craft_marking_count, sizeof *values->craft_markings,
			  compare_craft_markings);
	size_t kept = 0;
	for (int fg = 0; (size_t)fg < tie->count; ++fg) {
		const XwauMissionTieFlightGroup* entry = XwauMissionTie_FlightGroup(values, fg);
		bool                             any   = entry && has_values(entry);
		for (size_t i = 0; !any && i < values->craft_marking_count; ++i)
			any = values->craft_markings[i].fg == fg;
		kept += any;
	}
	return kept;
}

/* Writes the `flight_groups` of the checked values. */
static void write_values(FILE* f, const XwauMissionTie* values, const Tie* tie, const char* base) {
	bool   any   = false;
	size_t craft = 0; /* the next markings of one craft */
	while (craft < values->craft_marking_count && values->craft_markings[craft].fg < 0)
		++craft;
	for (int i = 0; (size_t)i < tie->count; ++i) {
		const XwauMissionTieFlightGroup* entry = XwauMissionTie_FlightGroup(values, i);
		const bool                       set   = entry && has_values(entry);
		const bool by_craft = craft < values->craft_marking_count && values->craft_markings[craft].fg == i;
		if (!set && !by_craft)
			continue;
		const XwauTieFlightGroup* fg = &tie->groups[i];
		if (!any)
			fputs("flight_groups:\n", f);
		any = true;
		fprintf(f, "  %d: { ", i);
		if (non_ascii(fg->name)) {
			Report(base, "flight group %d: its name isn't ASCII, so its entry has no name check", i);
		} else {
			fputs("name: ", f);
			Xwau_WriteString(f, fg->name);
			fputs(", ", f);
		}
		fprintf(f, "craft: %d", fg->craft);
		if (set && entry->has_markings)
			fprintf(f, ", markings: %d", entry->markings);
		if (set && entry->has_iff)
			fprintf(f, ", iff: %d", entry->iff);
		if (set && entry->has_pilot_voice) {
			fputs(", pilot_voice: ", f);
			Xwau_WriteString(f, entry->pilot_voice);
		}
		for (bool first = true; craft < values->craft_marking_count && values->craft_markings[craft].fg == i;
			 ++craft, first = false)
			fprintf(f, "%s%d: %d", first ? ", craft_markings: { " : ", ", values->craft_markings[craft].craft,
					values->craft_markings[craft].markings);
		fputs(by_craft ? " } }\n" : " }\n", f);
	}
}

/* ---- Mission files -------------------------------------------------------- */

/* Returns the flight group of each selection, or -1 when the mission has no
 * such flight group or when an earlier selection has it. Those selections are
 * dropped and reported. */
static int* check_flight_groups(const long long* fgs, const char* const* labels, size_t count, const Tie* tie,
								const char* base, const char* what, size_t* dropped) {
	int* targets = (int*)malloc((count ? count : 1) * sizeof *targets);
	for (size_t i = 0; targets && i < count; ++i) {
		const bool exists = fgs[i] >= 0 && (size_t)fgs[i] < tie->count;
		bool       twice  = false;
		for (size_t k = 0; exists && k < i; ++k)
			twice = twice || fgs[k] == fgs[i];
		targets[i] = exists && !twice ? (int)fgs[i] : -1;
		if (targets[i] < 0) {
			Report(base, "%s flight group %lld, so its %s%s%s%s dropped",
				   exists ? "an earlier line already sets" : "the mission has no", fgs[i], what,
				   labels ? " '" : "", labels ? labels[i] : "", labels ? "' is" : " are");
			++*dropped;
		}
	}
	return targets;
}

/* Returns the indices of `targets` sorted by flight group. The dropped ones
 * (-1) come first, and the caller skips them. */
static size_t* by_target(const int* targets, size_t count) {
	size_t* order = (size_t*)malloc((count ? count : 1) * sizeof *order);
	for (size_t i = 0; order && i < count; ++i) {
		size_t at = i;
		for (; at > 0 && targets[order[at - 1]] > targets[i]; --at)
			order[at] = order[at - 1];
		order[at] = i;
	}
	return order;
}

static bool write_mission(const char* path, const char* base, const Profile* profile_list,
						  const int* profile_targets, size_t profile_count, const SkinModels* skin_models,
						  const SkinChoice* skins, size_t skin_count, const XwauFlightGroupArticulation* fgs,
						  const int* fg_targets, size_t fg_count, const Tie* tie, const XwauLines* resdata,
						  const XwauMissionTie* values, const XwauMissionSettings* settings,
						  const Swap* swap_list, size_t swap_count, const CraftName* names, size_t name_count,
						  const XwauFrontPlanet* planet, bool lights, const XwauMissionStats* stats) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		return false;
	}
	fprintf(f, "# Imported from XWAU's %s selections by xwau_import.\nversion: 1\n", base);
	for (size_t i = 0; i < resdata->count; ++i) {
		fputs(i ? ", " : "resdata: [", f);
		Xwau_WriteString(f, resdata->items[i]);
	}
	if (resdata->count)
		fputs("]\n", f);
	XwauMissionSettings_Write(f, settings);
	XwauFrontPlanet_Write(f, planet);
	if (lights)
		fputs("headlights: true\n", f);
	XwauMissionStats_Write(f, stats);
	for (size_t i = 0; i < swap_count; ++i)
		fprintf(f, "%s  %s: %s\n", i ? "" : "model_swaps:\n", swap_list[i].source, swap_list[i].target);
	write_craft_names(f, names, name_count);
	write_values(f, values, tie, base);
	size_t* order = by_target(profile_targets, profile_count);
	bool    any   = false;
	for (size_t k = 0; order && k < profile_count; ++k) {
		const size_t i = order[k];
		if (profile_targets[i] < 0)
			continue;
		if (!any)
			fputs("object_profiles:\n  flight_groups:\n", f);
		any                          = true;
		const XwauTieFlightGroup* fg = &tie->groups[profile_targets[i]];
		fprintf(f, "    %d: { profile: ", profile_targets[i]);
		Xwau_WriteString(f, profile_list[i].profile);
		fputs(", name: ", f);
		Xwau_WriteString(f, fg->name);
		fprintf(f, ", craft: %d }\n", fg->craft);
	}
	free(order);
	for (size_t i = 0; i < skin_count; ++i) {
		if (i == 0)
			fputs("skins:\n", f);
		if (i == 0 || skins[i].model != skins[i - 1].model) {
			fputs("  ", f);
			Xwau_WriteString(f, skin_models->models[skins[i].model].key);
			fputs(":\n", f);
		}
		fprintf(f, "    %zu: ", skins[i].markings);
		if (skins[i].variant[0])
			Xwau_WriteString(f, skins[i].variant);
		else
			fputc('~', f);
		fputc('\n', f);
	}
	order = by_target(fg_targets, fg_count);
	any   = false;
	for (size_t k = 0; order && k < fg_count; ++k) {
		const size_t i = order[k];
		if (fg_targets[i] < 0)
			continue;
		if (!any)
			fputs("articulations:\n  flight_groups:\n", f);
		any                          = true;
		const XwauTieFlightGroup* fg = &tie->groups[fg_targets[i]];
		fprintf(f, "    %d: { name: ", fg_targets[i]);
		Xwau_WriteString(f, fg->name);
		fprintf(f, ", craft: %d", fg->craft);
		if (fgs[i].variant[0]) {
			fputs(", variant: ", f);
			Xwau_WriteString(f, fgs[i].variant);
		}
		for (size_t s = 0; s < fgs[i].start_count; ++s)
			fprintf(f, "%s%s", s ? ", " : ", start: [", fgs[i].start[s]);
		fputs(fgs[i].start_count ? "] }\n" : " }\n", f);
	}
	free(order);
	const bool ok = !ferror(f) && fclose(f) == 0;
	if (!ok)
		Report(NULL, "cannot write %s", path);
	return ok;
}

/* Imports the selections of mission `name` in folder `dir`, and writes its
 * file when it has some. */
static bool import_mission(const char* game, const XwauMissions* missions, const char* stock_game,
						   const char* dir, const char* name, const char* out, SkinModels* skin_models,
						   bool* skins_loaded, const XwauSettingsConfig* config,
						   const XwauMissionNumbers* numbers, const char* models, const XwauLines* keys,
						   const XwauGameTables* tables, XwauDatGroups* dat_groups, bool* dat_groups_loaded,
						   Totals* totals) {
	char base[1100];
	if (!PathFormat(base, sizeof base, "%s/%s", dir, name))
		return false;
	Profile*    profile_list  = NULL;
	size_t      profile_count = 0;
	SkinChoice* skins         = NULL;
	size_t      skin_count    = 0;
	XwauLines   lines         = { 0 };
	bool        ok =
		profiles(game, base, &profile_list, &profile_count) && XwauMissions_OwnSkinLines(game, base, &lines);
	if (ok && lines.count && !*skins_loaded) {
		ok            = load_skin_models(game, missions, skin_models);
		*skins_loaded = true;
	}
	ok = ok && (!lines.count || skin_choices(skin_models, &lines, base, &skins, &skin_count));
	XwauLines_Free(&lines);
	const XwauFlightGroupArticulation* fgs      = NULL;
	const size_t                       fg_count = XwauMissions_FlightGroupArticulations(missions, base, &fgs);
	size_t                             skinned_models = 0;
	for (size_t i = 0; i < skin_count; ++i)
		skinned_models += i == 0 || skins[i].model != skins[i - 1].model;
	XwauLines      resdata = { 0 };
	XwauMissionTie values  = { 0 };
	/* XwauMissions_Load already reports the problems of these lines. */
	ok = ok && resdata_names(game, base, &resdata) && XwauMissionTie_Read(game, base, false, &values);
	XwauMissionSettings settings = { 0 };
	ok               = ok && XwauMissionSettings_Read(game, base, XwauMissionNumbers_Find(numbers, dir, name),
													  &values.settings, config, &settings);
	Swap*  swap_list = NULL;
	size_t swap_count = 0;
	ok                = ok && swaps(game, base, models, keys, &swap_list, &swap_count);
	XwauFrontPlanet planet;
	ok          = ok && XwauFrontPlanet_Read(game, base, &planet);
	bool lights = false;
	ok          = ok && headlights(game, base, &lights);
	XwauMissionStats stats = { 0 };
	ok                     = ok && XwauMissionStats_Read(game, base, &stats);
	const bool any_stats   = XwauMissionStats_Any(&stats);
	if (ok && planet.image != -1 && !*dat_groups_loaded) {
		ok                 = XwauDatGroups_Read(game, stock_game, dat_groups);
		*dat_groups_loaded = true;
	}
	if (ok && planet.image != -1 && dat_groups->count && !XwauDatGroups_Has(dat_groups, planet.image))
		Report(base, "[concourse]: no DAT of RESDATA.TXT has group %d, so the concourse shows no planet",
			   planet.image);
	bool any_values = values.craft_marking_count != 0;
	for (size_t i = 0; i < values.fg_count; ++i)
		any_values |= has_values(&values.fgs[i]);
	if (!ok || (!profile_count && !skin_count && !fg_count && !resdata.count && !any_values &&
				!XwauMissionSettings_Any(&settings) && !swap_count && !values.craft_lines.count &&
				!XwauFrontPlanet_Any(&planet) && !lights && !any_stats)) {
		XwauMissionStats_Free(&stats);
		free_profiles(profile_list, profile_count);
		free(skins);
		XwauLines_Free(&resdata);
		XwauMissionTie_Free(&values);
		XwauMissionSettings_Free(&settings);
		free(swap_list);
		return ok;
	}

	/* The .TIE that plays: the mod's, else the game's. */
	char rel[1200], tie_path[1024];
	Tie  tie = { 0 };
	if (PathFormat(rel, sizeof rel, "%s.tie", base) &&
		(Xwau_FindFile(game, rel, tie_path, sizeof tie_path) ||
		 Xwau_FindFile(stock_game, rel, tie_path, sizeof tie_path)))
		tie.groups = XwauTie_FlightGroups(tie_path, &tie.count);
	if (!tie.groups) {
		Report(base,
			   "%zu object profile selections, %zu skinned models and %zu flight groups with "
			   "articulations are dropped because neither the mod nor the game has a .TIE for it",
			   profile_count, skinned_models, fg_count);
		totals->profiles_dropped += profile_count;
		totals->articulations_dropped += fg_count;
		XwauMissionStats_Free(&stats);
		free_profiles(profile_list, profile_count);
		free(skins);
		XwauLines_Free(&resdata);
		XwauMissionTie_Free(&values);
		XwauMissionSettings_Free(&settings);
		free(swap_list);
		return true;
	}
	long long*   profile_fgs = (long long*)malloc((profile_count ? profile_count : 1) * sizeof *profile_fgs);
	const char** profile_labels =
		(const char**)malloc((profile_count ? profile_count : 1) * sizeof *profile_labels);
	long long* articulated = (long long*)malloc((fg_count ? fg_count : 1) * sizeof *articulated);
	for (size_t i = 0; profile_fgs && profile_labels && i < profile_count; ++i) {
		profile_fgs[i]    = profile_list[i].fg;
		profile_labels[i] = profile_list[i].profile;
	}
	for (size_t i = 0; articulated && i < fg_count; ++i)
		articulated[i] = fgs[i].flight_group;
	int* profile_targets = profile_fgs && profile_labels
							   ? check_flight_groups(profile_fgs, profile_labels, profile_count, &tie, base,
													 "object profile", &totals->profiles_dropped)
							   : NULL;
	int* fg_targets      = articulated ? check_flight_groups(articulated, NULL, fg_count, &tie, base,
															 "articulations", &totals->articulations_dropped)
									   : NULL;
	free(profile_fgs);
	free(profile_labels);
	free(articulated);
	size_t profiles_kept = 0, fgs_kept = 0;
	for (size_t i = 0; profile_targets && i < profile_count; ++i)
		profiles_kept += profile_targets[i] >= 0;
	for (size_t i = 0; fg_targets && i < fg_count; ++i)
		fgs_kept += fg_targets[i] >= 0;
	ok                       = profile_targets && fg_targets;
	const size_t values_kept = check_values(&values, &tie, base, totals);
	XwauMissionSettings_CheckFlightGroups(&settings, tie.count, base);
	const bool any_settings = XwauMissionSettings_Any(&settings);
	CraftName* names        = NULL;
	size_t     name_count   = 0;
	ok                      = ok && craft_names(&values.craft_lines, tables, &tie, base, &names, &name_count);
	if (ok && (profiles_kept || skin_count || fgs_kept || resdata.count || values_kept || any_settings ||
			   swap_count || name_count || XwauFrontPlanet_Any(&planet) || lights || any_stats)) {
		char dir_path[1200], lower_dir[64], path[1400];
		LowerCopy(lower_dir, sizeof lower_dir, dir);
		ok = PathFormat(dir_path, sizeof dir_path, "%s/%s", out, lower_dir) &&
			 PathFormat(path, sizeof path, "%s/%s.yaml", dir_path, name) && MakeDirs(dir_path) &&
			 write_mission(path, base, profile_list, profile_targets, profile_count, skin_models, skins,
						   skin_count, fgs, fg_targets, fg_count, &tie, &resdata, &values, &settings,
						   swap_list, swap_count, names, name_count, &planet, lights, &stats);
		if (ok) {
			++totals->files;
			totals->profiles += profiles_kept;
			totals->skins += skin_count;
			totals->articulations += fgs_kept;
			totals->resdata += resdata.count != 0;
			totals->values += values_kept;
			totals->settings += any_settings;
			totals->swaps += swap_count;
			totals->craft_names += name_count;
			totals->front_planets += XwauFrontPlanet_Any(&planet);
			totals->headlights += lights;
			totals->stats_profiles += any_stats;
			for (size_t i = 0; i < values.craft_marking_count; ++i)
				totals->craft_markings += values.craft_markings[i].fg >= 0;
		}
	}
	free_craft_names(names, name_count);
	XwauMissionStats_Free(&stats);
	free(profile_targets);
	free(fg_targets);
	free_profiles(profile_list, profile_count);
	free(skins);
	free(tie.groups);
	XwauLines_Free(&resdata);
	XwauMissionTie_Free(&values);
	XwauMissionSettings_Free(&settings);
	free(swap_list);
	return ok;
}

bool XwauMissionFiles_Write(const char* game, const XwauMissions* missions, const char* stock_game,
							const char* out) {
	SkinModels         skin_models  = { 0 };
	bool               skins_loaded = false;
	Totals             totals       = { 0 };
	XwauSettingsConfig config       = { 0 };
	XwauMissionNumbers numbers      = { 0 };
	XwauLines          keys         = { 0 };
	/* The swaps' GLBs: the mission files go into <mod>/flight/missions, and
	   the models into <mod>/flight/models, next to it. */
	char        models[1100];
	const char* slash = strrchr(out, '/');
	bool ok = PathFormat(models, sizeof models, "%.*smodels", slash ? (int)(slash + 1 - out) : 0, out) &&
			  XwauSettingsConfig_Read(game, &config) && XwauMissionNumbers_Read(game, stock_game, &numbers) &&
			  list_keys(game, &keys);
	/* For the craft names: which model each craft record has. Without the
	   tables (reported), the names are dropped. */
	XwauGameTables* tables = ok ? XwauGameTables_Load(game) : NULL;
	/* For the concourse planets: the image groups of the DATs, read when a
	   mission sets one. */
	XwauDatGroups dat_groups        = { 0 };
	bool          dat_groups_loaded = false;
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(game, kMissionDirs[d], &names);
		/* The missions of the lists too: some settings have defaults by
		   mission number, also for missions that the mod doesn't change. */
		for (size_t i = 0; ok && config.mission_tie && i < numbers.count; ++i) {
			bool seen = strcasecmp(numbers.items[i].dir, kMissionDirs[d]) != 0;
			for (size_t n = 0; !seen && n < names.count; ++n)
				seen = strcmp(names.items[n], numbers.items[i].name) == 0;
			if (!seen)
				ok = XwauLines_Add(&names, numbers.items[i].name, strlen(numbers.items[i].name));
		}
		for (size_t n = 0; ok && n < names.count; ++n)
			ok = import_mission(game, missions, stock_game, kMissionDirs[d], names.items[n], out,
								&skin_models, &skins_loaded, &config, &numbers, models, &keys, tables,
								&dat_groups, &dat_groups_loaded, &totals);
		XwauLines_Free(&names);
	}
	free_skin_models(&skin_models);
	XwauSettingsConfig_Free(&config);
	XwauMissionNumbers_Free(&numbers);
	XwauLines_Free(&keys);
	XwauGameTables_Free(tables);
	XwauDatGroups_Free(&dat_groups);
	if (!ok)
		return false;
	Report(NULL,
		   "wrote %zu mission files with %zu object profile selections (%zu dropped), %zu skin "
		   "selections, %zu flight groups with articulations (%zu dropped), %zu flight groups with "
		   "[mission_tie] values (%zu dropped), the markings of %zu craft (%zu dropped), the settings of "
		   "%zu missions, %zu model swaps, the names of %zu models, the concourse planets of %zu missions, "
		   "the headlights of %zu missions, the stats profiles of %zu missions and the art files of %zu "
		   "missions",
		   totals.files, totals.profiles, totals.profiles_dropped, totals.skins, totals.articulations,
		   totals.articulations_dropped, totals.values, totals.values_dropped, totals.craft_markings,
		   totals.craft_markings_dropped, totals.settings, totals.swaps, totals.craft_names,
		   totals.front_planets, totals.headlights, totals.stats_profiles, totals.resdata);
	return true;
}
