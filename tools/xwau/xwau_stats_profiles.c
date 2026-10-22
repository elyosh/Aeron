#include "xwau_stats_profiles.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_missions.h"

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

/* The game's keys, then XWAU's keys, in the order of stats_profiles.h's
   fields. */
static const char* const kFieldKeys[XWAU_STATS_FIELDS] = {
	"speed",
	"acceleration",
	"deceleration",
	"pitch_rate",
	"roll_rate",
	"yaw_rate",
	"system_damage_hull_threshold",
	"hull",
	"system_strength",
	"shields",
	"speed_increment",
	"speed_decrement",
	"has_shields",
	"has_hyperdrive",
	"warhead_collision_damage",
};
static const char* const kXwauKeys[XWAU_STATS_FIELDS] = {
	"Speed",
	"Acceleration",
	"Deceleration",
	"Pitch",
	"Roll",
	"Yaw",
	"CriticalDamageThreshold",
	"HullStrength",
	"SystemStrength",
	"ShieldStrength",
	"SpeedIncrement",
	"SpeedDecrement",
	"HasShieldGenerator",
	"HasHyperdrive",
	"IsWarheadCollisionDamagesEnabled",
};

enum { FIELD_THRESHOLD = 6, FIELD_HULL = 7, FIELD_SHIELDS = 9, FIELD_HAS_SHIELDS = 12, FIELD_COLLISION = 14 };

/* Reads `key` as GetFileKeyValueInt reads it, or returns `fallback` when
 * the key is missing. A value that isn't a number stops XWAU (std::stoi
 * throws); it is reported, and `fallback` is used. */
static long long int_key(const XwauLines* lines, const char* source, const char* key, long long fallback) {
	char      text[256];
	long long value = 0;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (!text[0])
		return fallback;
	if (!Xwau_StoiAuto(text, &value) || value < INT_MIN || value > INT_MAX) {
		Report(source, "%s = %s isn't a number, which XWAU would fail on; it is ignored", key, text);
		return fallback;
	}
	return value;
}

static void lower_name(char* out, size_t cap, const char* text) {
	size_t i = 0;
	for (; text[i] && i + 1 < cap; ++i)
		out[i] = Xwau_Lower(text[i]);
	out[i] = 0;
}

static bool starts_with_name(const char* text, const char* prefix) {
	return strncasecmp(text, prefix, strlen(prefix)) == 0;
}

/* Whether `text` contains `part`, whatever the case. */
static bool contains_name(const char* text, const char* part) {
	for (; *text; ++text)
		if (starts_with_name(text, part))
			return true;
	return false;
}

/* ---- Genus classes ---------------------------------------------------------- */

static int genus_class(int genus) {
	if (genus == 4 || genus == 5)
		return 2;
	if (genus == 3 || genus == 17)
		return 1;
	return 0;
}

static bool genus_add(XwauStatsGenus* genus, const char* key, int cls) {
	for (size_t i = 0; i < genus->count; ++i) {
		if (strcmp(genus->items[i].key, key) == 0) {
			if (genus->items[i].genus_class != cls)
				genus->items[i].genus_class = -2;
			return true;
		}
	}
	struct XwauStatsGenusModel* added = ARRAY_PUSH(genus->items, genus->count);
	if (!added)
		return false;
	snprintf(added->key, sizeof added->key, "%s", key);
	added->genus_class = cls;
	return true;
}

/* The [Objects] lines of mission `base`: <mission>_Objects.txt, else the
 * section of <mission>.ini. */
static bool object_lines(const char* game, const char* base, XwauLines* lines) {
	char rel[1100], path[1024];
	snprintf(rel, sizeof rel, "%s_Objects.txt", base);
	if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, lines))
		return false;
	if (lines->count)
		return true;
	snprintf(rel, sizeof rel, "%s.ini", base);
	return XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Objects",
						  XWAU_RULES_CPP, lines);
}

/* Gives the targets of the swaps of mission `base` the classes of the
 * object types that load their sources. */
static bool genus_swaps(const char* game, const char* base, const XwauGameTables* tables,
						XwauStatsGenus* genus) {
	XwauLines lines;
	bool      ok = object_lines(game, base, &lines);
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char        line[2048], source[256], target[256], rel[1024], path[1024];
		char*       equals;
		const char* key;
		const char* value;
		snprintf(line, sizeof line, "%s", lines.items[i]);
		if (!(equals = strchr(line, '=')))
			continue;
		*equals = 0;
		key     = Xwau_Trim(line, XWAU_RULES_CPP);
		value   = Xwau_Trim(equals + 1, XWAU_RULES_CPP);
		snprintf(rel, sizeof rel, "%s", value);
		for (char* c = rel; *c; ++c)
			if (*c == '\\')
				*c = '/';
		if (!EndsWithNoCase(key, ".opt") || !Xwau_ModelKey(key, source, sizeof source) || !value[0] ||
			!Xwau_FindFile(game, rel, path, sizeof path) || !Xwau_ModelKey(value, target, sizeof target))
			continue;
		for (int t = 0; ok && t < XWAU_GAME_OBJECT_TYPES; ++t) {
			const XwauGameType type = XwauGameTables_Type(tables, t);
			if (strcmp(XwauGameTables_Model(tables, t), source) == 0)
				ok = genus_add(genus, target, genus_class(type.genus));
		}
	}
	XwauLines_Free(&lines);
	return ok;
}

bool XwauStatsGenus_Read(const char* game, const XwauGameTables* tables, XwauStatsGenus* out) {
	memset(out, 0, sizeof *out);
	if (!tables)
		return true;
	bool ok = true;
	for (int t = 0; ok && t < XWAU_GAME_OBJECT_TYPES; ++t) {
		const char* model = XwauGameTables_Model(tables, t);
		if (model[0])
			ok = genus_add(out, model, genus_class(XwauGameTables_Type(tables, t).genus));
	}
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(game, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n) {
			char base[300];
			snprintf(base, sizeof base, "%s/%s", kMissionDirs[d], names.items[n]);
			ok = genus_swaps(game, base, tables, out);
		}
		XwauLines_Free(&names);
	}
	return ok;
}

void XwauStatsGenus_Free(XwauStatsGenus* genus) {
	free(genus->items);
	memset(genus, 0, sizeof *genus);
}

int XwauStatsGenus_Find(const XwauStatsGenus* genus, const char* key) {
	for (size_t i = 0; i < genus->count; ++i)
		if (strcmp(genus->items[i].key, key) == 0)
			return genus->items[i].genus_class;
	return -1;
}

/* ---- Model profiles ------------------------------------------------------------- */

/* Adds the names of the sections of the INI `path` to `out`. */
static bool section_names(const char* path, XwauLines* out) {
	size_t size = 0;
	char*  text = path ? ReadWholeFile(path, &size) : NULL;
	bool   ok   = true;
	for (char* line = text; ok && line && *line;) {
		char* end = strchr(line, '\n');
		if (end)
			*end = 0;
		const char*  trimmed = Xwau_Trim(line, XWAU_RULES_CPP);
		const size_t len     = strlen(trimmed);
		if (len > 2 && trimmed[0] == '[' && trimmed[len - 1] == ']')
			ok = XwauLines_Add(out, trimmed + 1, len - 2);
		line = end ? end + 1 : NULL;
	}
	free(text);
	return ok;
}

static bool add_name(XwauLines* names, const char* name) {
	for (size_t i = 0; i < names->count; ++i)
		if (Xwau_SameName(names->items[i], name))
			return true;
	return XwauLines_Add(names, name, strlen(name));
}

/* The lines of profile `name` of model `model`: its .txt file, else its
 * section. */
static bool profile_lines(const char* game, const char* model, const char* name, XwauLines* lines,
						  char* source, size_t cap) {
	char rel[1024], path[1024];
	snprintf(rel, sizeof rel, "FlightModels/%sStatsProfile_%s.txt", model, name);
	snprintf(source, cap, "%s", rel);
	if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, lines))
		return false;
	if (lines->count)
		return true;
	char section[300];
	snprintf(rel, sizeof rel, "FlightModels/%s.ini", model);
	snprintf(section, sizeof section, "StatsProfile_%s", name);
	snprintf(source, cap, "%s [%s]", rel, section);
	return XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, section, XWAU_RULES_CPP,
						  lines);
}

/* Reports the keys of `lines` that the hook doesn't read. */
static void report_stray_keys(const XwauLines* lines, const char* source) {
	for (size_t i = 0; i < lines->count; ++i) {
		char        key[256];
		const char* equals = strchr(lines->items[i], '=');
		snprintf(key, sizeof key, "%.*s",
				 equals ? (int)(equals - lines->items[i]) : (int)strlen(lines->items[i]), lines->items[i]);
		Xwau_Trim(key, XWAU_RULES_CPP);
		bool known = Xwau_SameName(key, "ExplosionStrength");
		for (int f = 0; !known && f < XWAU_STATS_FIELDS; ++f)
			known = Xwau_SameName(key, kXwauKeys[f]);
		if (!known)
			Report(source, "'%s' isn't a stats profile key; it is ignored", lines->items[i]);
	}
}

/* Reads one profile. Returns false when it needs the genus division and
 * `genus_class` doesn't give one. */
static bool read_profile(const XwauLines* lines, const char* source, int genus_class, XwauStatsProfile* p) {
	for (int f = 0; f < XWAU_STATS_FIELDS; ++f) {
		long long value;
		if (f == FIELD_THRESHOLD)
			value = int_key(lines, source, kXwauKeys[f], int_key(lines, source, "ExplosionStrength", -1));
		else if (f == FIELD_COLLISION)
			value = int_key(lines, source, kXwauKeys[f], 1) != 0 ? -1 : 0;
		else
			value = int_key(lines, source, kXwauKeys[f], -1);
		if (value == -1)
			continue;
		if (f <= 2) {
			value = (int)((float)value * 2.25f + 0.5f);
		} else if (f <= 5) {
			value = (int32_t)((uint32_t)value * 256u);
		} else if (f == FIELD_THRESHOLD || f == FIELD_HULL || f == FIELD_SHIELDS) {
			int32_t scaled = (int32_t)((uint32_t)value * (f == FIELD_SHIELDS ? 50u : 105u));
			if (genus_class < 0)
				return false;
			value = genus_class == 2 ? scaled / 16 : genus_class == 1 ? scaled / 4 : scaled;
		} else if (f >= FIELD_HAS_SHIELDS) {
			value = value != 0;
		}
		p->set[f]   = true;
		p->value[f] = value;
	}
	return true;
}

bool XwauStatsProfiles_Read(const char* game, const char* model, char* const* names, size_t name_count,
							int genus_class, XwauStatsProfiles* out) {
	memset(out, 0, sizeof *out);
	char      rel[1024], ini[1024], prefix[300];
	XwauLines sections = { 0 }, profile_names = { 0 };
	size_t    unknown_units = 0;
	snprintf(rel, sizeof rel, "FlightModels/%s.ini", model);
	bool ok = section_names(Xwau_FindFile(game, rel, ini, sizeof ini) ? ini : NULL, &sections);
	for (size_t i = 0; ok && i < sections.count; ++i) {
		const char* section = sections.items[i];
		if (starts_with_name(section, "StatsProfile_") && section[13] && !strpbrk(section, ";[]"))
			ok = add_name(&profile_names, section + 13);
		else if (starts_with_name(section, "StatsModifiers"))
			Report(rel, "[%s] isn't imported: OpenXWA has no stats modifiers", section);
		else if (contains_name(section, "StatsProfile"))
			out->disabled++;
	}
	snprintf(prefix, sizeof prefix, "%sStatsProfile_", model);
	for (size_t i = 0; ok && i < name_count; ++i) {
		const size_t len = strlen(names[i]);
		if (starts_with_name(names[i], prefix) && EndsWithNoCase(names[i], ".txt") &&
			len > strlen(prefix) + 4) {
			char name[300];
			snprintf(name, sizeof name, "%.*s", (int)(len - strlen(prefix) - 4), names[i] + strlen(prefix));
			ok = add_name(&profile_names, name);
		} else if (starts_with_name(names[i], model) &&
				   starts_with_name(names[i] + strlen(model), "StatsModifiers")) {
			Report(names[i], "isn't imported: OpenXWA has no stats modifiers");
		}
	}
	for (size_t i = 0; ok && i < profile_names.count; ++i) {
		XwauLines lines;
		char      source[1300];
		ok = profile_lines(game, model, profile_names.items[i], &lines, source, sizeof source);
		if (!ok || !lines.count) {
			XwauLines_Free(&lines);
			continue;
		}
		XwauStatsProfile profile;
		memset(&profile, 0, sizeof profile);
		lower_name(profile.name, sizeof profile.name, profile_names.items[i]);
		report_stray_keys(&lines, source);
		if (strlen(profile_names.items[i]) >= XWAU_STATS_NAME_MAX) {
			Report(source, "the profile name is longer than %d characters; it is left out",
				   XWAU_STATS_NAME_MAX - 1);
		} else if (!read_profile(&lines, source, genus_class, &profile)) {
			++unknown_units;
		} else {
			XwauStatsProfile* added = ARRAY_PUSH(out->items, out->count);
			if ((ok = added != NULL))
				*added = profile;
		}
		XwauLines_Free(&lines);
	}
	if (unknown_units)
		Report(
			rel,
			genus_class == -2
				? "object types of different genus classes load the model, so the hull and shield units of "
				  "%zu profiles are unknown; they are left out"
				: "no object type loads the model, so the hull and shield units of %zu profiles are unknown; "
				  "they are left out",
			unknown_units);
	XwauLines_Free(&sections);
	XwauLines_Free(&profile_names);
	return ok;
}

void XwauStatsProfiles_Free(XwauStatsProfiles* profiles) {
	free(profiles->items);
	memset(profiles, 0, sizeof *profiles);
}

void XwauStatsProfiles_Write(FILE* f, const XwauStatsProfiles* profiles) {
	if (!profiles->count)
		return;
	fputs("stats_profiles:\n", f);
	for (size_t i = 0; i < profiles->count; ++i) {
		const XwauStatsProfile* p = &profiles->items[i];
		fputs("  ", f);
		Xwau_WriteString(f, p->name);
		fputs(": {", f);
		bool first = true;
		for (int field = 0; field < XWAU_STATS_FIELDS; ++field) {
			if (!p->set[field])
				continue;
			fprintf(f, "%s%s: ", first ? "" : ", ", kFieldKeys[field]);
			if (field >= FIELD_HAS_SHIELDS)
				fputs(p->value[field] ? "true" : "false", f);
			else
				fprintf(f, "%lld", p->value[field]);
			first = false;
		}
		fputs("}\n", f);
	}
}

/* ---- Mission choices ------------------------------------------------------------ */

/* Whether model `model` has profile `name` or one of its variants. */
static bool has_profile(const char* game, const char* model, const char* name) {
	static const char* const variants[] = { "",        "_Easy",        "_Medium",        "_Hard",
											"_Player", "_Player_Easy", "_Player_Medium", "_Player_Hard" };
	bool                     found      = false;
	for (size_t v = 0; !found && v < sizeof variants / sizeof variants[0]; ++v) {
		char      full[300], source[1300];
		XwauLines lines;
		snprintf(full, sizeof full, "%s%s", name, variants[v]);
		found = profile_lines(game, model, full, &lines, source, sizeof source) && lines.count;
		XwauLines_Free(&lines);
	}
	return found;
}

static struct XwauMissionStatsModel* mission_model(XwauMissionStats* stats, const char* key) {
	for (size_t i = 0; i < stats->model_count; ++i)
		if (strcmp(stats->models[i].key, key) == 0)
			return &stats->models[i];
	struct XwauMissionStatsModel* added = ARRAY_PUSH(stats->models, stats->model_count);
	if (added) {
		memset(added, 0, sizeof *added);
		snprintf(added->key, sizeof added->key, "%s", key);
	}
	return added;
}

/* Whether `key` is a percent key, which the model lines don't take. */
static bool percent_key(const char* key) {
	char name[300];
	for (int player = 0; player < 2; ++player) {
		for (int f = 0; f <= XWAU_STATS_PERCENT_FIELDS; ++f) {
			snprintf(name, sizeof name, "%s%sPercent", player ? "Player" : "",
					 f < XWAU_STATS_PERCENT_FIELDS ? kXwauKeys[f] : "ExplosionStrength");
			if (Xwau_SameName(key, name))
				return true;
		}
	}
	return false;
}

static void read_percents(const XwauLines* lines, const char* base, const char* prefix,
						  long long percent[XWAU_STATS_PERCENT_FIELDS]) {
	for (int f = 0; f < XWAU_STATS_PERCENT_FIELDS; ++f) {
		char key[300], alias[300];
		snprintf(key, sizeof key, "%s%sPercent", prefix, kXwauKeys[f]);
		snprintf(alias, sizeof alias, "%sExplosionStrengthPercent", prefix);
		percent[f] = int_key(lines, base, key, f == FIELD_THRESHOLD ? int_key(lines, base, alias, -1) : -1);
		if (percent[f] < -1) {
			Report(base, "%s = %lld: a negative percent isn't imported", key, percent[f]);
			percent[f] = -1;
		}
	}
}

/* Reads `text` as markings written as std::to_string writes them, or
 * returns -1. */
static int canonical_markings(const char* text) {
	if (!*text || strspn(text, "0123456789") != strlen(text) || (text[0] == '0' && text[1]) ||
		strlen(text) > 3)
		return -1;
	const int value = atoi(text);
	return value <= 255 ? value : -1;
}

bool XwauMissionStats_Read(const char* game, const char* base, XwauMissionStats* out) {
	memset(out, 0, sizeof *out);
	char      rel[1100], path[1024];
	XwauLines lines;
	snprintf(rel, sizeof rel, "%s_StatsProfiles.txt", base);
	if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	if (!lines.count) {
		snprintf(rel, sizeof rel, "%s.ini", base);
		if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "StatsProfiles",
							XWAU_RULES_CPP, &lines))
			return false;
	}
	read_percents(&lines, base, "", out->percent);
	read_percents(&lines, base, "Player", out->player_percent);
	XwauLines seen = { 0 };
	bool      ok   = true;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char  line[2048], key[300], value[300];
		char* equals;
		snprintf(line, sizeof line, "%s", lines.items[i]);
		if (!(equals = strchr(line, '=')))
			continue;
		*equals = 0;
		snprintf(key, sizeof key, "%s", Xwau_Trim(line, XWAU_RULES_CPP));
		snprintf(value, sizeof value, "%s", Xwau_Trim(equals + 1, XWAU_RULES_CPP));
		bool already = false;
		for (size_t s = 0; !already && s < seen.count; ++s)
			already = Xwau_SameName(seen.items[s], key);
		if (already || !key[0] || percent_key(key))
			continue;
		if (!(ok = XwauLines_Add(&seen, key, strlen(key))))
			break;
		if (!value[0])
			continue;
		char         lower[300], model[300];
		const size_t len      = strlen(key);
		int          markings = -1;
		bool         player   = false;
		lower_name(lower, sizeof lower, key);
		const char* fgc = NULL;
		for (const char* at = strstr(lower, "_fgc_"); at; at = strstr(at + 1, "_fgc_"))
			fgc = at;
		const char* fg = NULL;
		for (const char* at = strstr(lower, "_fg_"); at; at = strstr(at + 1, "_fg_"))
			fg = at;
		if (len > 10 && strcmp(lower + len - 10, "_fg_player") == 0) {
			player = true;
			snprintf(model, sizeof model, "%.*s", (int)(len - 10), lower);
		} else if (fgc) {
			if ((markings = canonical_markings(fgc + 5)) < 0) {
				Report(base, "[StatsProfiles]: '%s' isn't a key that XWAU reads; it is left out", key);
				continue;
			}
			snprintf(model, sizeof model, "%.*s", (int)(fgc - lower), lower);
		} else if (fg && fg[4] && strspn(fg + 4, "0123456789") == strlen(fg + 4)) {
			Report(base,
				   "[StatsProfiles]: '%s' isn't a key that XWAU reads (it reads _fgc_<markings>); it is "
				   "left out",
				   key);
			continue;
		} else {
			snprintf(model, sizeof model, "%s", lower);
		}
		if (!model[0] || strpbrk(model, "\\/ ")) {
			Report(base, "[StatsProfiles]: '%s' doesn't name a model; it is left out", key);
			continue;
		}
		const bool found = has_profile(game, model, value);
		if (!found)
			Report(base,
				   "[StatsProfiles]: %s = %s: the model has no such profile%s; the craft get no profile, not "
				   "even the default one",
				   key, value, strchr(value, ';') ? " (the value keeps its comment, as in XWAU)" : "");
		/* A name that names no profile still keeps the default from
		   applying, so a long one is cut rather than left out. */
		if (strlen(value) >= XWAU_STATS_NAME_MAX)
			Report(base, "[StatsProfiles]: %s: the name is longer than %d characters, so it is cut%s", key,
				   XWAU_STATS_NAME_MAX - 1, found ? " and the craft get no profile" : "");
		struct XwauMissionStatsModel* entry = mission_model(out, model);
		if (!(ok = entry != NULL))
			break;
		char* target = player ? entry->player : markings >= 0 ? entry->markings[markings] : entry->all;
		lower_name(target, XWAU_STATS_NAME_MAX, value);
		/* A tab can only come before a comment, so the name matches no
		   profile either way. */
		for (char* c = target; *c; ++c)
			if (*c == '\t')
				*c = ' ';
		if (markings >= 0)
			entry->has_markings[markings] = true;
	}
	XwauLines_Free(&seen);
	XwauLines_Free(&lines);
	return ok;
}

bool XwauMissionStats_Any(const XwauMissionStats* stats) {
	bool any = stats->model_count != 0;
	for (int f = 0; !any && f < XWAU_STATS_PERCENT_FIELDS; ++f)
		any = stats->percent[f] != -1 || stats->player_percent[f] != -1;
	return any;
}

void XwauMissionStats_Write(FILE* f, const XwauMissionStats* stats) {
	if (!XwauMissionStats_Any(stats))
		return;
	fputs("stats_profiles:\n", f);
	if (stats->model_count)
		fputs("  models:\n", f);
	for (size_t i = 0; i < stats->model_count; ++i) {
		const struct XwauMissionStatsModel* m     = &stats->models[i];
		bool                                first = true;
		fprintf(f, "    %s: {", m->key);
		if (m->player[0]) {
			fputs("player: ", f);
			Xwau_WriteString(f, m->player);
			first = false;
		}
		bool open = false;
		for (int k = 0; k < 256; ++k) {
			if (!m->has_markings[k])
				continue;
			fprintf(f, "%s%d: ", open ? ", " : first ? "markings: {" : ", markings: {", k);
			Xwau_WriteString(f, m->markings[k]);
			open  = true;
			first = false;
		}
		if (open)
			fputc('}', f);
		if (m->all[0]) {
			fputs(first ? "all: " : ", all: ", f);
			Xwau_WriteString(f, m->all);
		}
		fputs("}\n", f);
	}
	for (int g = 0; g < 2; ++g) {
		const long long* percent = g ? stats->player_percent : stats->percent;
		bool             first   = true;
		for (int field = 0; field < XWAU_STATS_PERCENT_FIELDS; ++field) {
			if (percent[field] == -1)
				continue;
			fprintf(f, "%s%s: %lld", first ? (g ? "  player_percent: {" : "  percent: {") : ", ",
					kFieldKeys[field], percent[field]);
			first = false;
		}
		if (!first)
			fputs("}\n", f);
	}
}

void XwauMissionStats_Free(XwauMissionStats* stats) {
	free(stats->models);
	memset(stats, 0, sizeof *stats);
}
