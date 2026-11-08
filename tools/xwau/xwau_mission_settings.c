#include "xwau_mission_settings.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_craft.h"
#include "xwau_object_limits.h"

static const char* const kListDirs[] = { "Missions", "Melee", "Combat" };

/* ---- Values ----------------------------------------------------------------- */

/* Appends the numbers of the list `text` (split as XWAU's Tokennize splits
 * it) to `out`, read in base 0 when `auto_base` is set, else in base 10.
 * Words that aren't numbers, which XWAU would fail on, are left out. Returns
 * false when out of memory. */
static bool int_list(const char* text, bool auto_base, int** out, size_t* count) {
	XwauLines words;
	if (!Xwau_Tokenize(text, XWAU_RULES_CPP, &words))
		return false;
	bool ok = true;
	for (size_t i = 0; ok && i < words.count; ++i) {
		long long value = 0;
		if (!(auto_base ? Xwau_StoiAuto(words.items[i], &value) : Xwau_Stoi(words.items[i], &value)))
			continue;
		int* added = ARRAY_PUSH(*out, *count);
		ok         = added != NULL;
		if (ok)
			*added = (int)value;
	}
	XwauLines_Free(&words);
	return ok;
}

/* ---- Global settings --------------------------------------------------------- */

bool XwauSettingsConfig_Read(const char* game, XwauSettingsConfig* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	out->mission_tie  = Xwau_FindFile(game, "hook_mission_tie.dll", path, sizeof path);
	out->engine_sound = Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path);
	out->opt_limit_engine_glows = Xwau_DllHasText(game, "hook_opt_limit.dll", "EngineGlows");
	out->weapon_switch = XwauCraft_ReadGlobalSwitchByIff(game, &out->weapon_switch_by_iff);
	out->mission_tie_stats      = Xwau_DllHasText(game, "hook_mission_tie.dll", "StatsProfile");
	if (Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path)) {
		out->weapon_rate_fire_check = Xwau_FileHasText(path, "_EnergyLowHighSeparation");
		out->weapon_rate_rack_check = Xwau_FileHasText(path, "Craft_Offset_292");
		out->weapon_rate_slot_cap   = Xwau_FileHasText(path, "WarheadTypeCount_fg_");
		out->weapon_rate_hardpoint_types = Xwau_FileHasText(path, "WeaponHardpointTypes");
		out->weapon_rate_rearm_statuses  = Xwau_FileHasText(path, "WarheadCapacity_00460904_Hook");
		if (Xwau_FileHasText(path, "EnableFireMeshFilter")) {
			XwauSection rate;
			if (!XwauSection_ReadHook(game, "hook_weapon_rate", &rate))
				return false;
			out->weapon_rate_mesh_filter = XwauSection_IntOr(&rate, "EnableFireMeshFilter", 1) != 0;
			XwauSection_Free(&rate);
		}
	}
	if (Xwau_DllHasText(game, "hook_mission_objects.dll", "Turrets.txt")) {
		const bool  unlimited = Xwau_DllHasText(game, "hook_mission_objects.dll", "UnlimitedTurretLaser");
		XwauSection turrets;
		out->gunner_seats = true;
		if (!XwauSection_ReadHook(game, "hook_mission_objects", &turrets))
			return false;
		out->gunner_selected_group = unlimited && XwauSection_IntOr(&turrets, "UnlimitedTurretLaser", 1) != 0;
		XwauSection_Free(&turrets);
	}
	out->crafts_count_sounds = XwauObjectLimits_HookEnabled(game) &&
							   Xwau_DllHasText(game, "hook_crafts_count.dll", "RS's crafts count");
	/* This text is the original bytes of the patch for the first droid's
	   motion. Older builds of the hook, without the droid hooks, don't have
	   it. */
	out->pilot_hangar_droids = Xwau_DllHasText(game, "hook_pilot.dll", "890DFC0D9100");
	XwauSection settings;
	if (Xwau_FindFile(game, "hook_weapon_type.dll", path, sizeof path)) {
		out->weapon_type          = true;
		out->weapon_type_missiles = Xwau_FileHasText(path, "To call the hook that fixes missile count");
	}
	if (Xwau_FindFile(game, "hook_jamming.dll", path, sizeof path)) {
		out->jamming = true;
		if (!XwauSection_ReadHook(game, "hook_jamming", &settings))
			return false;
		out->jamming_enabled = XwauSection_IntOr(&settings, "IsJammingEnabled", 1) != 0;
		XwauSection_Free(&settings);
	}
	if (out->engine_sound && Xwau_DllHasText(game, "hook_engine_sound.dll", "AreStarshipAmbientEnabled")) {
		if (!XwauSection_ReadHook(game, "hook_engine_sound", &settings))
			return false;
		out->starship_ambient = XwauSection_IntOr(&settings, "AreStarshipAmbientEnabled", 1) != 0;
		XwauSection_Free(&settings);
	}
	if (!XwauSection_ReadHook(game, "hook_mission_tie", &settings))
		return false;
	/* Without the hook, its settings do nothing. */
	out->warhead_collision_damage = XwauSection_IntOr(&settings, "IsWarheadCollisionDamagesEnabled", 1) != 0;
	out->target_method            = XwauSection_IntOr(&settings, "TargetCraftKeyMethod", 0);
	out->target_only_not_inspected =
		XwauSection_IntOr(&settings, "TargetCraftKeySelectOnlyNotInspected", 0) != 0;
	out->damage_through_shields =
		XwauSection_IntOr(&settings, "CanShootThroughtShieldOnHardDifficulty", 0) != 0;
	/* The importer doesn't know what the ranks modifier does in XWAU. */
	if (out->mission_tie && XwauSection_IntOr(&settings, "IsMissionRanksModifierEnabled", 0) != 0)
		ReportUnsupported("the hook_mission_tie key IsMissionRanksModifierEnabled");
	XwauSection_Free(&settings);
	XwauSection interdiction;
	if (!XwauSection_ReadGlobal(game, "Interdiction", XWAU_RULES_CPP, &interdiction))
		return false;
	char text[1024];
	XwauSection_Text(&interdiction, "Region", text, sizeof text);
	XwauSection_Free(&interdiction);
	/* Without hook_engine_sound.dll, XWAU never reads Region. */
	if (!out->engine_sound)
		return true;
	return int_list(text, false, &out->interdiction, &out->interdiction_count);
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
					   config->weapon_rate_slot_cap || config->gunner_seats ||
					   config->weapon_rate_mesh_filter || config->weapon_rate_hardpoint_types ||
					   config->weapon_rate_rearm_statuses || config->jamming || config->weapon_type ||
					   config->crafts_count_sounds || config->pilot_hangar_droids;
	if (rules)
		fputs("rules:\n", f);
	if (config->mission_tie)
		fputs("  reset_combat_music_at_mission_load: true\n  hangar_red_alert_any_mothership: true\n", f);
	if (config->opt_limit_engine_glows)
		fputs("  classic_hide_destroyed_engine_glows: true\n", f);
	if (config->weapon_switch)
		fprintf(f, "  weapon_iff_swap: %s\n", config->weapon_switch_by_iff ? "all" : "none");
	if (config->mission_tie_stats)
		fputs("  ai_attack_uses_max_speed_cache: true\n", f);
	if (config->weapon_rate_fire_check)
		fputs("  fire_needs_decharge_rate: true\n  tie_fighter_energy_step_always_high: true\n"
			  "  player_turret_racks_keep_charge: true\n  warhead_collision_bonus_any_target: true\n"
			  "  gun_turret_hardpoints: true\n",
			  f);
	if (config->weapon_rate_mesh_filter)
		fputs("  no_fire_from_destroyed_hardpoints: true\n", f);
	if (config->weapon_rate_hardpoint_types)
		fputs("  turbolaser_hardpoint_types: true\n", f);
	if (config->weapon_rate_rearm_statuses)
		fputs("  rearm_follows_both_statuses: true\n", f);
	if (config->weapon_rate_rack_check)
		fputs("  no_starship_fire_from_destroyed_racks: true\n", f);
	if (config->starship_ambient)
		fputs("  starship_ambient_sounds: true\n", f);
	if (config->weapon_rate_slot_cap)
		fputs("  no_player_warhead_slot_cap: true\n", f);
	if (config->gunner_seats)
		fputs("  gunner_weapon_select_in_seat: true\n  gunner_warheads_need_cockpit_hardpoint: true\n", f);
	if (config->gunner_selected_group)
		fputs("  gunner_fires_selected_group: true\n", f);
	if (config->jamming && !config->jamming_enabled)
		fputs("  player_weapon_jamming: false\n", f);
	else if (config->jamming)
		fputs("  super_star_destroyer_jamming: true\n", f);
	if (config->weapon_type)
		fputs("  two_launcher_fixes: true\n", f);
	if (config->weapon_type_missiles)
		fputs("  starship_missile_defense_any_warhead: true\n  weapon_rack_count_from_record: true\n", f);
	if (config->crafts_count_sounds)
		fputs("  overlapping_fly_by_sounds: true\n", f);
	if (config->pilot_hangar_droids)
		fputs("  hangar_droid_parts_from_model: true\n", f);
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
		fputs("  hyperspace_abort_sound_regions: ", f);
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

/* Checks a text of the order of battle. A text that isn't ASCII is counted
 * as unsupported, since the game may show it differently. Returns false when
 * it is too long. */
static bool check_text(const char* text, const char* what, const char* base) {
	if (strlen(text) > ORDER_TEXT_MAX) {
		ReportFatal(base, "%s '%s' has more than %d characters, the most that the game shows", what, text,
					ORDER_TEXT_MAX);
		return false;
	}
	if (non_ascii(text))
		ReportUnsupported("order of battle texts that aren't ASCII");
	return true;
}

bool XwauMissionSettings_Read(const char* game, const char* base, int number, const XwauSection* lines,
							  const XwauSettingsConfig* config, XwauMissionSettings* out) {
	memset(out, 0, sizeof *out);
	out->red_alert = out->skip_hyperspaced_messages = out->skip_arrival_iff = out->force_turret = -1;
	out->damage_through_shields = out->order_top = -1;
	bool ok                                      = true;
	/* Without hook_mission_tie.dll, XWAU never reads the key = value lines. */
	if (config->mission_tie) {
		/* The hangar's red alert: stock in mission 20. */
		const bool red_alert = XwauSection_IntOr(lines, "IsRedAlertEnabled", number == 20) != 0;
		if (red_alert != (number == 20))
			out->red_alert = red_alert;

		/* Message 143, stock in mission 49, and the "entering area"
		   messages, which the stock game never skips. */
		const bool hyperspaced = XwauSection_IntOr(lines, "SkipHyperspacedMessages", number == 49) != 0;
		if (hyperspaced || number == 49)
			out->skip_hyperspaced_messages = hyperspaced;

		/* The arrival messages of an IFF: stock for IFF 1 in missions 49 to
		   52. XWAU compares the value with a byte, and 255 means every IFF. */
		const bool      stock_iff = number >= 49 && number <= 52;
		const long long iff       = XwauSection_IntOr(lines, "SkipObjectsMessagesIff", stock_iff ? 1 : -1);
		const int       skip_iff  = iff >= 0 && iff <= 255 ? (int)iff : 256;
		if (skip_iff != (stock_iff ? 1 : 256))
			out->skip_arrival_iff = skip_iff;

		/* The forced turret: stock at 0:00:08 in mission 1. */
		const bool      turret  = XwauSection_IntOr(lines, "ForcePlayerInTurret", number == 1) != 0;
		const long long hours   = XwauSection_IntOr(lines, "ForcePlayerInTurretHours", 0);
		const long long minutes = XwauSection_IntOr(lines, "ForcePlayerInTurretMinutes", 0);
		const long long seconds = XwauSection_IntOr(lines, "ForcePlayerInTurretSeconds", 8);
		const bool      valid =
			hours >= 0 && hours <= 255 && minutes >= 0 && minutes <= 59 && seconds >= 0 && seconds <= 59;
		/* The mission clock never reads an invalid time, so the turret is
		   never forced. */
		if (turret && valid && !(number == 1 && hours == 0 && minutes == 0 && seconds == 8)) {
			out->force_turret = 1;
			snprintf(out->turret_time, sizeof out->turret_time, "%lld:%02lld:%02lld", hours, minutes,
					 seconds);
		} else if ((!turret || !valid) && number == 1) {
			out->force_turret = 0;
		}

		out->disable_lasers   = XwauSection_IntOr(lines, "DisablePlayerLaserShoot", 0) != 0;
		out->disable_warheads = XwauSection_IntOr(lines, "DisablePlayerWarheadShoot", 0) != 0;
		/* The global setting is the default of flight/flight.yaml. */
		out->no_warhead_collision_damage =
			config->warhead_collision_damage &&
			XwauSection_IntOr(lines, "IsWarheadCollisionDamagesEnabled", 1) == 0;
		const bool shields = XwauSection_IntOr(lines, "CanShootThroughtShieldOnHardDifficulty",
											   number == 50 ? 1 : config->damage_through_shields) != 0;
		if (shields != config->damage_through_shields)
			out->damage_through_shields = shields;
		out->skip_projectiles = XwauSection_IntOr(lines, "SkipProjectilesProximityCheck", 0) != 0;

		/* The O key. Without flight groups, the stock key. */
		char text[1024];
		XwauSection_Text(lines, "KEY_O_TargetCraftFGs", text, sizeof text);
		ok = int_list(text, true, &out->target_fgs, &out->target_fg_count);
		if (ok && out->target_fg_count) {
			const long long method = XwauSection_IntOr(lines, "TargetCraftKeyMethod", -1);
			const long long only   = XwauSection_IntOr(lines, "TargetCraftKeySelectOnlyNotInspected", -1);
			out->target_key        = true;
			out->target_cycle      = (method == -1 ? config->target_method : method) == 1;
			out->target_only_not_inspected = only == -1 ? config->target_only_not_inspected : only != 0;
		}

		/* The order of battle. */
		XwauSection_Text(lines, "CampaignCraftsList", text, sizeof text);
		ok              = ok && int_list(text, false, &out->order_fgs, &out->order_fg_count);
		out->order_list = out->order_fg_count != 0;
		for (size_t i = 0; ok && i < out->order_fg_count; ++i) {
			char key[64], name[256];
			snprintf(key, sizeof key, "CampaignCraftName_%d", out->order_fgs[i]);
			XwauSection_Text(lines, key, name, sizeof name);
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
		XwauSection_Text(lines, "CampaignCraftsListLines", text, sizeof text);
		XwauLines words = { 0 };
		ok              = ok && Xwau_Tokenize(text, XWAU_RULES_CPP, &words);
		for (size_t i = 0; ok && i < words.count; ++i) {
			if (out->order_lines.count == ORDER_LINES_MAX) {
				ReportFatal(base,
							"CampaignCraftsListLines has more than %d lines, the most that the game shows",
							ORDER_LINES_MAX);
				break;
			}
			if (check_text(words.items[i], "CampaignCraftsListLines: line", base))
				ok = XwauLines_Add(&out->order_lines, words.items[i], strlen(words.items[i]));
		}
		XwauLines_Free(&words);
		if (out->order_lines.count) {
			const long long top = XwauSection_IntOr(lines, "CampaignCraftsListLinesTop", -1);
			if (top >= 0 && top <= 479)
				out->order_top = (int)top;
			else if (top != -1)
				ReportFatal(base, "CampaignCraftsListLinesTop is %lld, and the game only takes 0 to 479",
							top);
		}
	}
	if (ok) {
		XwauSection interdiction;
		ok = XwauSection_ReadMission(game, base, "Interdiction", XWAU_RULES_CPP, &interdiction);
		/* Without hook_engine_sound.dll, XWAU never reads these lines. */
		if (ok && interdiction.lines.count && config->engine_sound) {
			char text[1024];
			XwauSection_Text(&interdiction, "Region", text, sizeof text);
			ok                = int_list(text, false, &out->interdiction_regions, &out->interdiction_count);
			out->interdiction = true;
		}
		XwauSection_Free(&interdiction);
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
static void check_list(int* list, size_t* count, size_t fg_count) {
	size_t kept = 0;
	for (size_t i = 0; i < *count; ++i)
		if (list[i] >= 0 && (size_t)list[i] < fg_count && list[i] < 192)
			list[kept++] = list[i];
	*count = kept;
}

void XwauMissionSettings_CheckFlightGroups(XwauMissionSettings* settings, size_t fg_count) {
	/* An emptied list still replaces the stock rule, as in XWAU. */
	check_list(settings->target_fgs, &settings->target_fg_count, fg_count);
	check_list(settings->order_fgs, &settings->order_fg_count, fg_count);
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
		fprintf(f, "  hide_hyperspace_exit_messages: %s\n", yes_no(s->skip_hyperspaced_messages));
	if (s->skip_hyperspaced_messages > 0)
		fputs("  hide_friendly_arrival_messages: true\n", f);
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
		fputs("  proximity_triggers_check_craft_only: true\n", f);
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
		fputs("  hyperspace_abort_sound_regions: ", f);
		write_ints(f, s->interdiction_regions, s->interdiction_count);
		fputc('\n', f);
	}
}
