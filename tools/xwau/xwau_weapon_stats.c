#include "xwau_weapon_stats.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

enum { FIRST_WEAPON = 280, WEAPONS = 28, WARHEAD_TYPES = 12, FORMS = 8, STATS = 15, LINKING_KEYS = 4 };

static const char* const kSections[XWAU_WEAPON_STATS_GROUPS] = { "WeaponStats", "WarheadStats", "EnergyStats",
																 "LinkingStats" };
static const char* const kGroupNames[XWAU_WEAPON_STATS_GROUPS] = { "stats", "warhead_capacity_percent",
																   "energy_speed_q16", "linking" };

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
static const char* const kStatNames[STATS] = {
	"duration_seconds",
	"duration_fraction",
	"score",
	"speed",
	"power",
	"power_speed_percent",
	"fire_rate",
	"duration_offset",
	"fire_ratio",
	"range",
	"precise",
	"spread_mask",
	"decharge_rate",
	"recharge_rate",
	"high_power_charge",
};

enum { STAT_PRECISE = 10 };

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
 * profile. */
static bool key_profile(const char* key, size_t at, char* out) {
	if (at == 0) {
		out[0] = '\0';
		return true;
	}
	if (key[at - 1] != '_' || at - 1 >= XWAU_WEAPON_STATS_NAME_MAX)
		return false;
	for (size_t i = 0; i < at - 1; ++i)
		out[i] = Xwau_Lower(key[i]);
	out[at - 1] = '\0';
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
static int parse_weapon_key(const char* key, XwauWeaponStatsValue* out) {
	const size_t length = strlen(key);
	for (size_t at = 0; at + 9 <= length; ++at) {
		if (strncasecmp(key + at, "Weapon", 6) != 0 || (at && key[at - 1] != '_'))
			continue;
		const char* digits = key + at + 6;
		if (digits[0] < '0' || digits[0] > '9' || digits[1] < '0' || digits[1] > '9' || digits[2] < '0' ||
			digits[2] > '9')
			continue;
		const int weapon = (digits[0] - '0') * 100 + (digits[1] - '0') * 10 + (digits[2] - '0');
		if (weapon < FIRST_WEAPON || weapon >= FIRST_WEAPON + WEAPONS || !key_profile(key, at, out->profile))
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
static bool parse_warhead_key(const char* key, XwauWeaponStatsValue* out) {
	const size_t length = strlen(key);
	for (size_t at = 0; at + 7 < length; ++at) {
		if (strncasecmp(key + at, "Warhead", 7) != 0 || !key_profile(key, at, out->profile))
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
							 XwauWeaponStatsValue* out) {
	const size_t length = strlen(key);
	/* Try the last names first. Otherwise SpeedFactorForPlayer would be read
	   as the SpeedFactor of a profile whose name ends with "ForPlayer". */
	for (int i = count - 1; i >= 0; --i) {
		const size_t name_length = strlen(names[i]);
		if (name_length > length || strcasecmp(key + length - name_length, names[i]) != 0 ||
			!key_profile(key, length - name_length, out->profile))
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

/* Reads the lines of section `group` into `out`. */
static bool read_section(const XwauLines* lines, const char* source, int group, XwauWeaponStats* out) {
	static const char* const kEnergyKeys[2] = { "SpeedFactor", "SpeedFactorForPlayer" };
	for (size_t i = 0; i < lines->count; ++i) {
		char                 key[256], text[256];
		XwauWeaponStatsValue value;
		memset(&value, 0, sizeof value);
		if (!split_line(lines->items[i], key, sizeof key, text, sizeof text))
			continue;
		bool known = false;
		if (group == XWAU_WEAPON_STATS_STATS) {
			const int parsed = parse_weapon_key(key, &value);
			if (parsed < 0) {
				Report(source, "%s isn't imported: the IFF swap comes from the game's tables", key);
				continue;
			}
			known = parsed > 0;
		} else if (group == XWAU_WEAPON_STATS_WARHEADS) {
			known = parse_warhead_key(key, &value);
		} else if (group == XWAU_WEAPON_STATS_ENERGY) {
			known = parse_suffix_key(key, group, kEnergyKeys, 2, &value);
		} else {
			known = parse_suffix_key(key, group, kLinkingKeys, LINKING_KEYS, &value);
		}
		if (!known) {
			Report(source, "%s isn't a key that the hook reads; it is left out", key);
			continue;
		}
		if (strcmp(value.profile, "default") == 0) {
			Report(source,
				   "%s: the hook never reads a profile named Default, because Default in a mission means "
				   "no profile; it is left out",
				   key);
			continue;
		}
		long long number = 0;
		if (!Xwau_StoiAuto(text, &number)) {
			Report(source, "%s = %s isn't a number, which XWAU would fail on; it is left out", key, text);
			continue;
		}
		if (number == -1)
			continue;
		if (group == XWAU_WEAPON_STATS_ENERGY) {
			float percent = 0.0f;
			if (!XwauWeaponStats_Stof(text, &percent)) {
				Report(source, "%s = %s isn't a number, which XWAU would fail on; it is left out", key, text);
				continue;
			}
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
		if (find_value(out, &value)) {
			Report(source, "%s is set twice; the first line wins", key);
			continue;
		}
		value.value                 = number;
		XwauWeaponStatsValue* added = ARRAY_PUSH(out->items, out->count);
		if (!added)
			return false;
		*added = value;
	}
	return true;
}

/* Adds the hardpoint kinds of `lines` to `out`. Each line is read the way
   std::stoi reads it. */
static void read_kinds(const XwauLines* lines, const char* source, XwauWeaponStats* out) {
	for (size_t i = 0; i < lines->count; ++i) {
		long long kind = 0;
		if (!Xwau_Stoi(lines->items[i], &kind)) {
			Report(source, "\"%s\" isn't a number, which would crash XWAU; it is left out", lines->items[i]);
			continue;
		}
		out->has_hardpoint_kinds = true;
		if (out->hardpoint_kind_count < XWAU_WEAPON_STATS_HARDPOINT_TYPES)
			out->hardpoint_kinds[out->hardpoint_kind_count++] = (int)kind;
	}
}

/* Returns whether the hook reads [WeaponStats], which it does when its
 * EnableWeaponStatsProfiles isn't 0. */
static bool stats_enabled(const char* game) {
	XwauLines lines;
	char      text[256];
	long long value = 1;
	if (!hook_lines(game, &lines))
		return true;
	XwauLines_Value(&lines, "EnableWeaponStatsProfiles", XWAU_RULES_CPP, text, sizeof text);
	XwauLines_Free(&lines);
	return !text[0] || !Xwau_StoiAuto(text, &value) || value != 0;
}

bool XwauWeaponStats_Read(const char* game, const char* model, XwauWeaponStats* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path))
		return true;
	const bool enabled = stats_enabled(game);
	bool       ok      = true;
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
		if (group == XWAU_WEAPON_STATS_STATS && !enabled) {
			if (lines.count)
				Report(source, "is left out: the hook's EnableWeaponStatsProfiles is 0");
		} else if (group < XWAU_WEAPON_STATS_GROUPS) {
			ok = read_section(&lines, source, group, out);
		} else {
			read_kinds(&lines, source, out);
		}
		XwauLines_Free(&lines);
	}
	/* The hook reads the model's hardpoint kinds, then the global ones. */
	if (ok && model && out->has_hardpoint_kinds) {
		XwauLines lines;
		char      source[600];
		if (!(ok = section_lines(game, "FlightModels/WeaponHardpointTypes.txt", "FlightModels/default.ini",
								 "WeaponHardpointTypes", &lines, source, sizeof source)))
			return false;
		read_kinds(&lines, source, out);
		XwauLines_Free(&lines);
	}
	if (!ok)
		XwauWeaponStats_Free(out);
	return ok;
}

void XwauWeaponStats_Free(XwauWeaponStats* stats) {
	free(stats->items);
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
	else
		fprintf(f, "%lld", value);
}

/* Writes the stats of one weapon type in a profile as a flow map. */
static void write_weapon(FILE* f, const XwauWeaponStats* stats, const char* profile, int weapon) {
	fprintf(f, "%d: {", weapon);
	bool first = true;
	for (int form = 0; form < FORMS; ++form) {
		bool form_open = false;
		for (int stat = 0; stat < STATS; ++stat) {
			for (size_t i = 0; i < stats->count; ++i) {
				const XwauWeaponStatsValue* v = &stats->items[i];
				if (v->group != XWAU_WEAPON_STATS_STATS || v->index != weapon || v->form != form ||
					v->stat != stat || strcmp(v->profile, profile) != 0)
					continue;
				if (form && !form_open) {
					fprintf(f, "%s%s: {", first ? "" : ", ", kFormNames[form]);
					form_open = true;
					first     = true;
				}
				fprintf(f, "%s%s: ", first ? "" : ", ", kStatNames[stat]);
				write_number(f, stat, v->value);
				first = false;
			}
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

void XwauWeaponStats_Write(FILE* f, const char* indent, const XwauWeaponStats* stats) {
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
					write_weapon(f, stats, head->profile, weapon);
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
					fputs(first ? "" : ", ", f);
					first = false;
					if (group == XWAU_WEAPON_STATS_WARHEADS)
						fprintf(f, "%d: %lld", index, v->value);
					else if (group == XWAU_WEAPON_STATS_ENERGY)
						fprintf(f, "%s: %lld", kEnergyNames[index], v->value);
					else if (index == 0)
						fprintf(f, "%s: %lld", kLinkingNames[index], v->value);
					else
						fprintf(f, "%s: %s", kLinkingNames[index], v->value ? "true" : "false");
				}
			}
			fputs("}\n", f);
		}
	}
	if (stats->has_hardpoint_kinds) {
		fprintf(f, "%shardpoint_kinds: [", indent);
		for (int i = 0; i < stats->hardpoint_kind_count; ++i)
			fprintf(f, "%s%d", i ? ", " : "", stats->hardpoint_kinds[i] & 0xFF);
		fputs("]\n", f);
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
			if (fg < 0 || *end || fg >= 192) {
				Report(source, "%s isn't a key that the hook reads; it is left out", key);
				continue;
			}
			if (seen[fg]) {
				Report(source, "%s is set twice; the first line wins", key);
				continue;
			}
			seen[fg] = true;
			/* Default in a mission means "no profile". */
			if (Xwau_SameName(value, "Default") || !value[0])
				continue;
			if (strlen(value) >= XWAU_WEAPON_STATS_NAME_MAX) {
				Report(source, "%s: the name '%s' is too long; it is left out", key, value);
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
			   "flight group %d: %s %s isn't a profile of %s; XWAU gives the flight group the stock values, "
			   "and OpenXWA gives it the default values",
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
