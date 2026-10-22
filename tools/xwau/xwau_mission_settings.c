#include "xwau_mission_settings.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_craft.h"

static const char* const kListDirs[] = { "Missions", "Melee", "Combat" };

/* ---- Values ----------------------------------------------------------------- */

/* The value of `key`, read as std::stoi(value, 0, 0) reads it, or
 * `fallback` when the key is missing. A value that isn't a number stops
 * XWAU; it is reported when `base` is set. */
static long long int_value(const XwauLines* lines, const char* key, long long fallback, const char* base) {
	char      text[256];
	long long value = 0;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (!text[0])
		return fallback;
	if (Xwau_StoiAuto(text, &value))
		return value;
	if (base)
		Report(base, "%s: '%s' isn't a number, which XWAU would fail on; the default is used", key, text);
	return fallback;
}

/* Appends the numbers of the list `text` (split as XWAU's Tokennize splits
 * it) to `out`, read in base 0 when `auto_base` is set, else in base 10.
 * Words that aren't numbers are reported and left out. Returns false when
 * out of memory. */
static bool int_list(const char* text, bool auto_base, const char* key, const char* base, int** out,
					 size_t* count) {
	XwauLines words;
	if (!Xwau_Tokenize(text, XWAU_RULES_CPP, &words))
		return false;
	bool ok = true;
	for (size_t i = 0; ok && i < words.count; ++i) {
		long long value = 0;
		if (!(auto_base ? Xwau_StoiAuto(words.items[i], &value) : Xwau_Stoi(words.items[i], &value))) {
			Report(base, "%s: '%s' isn't a number, which XWAU would fail on; it is left out", key,
				   words.items[i]);
			continue;
		}
		int* added = ARRAY_PUSH(*out, *count);
		ok         = added != NULL;
		if (ok)
			*added = (int)value;
	}
	XwauLines_Free(&words);
	return ok;
}

/* The [Interdiction] lines of `prefix` ("Missions/1b4m4b" or "FlightModels/"):
 * <prefix>_Interdiction.txt, else the section of <prefix>.ini, or, for the
 * global ones, FlightModels/Interdiction.txt, else the section of
 * FlightModels/default.ini. */
static bool interdiction_lines(const char* game, const char* txt, const char* ini, XwauLines* out) {
	char path[1024];
	if (!XwauLines_Read(Xwau_FindFile(game, txt, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, out))
		return false;
	if (!out->count)
		return XwauLines_Read(Xwau_FindFile(game, ini, path, sizeof path) ? path : NULL, "Interdiction",
							  XWAU_RULES_CPP, out);
	return true;
}

/* ---- Global settings --------------------------------------------------------- */

bool XwauSettingsConfig_Read(const char* game, XwauSettingsConfig* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	out->mission_tie  = Xwau_FindFile(game, "hook_mission_tie.dll", path, sizeof path);
	out->engine_sound = Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path);
	out->opt_limit_engine_glows =
		Xwau_FindFile(game, "hook_opt_limit.dll", path, sizeof path) && Xwau_FileHasText(path, "EngineGlows");
	out->weapon_switch = XwauCraft_ReadGlobalSwitchByIff(game, &out->weapon_switch_by_iff);
	out->mission_tie_stats = Xwau_FindFile(game, "hook_mission_tie.dll", path, sizeof path) &&
							 Xwau_FileHasText(path, "StatsProfile");
	if (Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path)) {
		out->weapon_rate_fire_check = Xwau_FileHasText(path, "_EnergyLowHighSeparation");
		out->weapon_rate_rack_check = Xwau_FileHasText(path, "Craft_Offset_292");
		out->weapon_rate_slot_cap   = Xwau_FileHasText(path, "WarheadTypeCount_fg_");
	}
	XwauLines lines;
	if (out->engine_sound && Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path) &&
		Xwau_FileHasText(path, "AreStarshipAmbientEnabled")) {
		if (!XwauLines_Read(Xwau_FindFile(game, "hook_engine_sound.cfg", path, sizeof path) ? path : NULL, "",
							XWAU_RULES_CPP, &lines))
			return false;
		if (!lines.count && !XwauLines_Read(Xwau_FindFile(game, "Hooks.ini", path, sizeof path) ? path : NULL,
											"hook_engine_sound", XWAU_RULES_CPP, &lines))
			return false;
		out->starship_ambient = int_value(&lines, "AreStarshipAmbientEnabled", 1, NULL) != 0;
		XwauLines_Free(&lines);
	}
	if (!XwauLines_Read(Xwau_FindFile(game, "hook_mission_tie.cfg", path, sizeof path) ? path : NULL, "",
						XWAU_RULES_CPP, &lines))
		return false;
	if (!lines.count && !XwauLines_Read(Xwau_FindFile(game, "Hooks.ini", path, sizeof path) ? path : NULL,
										"hook_mission_tie", XWAU_RULES_CPP, &lines))
		return false;
	out->warhead_collision_damage  = int_value(&lines, "IsWarheadCollisionDamagesEnabled", 1, NULL) != 0;
	out->target_method             = int_value(&lines, "TargetCraftKeyMethod", 0, NULL);
	out->target_only_not_inspected = int_value(&lines, "TargetCraftKeySelectOnlyNotInspected", 0, NULL) != 0;
	out->damage_through_shields = int_value(&lines, "CanShootThroughtShieldOnHardDifficulty", 0, NULL) != 0;
	XwauLines_Free(&lines);
	if (!out->engine_sound)
		return true;
	XwauLines interdiction;
	if (!interdiction_lines(game, "FlightModels/Interdiction.txt", "FlightModels/default.ini", &interdiction))
		return false;
	char text[1024];
	XwauLines_Value(&interdiction, "Region", XWAU_RULES_CPP, text, sizeof text);
	const bool ok =
		int_list(text, false, "Region", "FlightModels", &out->interdiction, &out->interdiction_count);
	XwauLines_Free(&interdiction);
	return ok;
}

void XwauSettingsConfig_Free(XwauSettingsConfig* config) {
	free(config->interdiction);
	memset(config, 0, sizeof *config);
}

static void write_ints(FILE* f, const int* values, size_t count) {
	fputc('[', f);
	for (size_t i = 0; i < count; ++i)
		fprintf(f, "%s%d", i ? ", " : "", values[i]);
	fputc(']', f);
}

bool XwauMissionSettings_WriteDefaults(FILE* f, const XwauSettingsConfig* config) {
	const bool rules = config->mission_tie || config->opt_limit_engine_glows || config->weapon_switch ||
					   config->mission_tie_stats || config->weapon_rate_fire_check ||
					   config->weapon_rate_rack_check || config->starship_ambient ||
					   config->weapon_rate_slot_cap;
	if (rules)
		fputs("rules:\n", f);
	if (config->mission_tie)
		fputs("  reset_combat_music_at_mission_load: true\n  hangar_red_alert_any_mothership: true\n", f);
	if (config->opt_limit_engine_glows)
		fputs("  classic_hide_destroyed_engine_glows: true\n", f);
	if (config->weapon_switch)
		fprintf(f, "  weapon_iff_swap: %s\n", config->weapon_switch_by_iff ? "true" : "false");
	if (config->mission_tie_stats)
		fputs("  ai_attack_uses_max_speed_cache: true\n", f);
	if (config->weapon_rate_fire_check)
		fputs("  fire_needs_decharge_rate: true\n", f);
	if (config->weapon_rate_rack_check)
		fputs("  no_starship_fire_from_destroyed_racks: true\n", f);
	if (config->starship_ambient)
		fputs("  starship_ambient_sounds: true\n", f);
	if (config->weapon_rate_slot_cap)
		fputs("  no_player_warhead_slot_cap: true\n", f);
	const bool warheads     = config->mission_tie && !config->warhead_collision_damage;
	const bool shields      = config->mission_tie && config->damage_through_shields;
	const bool interdiction = config->engine_sound && config->interdiction_count;
	if (warheads || shields || interdiction)
		fputs("mission_settings:\n", f);
	if (warheads)
		fputs("  warhead_collision_damage: false\n", f);
	if (shields)
		fputs("  damage_through_shields_on_hard: true\n", f);
	if (interdiction) {
		fputs("  interdiction_regions: ", f);
		write_ints(f, config->interdiction, config->interdiction_count);
		fputc('\n', f);
	}
	return rules || interdiction;
}

/* ---- Mission numbers --------------------------------------------------------- */

/* Strips the "!KEY!" that starts a line of a mission list, as the game
 * replaces it with its text, which comes after it. */
static const char* list_text(const char* line) {
	if (line[0] == '!') {
		const char* end = strchr(line + 1, '!');
		if (end)
			return end + 1;
	}
	return line;
}

/* Splits `text` into its lines, without their line ends. Returns false
 * when out of memory. */
static bool split_lines(char* text, XwauLines* out) {
	memset(out, 0, sizeof *out);
	for (char* line = text; *line;) {
		char*        end  = strchr(line, '\n');
		const size_t len  = end ? (size_t)(end - line) : strlen(line);
		size_t       trim = len;
		while (trim && line[trim - 1] == '\r')
			--trim;
		if (!XwauLines_Add(out, line, trim)) {
			XwauLines_Free(out);
			return false;
		}
		if (!end)
			break;
		line = end + 1;
	}
	return true;
}

/* Reads the mission list `path` of `dir` as the game reads it
 * (MissionSetup_LoadMissionList): before each entry, lines that start with
 * "//" are skipped, and lines in brackets start a section. An entry is a
 * number line, a file line (with a "* " or "& " mark) and a description
 * line. */
static bool read_list(const char* path, const char* dir, XwauMissionNumbers* out) {
	size_t size = 0;
	char*  text = ReadWholeFile(path, &size);
	if (!text)
		return true;
	XwauLines lines;
	bool      ok = split_lines(text, &lines);
	free(text);
	size_t i = 0;
	while (ok && i < lines.count) {
		const char* line = lines.items[i++];
		if ((line[0] == '/' && line[1] == '/') || list_text(line)[0] == '[')
			continue;
		const int number = atoi(list_text(line));
		if (i >= lines.count)
			break;
		const char* file = list_text(lines.items[i++]);
		if ((file[0] == '*' || file[0] == '&') && file[1])
			file += 2;
		++i; /* the description */
		struct XwauMissionNumber* entry = ARRAY_PUSH(out->items, out->count);
		if (!(ok = entry != NULL))
			break;
		snprintf(entry->dir, sizeof entry->dir, "%s", dir);
		LowerCopy(entry->name, sizeof entry->name, file);
		const size_t len = strlen(entry->name);
		if (len > 4 && strcmp(entry->name + len - 4, ".tie") == 0)
			entry->name[len - 4] = 0;
		entry->number = number;
	}
	XwauLines_Free(&lines);
	return ok;
}

bool XwauMissionNumbers_Read(const char* game, const char* stock_game, XwauMissionNumbers* out) {
	memset(out, 0, sizeof *out);
	bool ok = true;
	for (size_t d = 0; ok && d < sizeof kListDirs / sizeof kListDirs[0]; ++d) {
		char rel[64], path[1024];
		snprintf(rel, sizeof rel, "%s/MISSION.LST", kListDirs[d]);
		if (Xwau_FindFile(game, rel, path, sizeof path) || Xwau_FindFile(stock_game, rel, path, sizeof path))
			ok = read_list(path, kListDirs[d], out);
	}
	if (!ok)
		XwauMissionNumbers_Free(out);
	return ok;
}

void XwauMissionNumbers_Free(XwauMissionNumbers* numbers) {
	free(numbers->items);
	memset(numbers, 0, sizeof *numbers);
}

int XwauMissionNumbers_Find(const XwauMissionNumbers* numbers, const char* dir, const char* name) {
	for (size_t i = 0; i < numbers->count; ++i)
		if (strcasecmp(numbers->items[i].dir, dir) == 0 && strcmp(numbers->items[i].name, name) == 0)
			return numbers->items[i].number;
	return -1;
}

/* ---- A mission's settings ------------------------------------------------------ */

static bool non_ascii(const char* text) {
	for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
		if (*c >= 0x80)
			return true;
	return false;
}

/* The game's limits for the texts of the order of battle. */
#define ORDER_TEXT_MAX 127
#define ORDER_LINES_MAX 32

/* Reports a text that the game can't show as XWAU does. Returns false when
 * it is too long. */
static bool check_text(const char* text, const char* what, const char* base) {
	if (strlen(text) > ORDER_TEXT_MAX) {
		Report(base, "%s '%s' has more than %d characters; it is left out", what, text, ORDER_TEXT_MAX);
		return false;
	}
	if (non_ascii(text))
		Report(base, "%s '%s' isn't ASCII, so the game may show it differently", what, text);
	return true;
}

bool XwauMissionSettings_Read(const char* game, const char* base, int number, const XwauLines* lines,
							  const XwauSettingsConfig* config, XwauMissionSettings* out) {
	memset(out, 0, sizeof *out);
	out->red_alert = out->skip_hyperspaced_messages = out->skip_arrival_iff = out->force_turret = -1;
	out->damage_through_shields = out->order_top = -1;
	bool ok                                      = true;
	if (config->mission_tie) {
		/* The hangar's red alert: stock in mission 20. */
		const bool red_alert = int_value(lines, "IsRedAlertEnabled", number == 20, base) != 0;
		if (red_alert != (number == 20))
			out->red_alert = red_alert;

		/* Message 143, stock in mission 49, and the "entering area"
		   messages, which the stock game never skips. */
		const bool hyperspaced = int_value(lines, "SkipHyperspacedMessages", number == 49, base) != 0;
		if (hyperspaced || number == 49)
			out->skip_hyperspaced_messages = hyperspaced;

		/* The arrival messages of an IFF: stock for IFF 1 in missions 49 to
		   52. XWAU compares the value with a byte, and 255 means every IFF. */
		const bool      stock_iff = number >= 49 && number <= 52;
		const long long iff       = int_value(lines, "SkipObjectsMessagesIff", stock_iff ? 1 : -1, base);
		const int       skip_iff  = iff >= 0 && iff <= 255 ? (int)iff : 256;
		if (skip_iff != (stock_iff ? 1 : 256))
			out->skip_arrival_iff = skip_iff;

		/* The forced turret: stock at 0:00:08 in mission 1. */
		const bool      turret  = int_value(lines, "ForcePlayerInTurret", number == 1, base) != 0;
		const long long hours   = int_value(lines, "ForcePlayerInTurretHours", 0, base);
		const long long minutes = int_value(lines, "ForcePlayerInTurretMinutes", 0, base);
		const long long seconds = int_value(lines, "ForcePlayerInTurretSeconds", 8, base);
		const bool      valid =
			hours >= 0 && hours <= 255 && minutes >= 0 && minutes <= 59 && seconds >= 0 && seconds <= 59;
		if (turret && !valid)
			Report(
				base,
				"ForcePlayerInTurret: the mission clock never reads %lld:%02lld:%02lld, so it never happens",
				hours, minutes, seconds);
		if (turret && valid && !(number == 1 && hours == 0 && minutes == 0 && seconds == 8)) {
			out->force_turret = 1;
			snprintf(out->turret_time, sizeof out->turret_time, "%lld:%02lld:%02lld", hours, minutes,
					 seconds);
		} else if ((!turret || !valid) && number == 1) {
			out->force_turret = 0;
		}

		out->disable_lasers   = int_value(lines, "DisablePlayerLaserShoot", 0, base) != 0;
		out->disable_warheads = int_value(lines, "DisablePlayerWarheadShoot", 0, base) != 0;
		/* The global setting is the default of flight/flight.yaml. */
		out->no_warhead_collision_damage = config->warhead_collision_damage &&
										   int_value(lines, "IsWarheadCollisionDamagesEnabled", 1, base) == 0;
		const bool shields = int_value(lines, "CanShootThroughtShieldOnHardDifficulty",
									   number == 50 ? 1 : config->damage_through_shields, base) != 0;
		if (shields != config->damage_through_shields)
			out->damage_through_shields = shields;
		out->skip_projectiles = int_value(lines, "SkipProjectilesProximityCheck", 0, base) != 0;

		/* The O key. Without flight groups, the stock key. */
		char text[1024];
		XwauLines_Value(lines, "KEY_O_TargetCraftFGs", XWAU_RULES_CPP, text, sizeof text);
		ok = int_list(text, true, "KEY_O_TargetCraftFGs", base, &out->target_fgs, &out->target_fg_count);
		if (ok && out->target_fg_count) {
			const long long method = int_value(lines, "TargetCraftKeyMethod", -1, base);
			const long long only   = int_value(lines, "TargetCraftKeySelectOnlyNotInspected", -1, base);
			out->target_key        = true;
			out->target_cycle      = (method == -1 ? config->target_method : method) == 1;
			out->target_only_not_inspected = only == -1 ? config->target_only_not_inspected : only != 0;
		}

		/* The order of battle. */
		XwauLines_Value(lines, "CampaignCraftsList", XWAU_RULES_CPP, text, sizeof text);
		ok = ok && int_list(text, false, "CampaignCraftsList", base, &out->order_fgs, &out->order_fg_count);
		out->order_list = out->order_fg_count != 0;
		for (size_t i = 0; ok && i < out->order_fg_count; ++i) {
			char key[64], name[256];
			snprintf(key, sizeof key, "CampaignCraftName_%d", out->order_fgs[i]);
			XwauLines_Value(lines, key, XWAU_RULES_CPP, name, sizeof name);
			bool seen = false;
			for (size_t k = 0; k < out->order_name_count; ++k)
				seen = seen || out->order_names[k].fg == out->order_fgs[i];
			if (seen || !name[0] || out->order_fgs[i] < 0 || !check_text(name, key, base))
				continue;
			struct XwauOrderName* added = ARRAY_PUSH(out->order_names, out->order_name_count);
			ok                          = added && (added->text = StrDup(name)) != NULL;
			if (ok)
				added->fg = out->order_fgs[i];
		}
		XwauLines_Value(lines, "CampaignCraftsListLines", XWAU_RULES_CPP, text, sizeof text);
		XwauLines words = { 0 };
		ok              = ok && Xwau_Tokenize(text, XWAU_RULES_CPP, &words);
		for (size_t i = 0; ok && i < words.count; ++i) {
			if (out->order_lines.count == ORDER_LINES_MAX) {
				Report(base, "CampaignCraftsListLines: more than %d lines; the rest are left out",
					   ORDER_LINES_MAX);
				break;
			}
			if (check_text(words.items[i], "CampaignCraftsListLines: line", base))
				ok = XwauLines_Add(&out->order_lines, words.items[i], strlen(words.items[i]));
		}
		XwauLines_Free(&words);
		if (out->order_lines.count) {
			const long long top = int_value(lines, "CampaignCraftsListLinesTop", -1, base);
			if (top >= 0 && top <= 479)
				out->order_top = (int)top;
			else if (top != -1)
				Report(base, "CampaignCraftsListLinesTop: %lld is off the screen; it is left out", top);
		}
	}
	if (ok && config->engine_sound) {
		char      txt[1200], ini[1200];
		XwauLines interdiction;
		ok = PathFormat(txt, sizeof txt, "%s_Interdiction.txt", base) &&
			 PathFormat(ini, sizeof ini, "%s.ini", base) && interdiction_lines(game, txt, ini, &interdiction);
		if (ok && interdiction.count) {
			char text[1024];
			XwauLines_Value(&interdiction, "Region", XWAU_RULES_CPP, text, sizeof text);
			ok = int_list(text, false, "Interdiction: Region", base, &out->interdiction_regions,
						  &out->interdiction_count);
			out->interdiction = true;
		}
		if (ok)
			XwauLines_Free(&interdiction);
	}
	if (!ok)
		XwauMissionSettings_Free(out);
	return ok;
}

void XwauMissionSettings_Free(XwauMissionSettings* settings) {
	free(settings->target_fgs);
	free(settings->order_fgs);
	for (size_t i = 0; i < settings->order_name_count; ++i)
		free(settings->order_names[i].text);
	free(settings->order_names);
	XwauLines_Free(&settings->order_lines);
	free(settings->interdiction_regions);
	memset(settings, 0, sizeof *settings);
}

/* Drops the values of `list` that aren't flight groups of the .TIE. */
static void check_list(int* list, size_t* count, size_t fg_count, const char* key, const char* base) {
	size_t kept = 0;
	for (size_t i = 0; i < *count; ++i) {
		if (list[i] >= 0 && (size_t)list[i] < fg_count && list[i] < 192)
			list[kept++] = list[i];
		else
			Report(base, "%s: the mission has no flight group %d; it is left out", key, list[i]);
	}
	*count = kept;
}

void XwauMissionSettings_CheckFlightGroups(XwauMissionSettings* settings, size_t fg_count, const char* base) {
	/* An emptied list still replaces the stock rule, as in XWAU. */
	check_list(settings->target_fgs, &settings->target_fg_count, fg_count, "KEY_O_TargetCraftFGs", base);
	check_list(settings->order_fgs, &settings->order_fg_count, fg_count, "CampaignCraftsList", base);
}

bool XwauMissionSettings_Any(const XwauMissionSettings* s) {
	return s->red_alert >= 0 || s->skip_hyperspaced_messages >= 0 || s->skip_arrival_iff >= 0 ||
		   s->force_turret >= 0 || s->disable_lasers || s->disable_warheads ||
		   s->no_warhead_collision_damage || s->damage_through_shields >= 0 || s->target_key ||
		   s->skip_projectiles || s->order_list || s->order_lines.count || s->interdiction;
}

static const char* yes_no(int value) { return value ? "true" : "false"; }

void XwauMissionSettings_Write(FILE* f, const XwauMissionSettings* s) {
	if (!XwauMissionSettings_Any(s))
		return;
	fputs("settings:\n", f);
	if (s->red_alert >= 0)
		fprintf(f, "  red_alert: %s\n", yes_no(s->red_alert));
	if (s->skip_hyperspaced_messages >= 0)
		fprintf(f, "  skip_hyperspaced_messages: %s\n", yes_no(s->skip_hyperspaced_messages));
	if (s->skip_arrival_iff == 255)
		fputs("  skip_arrival_messages_iff: all\n", f);
	else if (s->skip_arrival_iff == 256)
		fputs("  skip_arrival_messages_iff: none\n", f);
	else if (s->skip_arrival_iff >= 0)
		fprintf(f, "  skip_arrival_messages_iff: %d\n", s->skip_arrival_iff);
	if (s->force_turret == 0)
		fputs("  force_player_in_turret: false\n", f);
	else if (s->force_turret == 1)
		fprintf(f, "  force_player_in_turret: \"%s\"\n", s->turret_time);
	if (s->disable_lasers)
		fputs("  disable_player_lasers: true\n", f);
	if (s->disable_warheads)
		fputs("  disable_player_warheads: true\n", f);
	if (s->no_warhead_collision_damage)
		fputs("  warhead_collision_damage: false\n", f);
	if (s->damage_through_shields >= 0)
		fprintf(f, "  damage_through_shields_on_hard: %s\n", yes_no(s->damage_through_shields));
	if (s->target_key) {
		fputs("  target_key: { flight_groups: ", f);
		write_ints(f, s->target_fgs, s->target_fg_count);
		fprintf(f, ", method: %s, only_not_inspected: %s }\n", s->target_cycle ? "cycle" : "nearest",
				yes_no(s->target_only_not_inspected));
	}
	if (s->skip_projectiles)
		fputs("  skip_projectiles_proximity_check: true\n", f);
	if (s->order_list || s->order_lines.count) {
		fputs("  order_of_battle:\n", f);
		if (s->order_list) {
			fputs("    flight_groups: ", f);
			write_ints(f, s->order_fgs, s->order_fg_count);
			fputc('\n', f);
		}
		for (size_t i = 0; i < s->order_name_count; ++i) {
			fprintf(f, "%s%d: ", i ? ", " : "    names: { ", s->order_names[i].fg);
			Xwau_WriteString(f, s->order_names[i].text);
		}
		if (s->order_name_count)
			fputs(" }\n", f);
		if (s->order_lines.count) {
			fputs("    lines:\n", f);
			for (size_t i = 0; i < s->order_lines.count; ++i) {
				fputs("      - ", f);
				Xwau_WriteString(f, s->order_lines.items[i]);
				fputc('\n', f);
			}
		}
		if (s->order_top >= 0)
			fprintf(f, "    top: %d\n", s->order_top);
	}
	if (s->interdiction) {
		fputs("  interdiction_regions: ", f);
		write_ints(f, s->interdiction_regions, s->interdiction_count);
		fputc('\n', f);
	}
}
