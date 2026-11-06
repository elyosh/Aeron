#include "xwau_weapon_stats.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aeron/asset/flight_names.h"
#include "util.h"
#include "xwau_config.h"

enum { FIRST_WEAPON = 280, WEAPONS = 28, WARHEAD_TYPES = 12, FORMS = 8, STATS = 15, LINKING_KEYS = 4 };

static const char* const kSections[XWAU_WEAPON_STATS_GROUPS] = { "WeaponStats", "WarheadStats", "EnergyStats",
																 "LinkingStats" };
static const char* const kGroupNames[XWAU_WEAPON_STATS_GROUPS] = { "stats", "warhead_capacity_percent",
																   "energy_speed_percent", "linking" };

/* The hook's stats, and the names that the game's files use for them. */
static const char* const kStats[STATS] = {
	"DurationIntegerPart",
	"DurationDecimalPart",
	"Score",
	"Speed",
	"Power",
	"PowerSpeedPercent",
	"FireRate",
	"DurationOffset",
	"FireRatio",
	"Range",
	"IsPrecise",
	"DegreeOfSpreadMask",
	"DechargeRate",
	"RechargeRate",
	"EnergyLowHighSeparation",
};
/* XWAU's DurationIntegerPart and DurationDecimalPart are written together
   as `duration_ms`, so the name of the second one is empty. */
static const char* const kStatNames[STATS] = {
	"duration_ms",
	"",
	"score",
	"speed_mglt",
	"power",
	"power_speed_percent",
	"fire_rate_ms",
	"duration_offset_ms",
	"fire_ratio_ms",
	"range_m",
	"precise",
	"spread_mask",
	"decharge_rate",
	"recharge_rate",
	"high_power_charge",
};

enum { STAT_DURATION_SECONDS = 0, STAT_DURATION_FRACTION = 1, STAT_PRECISE = 10 };

/* The unit of each stat in the model file. */
static const XwauUnit kStatUnits[STATS] = {
	XWAU_UNIT_NONE, XWAU_UNIT_NONE,         XWAU_UNIT_NONE,         XWAU_UNIT_MGLT,         XWAU_UNIT_NONE,
	XWAU_UNIT_NONE, XWAU_UNIT_MILLISECONDS, XWAU_UNIT_MILLISECONDS, XWAU_UNIT_MILLISECONDS, XWAU_UNIT_METRES,
	XWAU_UNIT_NONE, XWAU_UNIT_NONE,         XWAU_UNIT_NONE,         XWAU_UNIT_NONE,         XWAU_UNIT_NONE,
};

/* The forms of a stat, in the order that the game numbers them. */
static const char* const kForms[FORMS]     = { "",      "_Player",      "_Easy",          "_Medium",
											   "_Hard", "_Player_Easy", "_Player_Medium", "_Player_Hard" };
static const char* const kFormNames[FORMS] = { "",     "player",      "easy",          "medium",
											   "hard", "player_easy", "player_medium", "player_hard" };

static const char* const kEnergyNames[2] = { "all", "player" };

static const char* const kLinkingKeys[LINKING_KEYS]  = { "LaserLink", "PreventAILasersIonWhenDisabling",
														 "PreventAIIonWhenNotDisabling",
														 "PreventPlayerLinkedLasersIon" };
static const char* const kLinkingNames[LINKING_KEYS] = { "laser_link", "prevent_ai_lasers_ion_when_disabling",
														 "prevent_ai_ion_when_not_disabling",
														 "prevent_player_linked_lasers_ion" };

static const char* const kChoiceSections[XWAU_WEAPON_STATS_GROUPS] = { "WeaponProfiles", "WarheadProfiles",
																	   "EnergyProfiles", "LinkingProfiles" };
static const char* const kChoiceKeys[XWAU_WEAPON_STATS_GROUPS]  = { "WeaponProfile_fg_", "WarheadProfile_fg_",
																	"EnergyProfile_fg_",
																	"LinkingProfile_fg_" };
static const char* const kChoiceNames[XWAU_WEAPON_STATS_GROUPS] = { "weapon_profile", "warhead_profile",
																	"energy_profile", "linking_profile" };

/* ---- Lines
   ---------------------------------------------------------------- */

/* Reads the lines of `txt`, or the lines of section `name` in `ini` when
 * `txt` has none. Returns false when out of memory. */
static bool section_lines(const char* game, const char* txt, const char* ini, const char* name,
						  XwauLines* out, char* source, size_t source_cap) {
	char path[1024];
	if (!XwauLines_Read(Xwau_FindFile(game, txt, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, out))
		return false;
	snprintf(source, source_cap, "%s", txt);
	if (out->count)
		return true;
	snprintf(source, source_cap, "%s [%s]", ini, name);
	return XwauLines_Read(Xwau_FindFile(game, ini, path, sizeof path) ? path : NULL, name, XWAU_RULES_CPP,
						  out);
}

/* Reads hook_weapon_rate's settings: hook_weapon_rate.cfg, or else the
 * [hook_weapon_rate] section of Hooks.ini. */
static bool hook_lines(const char* game, XwauLines* out) {
	char path[1024];
	if (!XwauLines_Read(Xwau_FindFile(game, "hook_weapon_rate.cfg", path, sizeof path) ? path : NULL, "",
						XWAU_RULES_CPP, out))
		return false;
	if (out->count)
		return true;
	return XwauLines_Read(Xwau_FindFile(game, "Hooks.ini", path, sizeof path) ? path : NULL,
						  "hook_weapon_rate", XWAU_RULES_CPP, out);
}

/* Splits `line` into a trimmed key and value, like the hook's
 * GetFileKeyValue. Returns false when the line has no '=' or no key. */
static bool split_line(const char* line, char* key, size_t key_cap, char* value, size_t value_cap) {
	const char* equals = strchr(line, '=');
	if (!equals)
		return false;
	snprintf(key, key_cap, "%.*s", (int)(equals - line), line);
	snprintf(value, value_cap, "%s", equals + 1);
	Xwau_Trim(key, XWAU_RULES_CPP);
	Xwau_Trim(value, XWAU_RULES_CPP);
	return key[0] != '\0';
}

/* Writes the profile of a key in lower case. The profile is the text before
 * `at`, without the '_' at its end. Returns false when that text isn't a
 * profile. A profile that doesn't fit is cut, and sets `*too_long`. */
static bool key_profile(const char* key, size_t at, char* out, bool* too_long) {
	if (at == 0) {
		out[0] = '\0';
		return true;
	}
	if (key[at - 1] != '_')
		return false;
	size_t length = at - 1;
	if (length >= XWAU_WEAPON_STATS_NAME_MAX) {
		*too_long = true;
		length    = XWAU_WEAPON_STATS_NAME_MAX - 1;
	}
	for (size_t i = 0; i < length; ++i)
		out[i] = Xwau_Lower(key[i]);
	out[length] = '\0';
	return true;
}

bool XwauWeaponStats_Stof(const char* text, float* out) {
	char* end = NULL;
	*out      = strtof(text, &end);
	return end != text;
}

/* ---- Keys
   ----------------------------------------------------------------- */

/* Parses a [WeaponStats] key. Returns 1 for a stat, -1 for Side and
 * SideModel, and 0 for other keys. */
static int parse_weapon_key(const char* key, XwauWeaponStatsValue* out, bool* too_long) {
	const size_t length = strlen(key);
	for (size_t at = 0; at + 9 <= length; ++at) {
		if (strncasecmp(key + at, "Weapon", 6) != 0 || (at && key[at - 1] != '_'))
			continue;
		const char* digits = key + at + 6;
		if (digits[0] < '0' || digits[0] > '9' || digits[1] < '0' || digits[1] > '9' || digits[2] < '0' ||
			digits[2] > '9')
			continue;
		const int weapon = (digits[0] - '0') * 100 + (digits[1] - '0') * 10 + (digits[2] - '0');
		if (weapon < FIRST_WEAPON || weapon >= FIRST_WEAPON + WEAPONS ||
			!key_profile(key, at, out->profile, too_long))
			continue;
		const char* rest = digits + 3;
		for (int form = 0; form < FORMS; ++form) {
			const size_t form_length = strlen(kForms[form]);
			if (strncasecmp(rest, kForms[form], form_length) != 0 || rest[form_length] != '_')
				continue;
			const char* stat_name = rest + form_length + 1;
			if (!strcasecmp(stat_name, "Side") || !strcasecmp(stat_name, "SideModel"))
				return -1;
			for (int stat = 0; stat < STATS; ++stat) {
				if (strcasecmp(stat_name, kStats[stat]) != 0)
					continue;
				out->group = XWAU_WEAPON_STATS_STATS;
				out->index = weapon;
				out->form  = form;
				out->stat  = stat;
				return 1;
			}
		}
	}
	return 0;
}

/* Parses a [WarheadStats] key. */
static bool parse_warhead_key(const char* key, XwauWeaponStatsValue* out, bool* too_long) {
	const size_t length = strlen(key);
	for (size_t at = 0; at + 7 < length; ++at) {
		if (strncasecmp(key + at, "Warhead", 7) != 0 || !key_profile(key, at, out->profile, too_long))
			continue;
		char*      end  = NULL;
		const long type = strtol(key + at + 7, &end, 10);
		if (end == key + at + 7 || key[at + 7] == '-' || key[at + 7] == '+' ||
			strcasecmp(end, "_CapacityPercent") != 0)
			continue;
		if (type < 0 || type >= WARHEAD_TYPES)
			return false;
		out->group = XWAU_WEAPON_STATS_WARHEADS;
		out->index = (int)type;
		return true;
	}
	return false;
}

/* Parses an [EnergyStats] or [LinkingStats] key, which ends with one of
 * `names`. */
static bool parse_suffix_key(const char* key, int group, const char* const* names, int count,
							 XwauWeaponStatsValue* out, bool* too_long) {
	const size_t length = strlen(key);
	/* Try the last names first. Otherwise SpeedFactorForPlayer would be read
	   as the SpeedFactor of a profile whose name ends with "ForPlayer". */
	for (int i = count - 1; i >= 0; --i) {
		const size_t name_length = strlen(names[i]);
		if (name_length > length || strcasecmp(key + length - name_length, names[i]) != 0 ||
			!key_profile(key, length - name_length, out->profile, too_long))
			continue;
		out->group = group;
		out->index = i;
		return true;
	}
	return false;
}

/* ---- Reading
   -------------------------------------------------------------- */

static XwauWeaponStatsValue* find_value(const XwauWeaponStats* stats, const XwauWeaponStatsValue* value) {
	for (size_t i = 0; i < stats->count; ++i) {
		XwauWeaponStatsValue* item = &stats->items[i];
		if (item->group == value->group && item->index == value->index && item->form == value->form &&
			item->stat == value->stat && strcmp(item->profile, value->profile) == 0)
			return item;
	}
	return NULL;
}

static const char* const kEnergyKeys[2] = { "SpeedFactor", "SpeedFactorForPlayer" };

/* Reads the lines of section `group` into `out`, and adds the keys set to
 * -1, which means "not set", to `unset`. Reports what the import can't keep
 * when `report` is true. The hook doesn't read lines without a key, keys
 * that it doesn't know, keys of a profile named Default, or values that
 * aren't numbers, and the first line of a key wins. The importer leaves the
 * other lines out. */
static bool read_section(const XwauLines* lines, const char* source, int group, XwauWeaponStats* out,
						 XwauWeaponStats* unset, bool report) {
	for (size_t i = 0; i < lines->count; ++i) {
		char                 key[256], text[256];
		XwauWeaponStatsValue value;
		bool                 too_long = false;
		memset(&value, 0, sizeof value);
		if (!split_line(lines->items[i], key, sizeof key, text, sizeof text))
			continue;
		bool known = false;
		if (group == XWAU_WEAPON_STATS_STATS) {
			const int parsed = parse_weapon_key(key, &value, &too_long);
			if (parsed < 0) {
				long long side = 0;
				if (report && !(Xwau_StoiAutoInt(text, &side) && side == -1))
					ReportUnsupported("the [WeaponStats] keys Side and SideModel");
				continue;
			}
			known = parsed > 0;
		} else if (group == XWAU_WEAPON_STATS_WARHEADS) {
			known = parse_warhead_key(key, &value, &too_long);
		} else if (group == XWAU_WEAPON_STATS_ENERGY) {
			known = parse_suffix_key(key, group, kEnergyKeys, 2, &value, &too_long);
		} else {
			known = parse_suffix_key(key, group, kLinkingKeys, LINKING_KEYS, &value, &too_long);
		}
		if (!known)
			continue;
		if (too_long) {
			if (report)
				ReportFatal(source,
							"%s has a profile name longer than %d characters, which OpenXWA can't hold", key,
							XWAU_WEAPON_STATS_NAME_MAX - 1);
			continue;
		}
		/* Default in a mission means "no profile". */
		if (strcmp(value.profile, "default") == 0)
			continue;
		long long number = 0;
		if (!Xwau_StoiAutoInt(text, &number))
			continue;
		if (number == -1) {
			/* The hook takes the first line with the key, so a -1 still hides a
			   later line with the same key. */
			if (find_value(out, &value) || find_value(unset, &value))
				continue;
			XwauWeaponStatsValue* added = ARRAY_PUSH(unset->items, unset->count);
			if (!added)
				return false;
			*added = value;
			continue;
		}
		if (group == XWAU_WEAPON_STATS_ENERGY) {
			float percent = 0.0f;
			if (!XwauWeaponStats_Stof(text, &percent))
				continue;
			/* As in the hook: a fraction of the top speed, from 0 to 1, in
			   Q16. */
			float factor = percent * 0.01f;
			if (factor < 0.0f)
				factor = 0.0f;
			else if (factor > 1.0f)
				factor = 1.0f;
			factor = factor * 65536.0f;
			number = (long long)factor;
		} else if (group == XWAU_WEAPON_STATS_WARHEADS && number < -1) {
			number = 0;
		}
		if (find_value(out, &value) || find_value(unset, &value))
			continue;
		value.value                 = number;
		XwauWeaponStatsValue* added = ARRAY_PUSH(out->items, out->count);
		if (!added)
			return false;
		*added = value;
	}
	return true;
}

/* Adds the hardpoint kinds of `lines` to `out`. Each line is read the way
   std::stoi reads it. A line that isn't a number would crash XWAU, and the
   hook only reads the first lines, so both are left out. */
static void read_kinds(const XwauLines* lines, XwauWeaponStats* out) {
	for (size_t i = 0; i < lines->count; ++i) {
		long long kind = 0;
		if (!Xwau_Stoi(lines->items[i], &kind))
			continue;
		out->has_hardpoint_kinds = true;
		if (out->hardpoint_kind_count < XWAU_WEAPON_STATS_HARDPOINT_TYPES)
			out->hardpoint_kinds[out->hardpoint_kind_count++] = (int)kind;
	}
}

/* Returns whether the hook reads [WeaponStats], which it does when its
 * EnableWeaponStatsProfiles isn't 0. XWAU fails on a value that isn't a
 * number, and the importer then reads [WeaponStats] as if it were 1. */
static bool stats_enabled(const char* game) {
	XwauLines lines;
	char      text[256];
	long long value = 1;
	if (!hook_lines(game, &lines))
		return true;
	XwauLines_Value(&lines, "EnableWeaponStatsProfiles", XWAU_RULES_CPP, text, sizeof text);
	XwauLines_Free(&lines);
	if (text[0] && !Xwau_StoiAutoInt(text, &value))
		return true;
	return value != 0;
}

/* Writes the hook's key of `value`, such as "Weapon280_Player_Power". */
static void value_key(const XwauWeaponStatsValue* value, char* out, size_t cap) {
	char base[160];
	if (value->group == XWAU_WEAPON_STATS_STATS)
		snprintf(base, sizeof base, "Weapon%03d%s_%s", value->index, kForms[value->form],
				 kStats[value->stat]);
	else if (value->group == XWAU_WEAPON_STATS_WARHEADS)
		snprintf(base, sizeof base, "Warhead%d_CapacityPercent", value->index);
	else if (value->group == XWAU_WEAPON_STATS_ENERGY)
		snprintf(base, sizeof base, "%s", kEnergyKeys[value->index]);
	else
		snprintf(base, sizeof base, "%s", kLinkingKeys[value->index]);
	snprintf(out, cap, "%s%s%s", value->profile, value->profile[0] ? "_" : "", base);
}

/* Reads the global sections of `group` without reports, into `out`. */
static bool read_global_section(const char* game, int group, XwauWeaponStats* out) {
	char            txt[512], source[600];
	XwauLines       lines;
	XwauWeaponStats unset = { 0 };
	snprintf(txt, sizeof txt, "FlightModels/%s.txt", kSections[group]);
	if (!section_lines(game, txt, "FlightModels/default.ini", kSections[group], &lines, source,
					   sizeof source))
		return false;
	const bool ok = read_section(&lines, source, group, out, &unset, false);
	XwauLines_Free(&lines);
	XwauWeaponStats_Free(&unset);
	return ok;
}

bool XwauWeaponStats_Read(const char* game, const char* model, XwauWeaponStats* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path))
		return true;
	const bool      enabled = stats_enabled(game);
	bool            ok      = true;
	XwauWeaponStats unset   = { 0 };
	for (int group = 0; ok && group < XWAU_WEAPON_STATS_GROUPS + 1; ++group) {
		const char* section = group < XWAU_WEAPON_STATS_GROUPS ? kSections[group] : "WeaponHardpointTypes";
		char        txt[512], ini[512], source[600];
		XwauLines   lines;
		if (model) {
			snprintf(txt, sizeof txt, "FlightModels/%s%s.txt", model, section);
			snprintf(ini, sizeof ini, "FlightModels/%s.ini", model);
		} else {
			snprintf(txt, sizeof txt, "FlightModels/%s.txt", section);
			snprintf(ini, sizeof ini, "FlightModels/default.ini");
		}
		if (!(ok = section_lines(game, txt, ini, section, &lines, source, sizeof source)))
			break;
		/* With EnableWeaponStatsProfiles = 0, the hook doesn't read
		   [WeaponStats]. */
		if (group == XWAU_WEAPON_STATS_STATS && !enabled) {
			XwauLines_Free(&lines);
			continue;
		}
		if (group < XWAU_WEAPON_STATS_GROUPS) {
			const size_t before = unset.count;
			ok                  = read_section(&lines, source, group, out, &unset, true);
			/* A model's -1 hides the global line with the same key. The game
			   has no way to do that, so it would use the global value. */
			XwauWeaponStats global = { 0 };
			if (ok && model && unset.count > before)
				ok = read_global_section(game, group, &global);
			for (size_t i = before; ok && model && i < unset.count; ++i) {
				char key[256];
				if (!find_value(&global, &unset.items[i]))
					continue;
				value_key(&unset.items[i], key, sizeof key);
				ReportFatal(source, "%s = -1 hides the global value of the same key, which OpenXWA can't do",
							key);
			}
			XwauWeaponStats_Free(&global);
		} else {
			read_kinds(&lines, out);
		}
		XwauLines_Free(&lines);
	}
	XwauWeaponStats_Free(&unset);
	/* The game looks for each part of a duration in the model first, then in
	   the global section. Keep the global parts, so that a model's duration
	   can use them. */
	if (ok && model && enabled) {
		XwauWeaponStats global = { 0 };
		ok                     = read_global_section(game, XWAU_WEAPON_STATS_STATS, &global);
		for (size_t i = 0; ok && i < global.count; ++i) {
			const XwauWeaponStatsValue* v = &global.items[i];
			if (v->group != XWAU_WEAPON_STATS_STATS ||
				(v->stat != STAT_DURATION_SECONDS && v->stat != STAT_DURATION_FRACTION))
				continue;
			XwauWeaponStatsValue* added = ARRAY_PUSH(out->global_durations, out->global_duration_count);
			if (!added)
				ok = false;
			else
				*added = *v;
		}
		XwauWeaponStats_Free(&global);
	}
	/* The hook reads the model's hardpoint kinds, then the global ones. */
	if (ok && model && out->has_hardpoint_kinds) {
		XwauLines lines;
		char      source[600];
		if (!(ok = section_lines(game, "FlightModels/WeaponHardpointTypes.txt", "FlightModels/default.ini",
								 "WeaponHardpointTypes", &lines, source, sizeof source)))
			return false;
		XwauWeaponStats global = { 0 };
		read_kinds(&lines, &global);
		XwauLines_Free(&lines);
		out->has_global_hardpoint_kinds  = global.has_hardpoint_kinds;
		out->global_hardpoint_kind_count = global.hardpoint_kind_count;
		memcpy(out->global_hardpoint_kinds, global.hardpoint_kinds, sizeof global.hardpoint_kinds);
		for (int i = 0;
			 i < global.hardpoint_kind_count && out->hardpoint_kind_count < XWAU_WEAPON_STATS_HARDPOINT_TYPES;
			 ++i)
			out->hardpoint_kinds[out->hardpoint_kind_count++] = global.hardpoint_kinds[i];
	}
	if (!ok)
		XwauWeaponStats_Free(out);
	return ok;
}

void XwauWeaponStats_Free(XwauWeaponStats* stats) {
	free(stats->items);
	free(stats->global_durations);
	memset(stats, 0, sizeof *stats);
}

bool XwauWeaponStats_Any(const XwauWeaponStats* stats) { return stats->count || stats->has_hardpoint_kinds; }

/* ---- Writing
   -------------------------------------------------------------- */

/* Returns whether `stats` has a value of `profile` in `group` before item
   `before`. */
static bool profile_seen(const XwauWeaponStats* stats, int group, const char* profile, size_t before) {
	for (size_t i = 0; i < before; ++i)
		if (stats->items[i].group == group && strcmp(stats->items[i].profile, profile) == 0)
			return true;
	return false;
}

static void write_number(FILE* f, int stat, long long value) {
	if (stat == STAT_PRECISE)
		fputs(value ? "true" : "false", f);
	else if (kStatUnits[stat] != XWAU_UNIT_NONE)
		Xwau_WriteUnit(f, kStatUnits[stat], value);
	else
		fprintf(f, "%lld", value);
}

/* The order in which the game looks at the forms. Each row starts with a
   form and lists the forms that the game tries after it. -1 ends the row.
   For example, for a player on easy difficulty, the game tries
   player_easy, then player, then easy, then the plain value. */
static const int kFormChains[FORMS][5] = {
	{ 0, -1 },    { 1, 0, -1 },       { 2, 0, -1 },       { 3, 0, -1 },
	{ 4, 0, -1 }, { 5, 1, 2, 0, -1 }, { 6, 1, 3, 0, -1 }, { 7, 1, 4, 0, -1 },
};

/* Finds the value of `stat` for `weapon` in `profile` at `form`. Returns
   NULL if there isn't one. */
static const XwauWeaponStatsValue* find_stat(const XwauWeaponStatsValue* items, size_t count,
											 const char* profile, int weapon, int form, int stat) {
	for (size_t i = 0; i < count; ++i)
		if (items[i].group == XWAU_WEAPON_STATS_STATS && items[i].index == weapon && items[i].form == form &&
			items[i].stat == stat && strcmp(items[i].profile, profile) == 0)
			return &items[i];
	return NULL;
}

/* Returns one part of a duration at `form`. It looks in the same order as
   the game: for each form in the row of `form`, first the model's values,
   then the global values. If no value is set, it returns the stock
   value. */
static long long duration_part(const XwauWeaponStats* stats, const XwauStockValues* stock,
							   const char* profile, int weapon, int form, int stat) {
	for (const int* f = kFormChains[form]; *f >= 0; ++f) {
		const XwauWeaponStatsValue* v = find_stat(stats->items, stats->count, profile, weapon, *f, stat);
		if (!v)
			v = find_stat(stats->global_durations, stats->global_duration_count, profile, weapon, *f, stat);
		if (v)
			return v->value;
	}
	const size_t at = (size_t)(weapon - FIRST_WEAPON);
	if (at >= stock->lifetime_count)
		return 0;
	return stat == STAT_DURATION_SECONDS ? stock->lifetime_seconds[at] : stock->lifetime_fractions[at];
}

/* Writes the duration of `weapon` at `form`, in milliseconds. The game
   counts 236 ticks for each second, adds the fraction rounded to a whole
   tick, and keeps the result in 16 bits. A tick is 4 ms. */
static void write_duration(FILE* f, const XwauWeaponStats* stats, const XwauStockValues* stock,
						   const char* profile, int weapon, int form) {
	const bool has_seconds =
		find_stat(stats->items, stats->count, profile, weapon, form, STAT_DURATION_SECONDS);
	const bool has_fraction =
		find_stat(stats->items, stats->count, profile, weapon, form, STAT_DURATION_FRACTION);
	/* For a player, the game tries the player form before the difficulty
	   forms. When the player form has one part and a difficulty form has
	   the other, the game mixes them. One merged value can't do that. */
	if (form == 1 && has_seconds != has_fraction) {
		const int missing = has_seconds ? STAT_DURATION_FRACTION : STAT_DURATION_SECONDS;
		for (int difficulty = 2; difficulty <= 4; ++difficulty)
			if (find_stat(stats->items, stats->count, profile, weapon, difficulty, missing) ||
				find_stat(stats->global_durations, stats->global_duration_count, profile, weapon, difficulty,
						  missing)) {
				ReportUnsupported("shot durations split between the player form and a difficulty form");
				break;
			}
	}
	const uint16_t seconds =
		(uint16_t)duration_part(stats, stock, profile, weapon, form, STAT_DURATION_SECONDS);
	const uint16_t fraction =
		(uint16_t)duration_part(stats, stock, profile, weapon, form, STAT_DURATION_FRACTION);
	const uint16_t ticks = (uint16_t)((uint16_t)(236u * seconds) + ((236u * fraction + 0x8000u) >> 16));
	Xwau_WriteUnit(f, XWAU_UNIT_MILLISECONDS, ticks);
}

/* Writes the stats of one weapon type in a profile as a flow map. */
static void write_weapon(FILE* f, const XwauNames* names, const XwauStockValues* stock,
						 const XwauWeaponStats* stats, const char* profile, int weapon) {
	fprintf(f, "%s: {", XwauNames_WeaponType(names, weapon));
	bool first = true;
	for (int form = 0; form < FORMS; ++form) {
		bool form_open = false;
		for (int stat = 0; stat < STATS; ++stat) {
			const XwauWeaponStatsValue* v =
				find_stat(stats->items, stats->count, profile, weapon, form, stat);
			/* The fraction is written with the seconds, as one key. */
			if (stat == STAT_DURATION_FRACTION)
				continue;
			if (stat == STAT_DURATION_SECONDS && !v)
				v = find_stat(stats->items, stats->count, profile, weapon, form, STAT_DURATION_FRACTION);
			if (!v)
				continue;
			if (form && !form_open) {
				fprintf(f, "%s%s: {", first ? "" : ", ", kFormNames[form]);
				form_open = true;
				first     = true;
			}
			fprintf(f, "%s%s: ", first ? "" : ", ", kStatNames[stat]);
			if (stat == STAT_DURATION_SECONDS)
				write_duration(f, stats, stock, profile, weapon, form);
			else
				write_number(f, stat, v->value);
			first = false;
		}
		if (form_open) {
			fputc('}', f);
			first = false;
		}
	}
	fputs("}\n", f);
}

static void write_profile_key(FILE* f, const char* indent, const char* profile) {
	fprintf(f, "%s  ", indent);
	if (profile[0])
		Xwau_WriteString(f, profile);
	else
		fputs("default", f);
	fputs(":", f);
}

/* Returns the kind that the game gives hardpoint type `type` when the map of
   `stats` leaves it out: the global kind for a model when the mod has
   global kinds, else the stock kind. */
static int fallback_kind(const XwauWeaponStats* stats, const XwauStockValues* stock, int type) {
	if (stats->has_global_hardpoint_kinds)
		return type < stats->global_hardpoint_kind_count ? stats->global_hardpoint_kinds[type] & 0xFF : 0;
	return (size_t)type < stock->hardpoint_kind_count ? stock->hardpoint_kinds[type] : 0;
}

void XwauWeaponStats_Write(FILE* f, const XwauNames* names, const XwauStockValues* stock, const char* indent,
						   const XwauWeaponStats* stats) {
	for (int group = 0; group < XWAU_WEAPON_STATS_GROUPS; ++group) {
		bool any = false;
		for (size_t p = 0; p < stats->count; ++p) {
			const XwauWeaponStatsValue* head = &stats->items[p];
			if (head->group != group || profile_seen(stats, group, head->profile, p))
				continue;
			if (!any)
				fprintf(f, "%s%s:\n", indent, kGroupNames[group]);
			any = true;
			write_profile_key(f, indent, head->profile);
			if (group == XWAU_WEAPON_STATS_STATS) {
				fputc('\n', f);
				for (int weapon = FIRST_WEAPON; weapon < FIRST_WEAPON + WEAPONS; ++weapon) {
					bool has = false;
					for (size_t i = 0; !has && i < stats->count; ++i)
						has = stats->items[i].group == group && stats->items[i].index == weapon &&
							  strcmp(stats->items[i].profile, head->profile) == 0;
					if (!has)
						continue;
					fprintf(f, "%s    ", indent);
					write_weapon(f, names, stock, stats, head->profile, weapon);
				}
				continue;
			}
			fputs(" {", f);
			bool      first = true;
			const int count = group == XWAU_WEAPON_STATS_WARHEADS ? WARHEAD_TYPES
							  : group == XWAU_WEAPON_STATS_ENERGY ? 2
																  : LINKING_KEYS;
			for (int index = 0; index < count; ++index) {
				for (size_t i = 0; i < stats->count; ++i) {
					const XwauWeaponStatsValue* v = &stats->items[i];
					if (v->group != group || v->index != index || strcmp(v->profile, head->profile) != 0)
						continue;
					/* A warhead type that the game has no name for doesn't
					   exist. */
					if (group == XWAU_WEAPON_STATS_WARHEADS && !XwauNames_Name(&names->warheads, index))
						continue;
					fputs(first ? "" : ", ", f);
					first = false;
					if (group == XWAU_WEAPON_STATS_WARHEADS)
						fprintf(f, "%s: %lld", XwauNames_Name(&names->warheads, index), v->value);
					else if (group == XWAU_WEAPON_STATS_ENERGY) {
						fprintf(f, "%s: ", kEnergyNames[index]);
						Xwau_WriteUnit(f, XWAU_UNIT_PERCENT_Q16, v->value);
					} else if (index == 0)
						fprintf(f, "%s: %lld", kLinkingNames[index], v->value);
					else
						fprintf(f, "%s: %s", kLinkingNames[index], v->value ? "true" : "false");
				}
			}
			fputs("}\n", f);
		}
	}
	/* XWAU gives kind 0 to the hardpoint types past the end of its list. The
	   map only has the hardpoint types whose kind differs from the kind that
	   the game would use without the entry. */
	if (stats->has_hardpoint_kinds) {
		bool first = true;
		for (int type = 0; type < XWAU_WEAPON_STATS_HARDPOINT_TYPES; ++type) {
			const int   kind = type < stats->hardpoint_kind_count ? stats->hardpoint_kinds[type] & 0xFF : 0;
			const char* kind_name = XwauNames_Name(&names->hardpoint_kinds, kind);
			if (kind == fallback_kind(stats, stock, type))
				continue;
			if (!kind_name) {
				ReportFatal(NULL, "hardpoint type %d has kind %d, which the game has no name for", type,
							kind);
				continue;
			}
			if (first)
				fprintf(f, "%shardpoint_kinds: {", indent);
			else
				fputs(", ", f);
			fprintf(f, "%s: %s", Aeron_FlightName(AERON_FLIGHT_HARDPOINT_TYPES, (uint32_t)type), kind_name);
			first = false;
		}
		if (!first)
			fputs("}\n", f);
	}
}

bool XwauWeaponStats_HasProfile(const char* game, const char* model, int group, const char* profile) {
	bool found = false;
	for (int pass = 0; !found && pass < 2; ++pass) {
		XwauWeaponStats stats;
		if (!XwauWeaponStats_Read(game, pass ? NULL : model, &stats))
			return true;
		for (size_t i = 0; !found && i < stats.count; ++i)
			found = stats.items[i].group == group && strcmp(stats.items[i].profile, profile) == 0;
		XwauWeaponStats_Free(&stats);
	}
	return found;
}

/* ---- Missions
   ------------------------------------------------------------- */

static int compare_choices(const void* a, const void* b) {
	const int x = ((const XwauWeaponProfileChoice*)a)->fg, y = ((const XwauWeaponProfileChoice*)b)->fg;
	return (x > y) - (x < y);
}

/* Reads the lines of a mission's section `name`, from the files that the
 * hook's GetMissionLines reads. */
static bool choice_lines(const char* game, const char* base, const char* name, XwauLines* out, char* source,
						 size_t cap) {
	char txt[600], ini[600];
	snprintf(txt, sizeof txt, "%s_%s.txt", base, name);
	snprintf(ini, sizeof ini, "%s.ini", base);
	if (!section_lines(game, txt, ini, name, out, source, cap))
		return false;
	if (out->count)
		return true;
	XwauLines_Free(out);
	snprintf(txt, sizeof txt, "FlightModels/%s.txt", name);
	return section_lines(game, txt, "FlightModels/default.ini", name, out, source, cap);
}

bool XwauWeaponStats_ReadChoices(const char* game, const char* base, XwauWeaponProfileChoices* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path))
		return true;
	for (int group = 0; group < XWAU_WEAPON_STATS_GROUPS; ++group) {
		XwauLines lines;
		char      source[700];
		if (!choice_lines(game, base, kChoiceSections[group], &lines, source, sizeof source)) {
			XwauWeaponStats_FreeChoices(out);
			return false;
		}
		const size_t prefix = strlen(kChoiceKeys[group]);
		bool         seen[192];
		memset(seen, 0, sizeof seen);
		/* The hook doesn't read lines without a key or with other keys, and
		   the first line of a key wins. */
		for (size_t i = 0; i < lines.count; ++i) {
			char key[256], value[256];
			if (!split_line(lines.items[i], key, sizeof key, value, sizeof value))
				continue;
			const char* digits = key + prefix;
			char*       end    = NULL;
			const long  fg     = strncasecmp(key, kChoiceKeys[group], prefix) == 0 && digits[0] >= '0' &&
										 digits[0] <= '9' && (digits[0] != '0' || digits[1] == '\0')
									 ? strtol(digits, &end, 10)
									 : -1;
			if (fg < 0 || *end || fg >= 192)
				continue;
			if (seen[fg])
				continue;
			seen[fg] = true;
			/* Default in a mission means "no profile". */
			if (Xwau_SameName(value, "Default") || !value[0])
				continue;
			if (strlen(value) >= XWAU_WEAPON_STATS_NAME_MAX) {
				ReportFatal(source,
							"%s = %s has a profile name longer than %d characters, which OpenXWA can't hold",
							key, value, XWAU_WEAPON_STATS_NAME_MAX - 1);
				continue;
			}
			XwauWeaponProfileChoice* choice =
				(XwauWeaponProfileChoice*)XwauWeaponStats_FindChoice(out, (int)fg);
			if (!choice) {
				if (!(choice = ARRAY_PUSH(out->items, out->count))) {
					XwauLines_Free(&lines);
					XwauWeaponStats_FreeChoices(out);
					return false;
				}
				memset(choice, 0, sizeof *choice);
				choice->fg = (int)fg;
				qsort(out->items, out->count, sizeof *out->items, compare_choices);
				choice = (XwauWeaponProfileChoice*)XwauWeaponStats_FindChoice(out, (int)fg);
			}
			for (size_t c = 0; value[c]; ++c)
				choice->name[group][c] = Xwau_Lower(value[c]);
		}
		XwauLines_Free(&lines);
	}
	return true;
}

void XwauWeaponStats_FreeChoices(XwauWeaponProfileChoices* choices) {
	free(choices->items);
	memset(choices, 0, sizeof *choices);
}

const XwauWeaponProfileChoice* XwauWeaponStats_FindChoice(const XwauWeaponProfileChoices* choices, int fg) {
	for (size_t i = 0; i < choices->count; ++i)
		if (choices->items[i].fg == fg)
			return &choices->items[i];
	return NULL;
}

bool XwauWeaponStats_AnyChoice(const XwauWeaponProfileChoice* choice) {
	for (int group = 0; choice && group < XWAU_WEAPON_STATS_GROUPS; ++group)
		if (choice->name[group][0])
			return true;
	return false;
}

void XwauWeaponStats_ReportChoice(const char* game, const char* base, const char* model,
								  const XwauWeaponProfileChoice* choice) {
	for (int group = 0; group < XWAU_WEAPON_STATS_GROUPS; ++group) {
		if (!choice->name[group][0] || XwauWeaponStats_HasProfile(game, model, group, choice->name[group]))
			continue;
		Report(base,
			   "flight group %d picks %s %s, which isn't a profile of %s, so it gets the original game's "
			   "values",
			   choice->fg, kChoiceNames[group], choice->name[group], model);
	}
}

void XwauWeaponStats_WriteChoice(FILE* f, const XwauWeaponProfileChoice* choice) {
	for (int group = 0; choice && group < XWAU_WEAPON_STATS_GROUPS; ++group) {
		if (!choice->name[group][0])
			continue;
		fprintf(f, ", %s: ", kChoiceNames[group]);
		Xwau_WriteString(f, choice->name[group]);
	}
}
