#include "xwau_flight_settings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_craft.h"
#include "xwau_mission_settings.h"

/* The illuminance of the game's white sun, in W/m^2: the strength of the
 * Blender sun that matches it. */
#define HEADLIGHTS_SUN_W_PER_M2 2.5
/* XWAU's headlight gives the same light at every distance, and OpenXWA's
 * falls with the square of the distance. They give the same light at this
 * distance: about the distance to the walls of the Death Star's tunnels,
 * which are 55 to 140 m wide. */
#define HEADLIGHTS_MATCH_DISTANCE_M 50.0

/* The headlight settings of ddraw's SSAO.cfg. */
typedef struct Headlights {
	bool   ssao_enabled, auto_turn_on;
	double position[3], color[3], angle_degrees;
} Headlights;

/* Reads `count` numbers from `text` into `out`, as ddraw reads them: one
 * with atof, three with sscanf("%f, %f, %f"). Returns false when there are
 * fewer. */
static bool read_numbers(const char* text, int count, double* out) {
	float v[3] = { 0 };
	if (count == 1) {
		out[0] = atof(text);
		return true;
	}
	if (sscanf(text, "%f, %f, %f", &v[0], &v[1], &v[2]) < 3)
		return false;
	for (int i = 0; i < 3; ++i)
		out[i] = v[i];
	return true;
}

/* Reads SSAO.cfg as ddraw's LoadSSAOParams does: a line is `key = value`,
 * lines that start with ';' or '#' are comments, keys are found in any case
 * and the last line with a key wins. Without the file, the headlights are
 * off. */
static void read_headlights(const char* game, Headlights* out) {
	/* ddraw's defaults. Its default angle is given by its cosine, 0.25. */
	*out = (Headlights) { .auto_turn_on  = true,
						  .position      = { 0.0, 0.0, 20.0 },
						  .color         = { 0.85, 0.85, 0.9 },
						  .angle_degrees = acos(0.25) / 0.01745 };
	char   path[1024];
	size_t size = 0;
	char*  text = Xwau_FindFile(game, "SSAO.cfg", path, sizeof path) ? ReadWholeFile(path, &size) : NULL;
	for (char* line = text; line && *line;) {
		char* end = strchr(line, '\n');
		if (end)
			*end = 0;
		/* Like sscanf("%s = %s"), which skips the white space before the key. */
		const char* start = line;
		while (isspace((unsigned char)*start))
			++start;
		char key[128];
		int  key_length = 0;
		while (start[key_length] && !isspace((unsigned char)start[key_length]) &&
			   key_length + 1 < (int)sizeof key)
			++key_length;
		memcpy(key, start, (size_t)key_length);
		key[key_length]   = 0;
		const char* value = start + key_length;
		while (isspace((unsigned char)*value))
			++value;
		if (line[0] != ';' && line[0] != '#' && *value == '=') {
			++value;
			double number[3];
			if (strcasecmp(key, "ssao_enabled") == 0 && read_numbers(value, 1, number))
				out->ssao_enabled = number[0] != 0.0;
			else if (strcasecmp(key, "headlights_pos") == 0 || strcasecmp(key, "headlights_col") == 0) {
				if (read_numbers(value, 3, number))
					memcpy(strcasecmp(key, "headlights_pos") == 0 ? out->position : out->color, number,
						   sizeof number);
				else
					Report("SSAO.cfg", "%s needs three numbers; ddraw's default is used", key);
			} else if (strcasecmp(key, "headlights_angle") == 0 && read_numbers(value, 1, number))
				out->angle_degrees = number[0];
			else if (strcasecmp(key, "headlights_auto_turn_on") == 0 && read_numbers(value, 1, number))
				out->auto_turn_on = number[0] != 0.0;
		}
		line = end ? end + 1 : NULL;
	}
	free(text);
}

/* Writes the `headlights` section, when the mod's ddraw draws headlights.
 * ddraw's colour holds the light's tint and its brightness, relative to the
 * game's white sun. The section has the tint with its brightest channel at
 * 1, and the power of a one-sided light, as a Blender area light, that gives
 * that brightness at HEADLIGHTS_MATCH_DISTANCE_M: E = P / (pi d^2). ddraw's
 * distance (where its light ends) and ambient aren't written: they don't
 * apply to a light that falls with the distance. */
static bool write_headlights(FILE* f, const char* game) {
	char found[1024];
	if (!Xwau_FindFile(game, "ddraw.dll", found, sizeof found))
		return false;
	Headlights h;
	read_headlights(game, &h);
	double brightness = fmax(h.color[0], fmax(h.color[1], h.color[2]));
	if (!h.ssao_enabled || !(brightness > 0.0))
		return false;
	const double power = brightness * HEADLIGHTS_SUN_W_PER_M2 * 3.14159265358979 *
						 HEADLIGHTS_MATCH_DISTANCE_M * HEADLIGHTS_MATCH_DISTANCE_M;
	fprintf(f,
			"headlights:\n  position_m: [%g, %g, %g]\n  color: [%.3g, %.3g, %.3g]\n  power_w: %.0f\n"
			"  angle_degrees: %g\n  auto_turn_on: %s\n",
			h.position[0], h.position[1], h.position[2], h.color[0] / brightness, h.color[1] / brightness,
			h.color[2] / brightness, power, h.angle_degrees, h.auto_turn_on ? "true" : "false");
	return true;
}

/* Reads the settings of `hook` ("hook_shield"): its .cfg file, else its
 * section of Hooks.ini. Writes the name of the file into `source`, for the
 * reports. */
static void hook_lines(const char* game, const char* hook, XwauLines* lines, char* source, size_t cap) {
	char path[1024];
	memset(lines, 0, sizeof *lines);
	snprintf(source, cap, "%s.cfg", hook);
	if (Xwau_FindFile(game, source, path, sizeof path))
		XwauLines_Read(path, "", XWAU_RULES_CPP, lines);
	if (lines->count)
		return;
	XwauLines_Free(lines);
	snprintf(source, cap, "Hooks.ini");
	if (Xwau_FindFile(game, "Hooks.ini", path, sizeof path))
		XwauLines_Read(path, hook, XWAU_RULES_CPP, lines);
}

/* Reads `key` of a hook's settings as GetFileKeyValueInt does. */
static long long hook_value(const XwauLines* lines, const char* source, const char* key, long long fallback) {
	char      text[256];
	long long value = 0;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (!text[0])
		return fallback;
	if (!Xwau_StoiAuto(text, &value)) {
		Report(source, "%s = %s isn't a number, which XWAU would fail on; the default is used", key, text);
		return fallback;
	}
	return value;
}

/* Reads `key`, a percent of hook_weapon_rate's settings, for the
 * `weapons` section. */
static long long rate_percent(const XwauLines* lines, const char* source, const char* key) {
	const long long percent = hook_value(lines, source, key, 100);
	if (percent < 0 || percent > INT32_MAX) {
		Report(source, "%s = %lld: the percent isn't imported; 100 is used", key, percent);
		return 100;
	}
	return percent;
}

/* Reads the float `key` of hook_weapon_rate's settings, like the hook's
 * GetFileKeyValueFloat. Returns `fallback` when the key isn't set. */
static float hook_float(const XwauLines* lines, const char* source, const char* key, float fallback) {
	char  text[256];
	float value = fallback;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (text[0] && !XwauWeaponStats_Stof(text, &value)) {
		Report(source, "%s = %s isn't a number, which XWAU would fail on; the default is used", key, text);
		value = fallback;
	}
	return value;
}

/* Writes the hook's factors `keys` as the list `name`, when the DLL `dll`
 * reads them. */
static void write_factors(FILE* f, const char* dll, const XwauLines* lines, const char* source,
						  const char* name, const char* const* keys, const float* defaults, int count) {
	if (!Xwau_FileHasText(dll, keys[0]))
		return;
	fprintf(f, "  %s: [", name);
	for (int i = 0; i < count; ++i)
		fprintf(f, "%s%.9g", i ? ", " : "", (double)hook_float(lines, source, keys[i], defaults[i]));
	fputs("]\n", f);
}

/* Writes the global weapon profiles of hook_weapon_rate and the hook's settings
   for them. */
static void write_weapon_profile_settings(FILE* f, const char* game, const char* dll, const XwauLines* lines,
										  const char* source) {
	XwauWeaponStats global;
	if (XwauWeaponStats_Read(game, NULL, &global)) {
		XwauWeaponStats_Write(f, "  ", &global);
		XwauWeaponStats_Free(&global);
	}
	static const char* const rank_keys[] = {
		"NoviceRankFireratioMultiplicator",  "OfficerRankFireratioMultiplicator",
		"VeteranRankFireratioMultiplicator", "AceRankFireratioMultiplicator",
		"TopAceRankFireratioMultiplicator",  "SuperAceRankFireratioMultiplicator"
	};
	static const float rank_defaults[] = { 4.0f, 3.0f, 2.0f, 2.0f, 1.0f, 1.0f };
	write_factors(f, dll, lines, source, "fire_ratio_by_rank", rank_keys, rank_defaults, 6);
	static const char* const difficulty_keys[]     = { "DamagesMultiplicatorForDifficulty0",
													   "DamagesMultiplicatorForDifficulty1",
													   "DamagesMultiplicatorForDifficulty2" };
	static const float       difficulty_defaults[] = { 0.5f, 0.25f, 1.0f };
	write_factors(f, dll, lines, source, "damage_by_difficulty", difficulty_keys, difficulty_defaults, 3);
	if (Xwau_FileHasText(dll, "LimitLinkedWarheadFireRate") &&
		hook_value(lines, source, "LimitLinkedWarheadFireRate", 0) != 0)
		fputs("  limit_linked_warhead_fire_rate: true\n", f);
	static const char* const linking_keys[]  = { "PreventAILasersIonWhenDisabling",
												 "PreventAIIonWhenNotDisabling",
												 "PreventPlayerLinkedLasersIon" };
	static const char* const linking_names[] = { "prevent_ai_lasers_ion_when_disabling",
												 "prevent_ai_ion_when_not_disabling",
												 "prevent_player_linked_lasers_ion" };
	bool                     open            = false;
	for (int i = 0; i < 3 && Xwau_FileHasText(dll, linking_keys[i]); ++i) {
		if (hook_value(lines, source, linking_keys[i], 0) == 0)
			continue;
		fprintf(f, "%s%s: true", open ? ", " : "  linking_rules: {", linking_names[i]);
		open = true;
	}
	if (open)
		fputs("}\n", f);
	const long long count = hook_value(lines, source, "WeaponsCount", 12);
	if (Xwau_FileHasText(dll, "WeaponsCount") && count != 12)
		Report(source, "WeaponsCount = %lld isn't imported: a player keeps 12 projectile slots", count);
	if (Xwau_FileHasText(dll, "IsLaserLinksFixEnabled") &&
		hook_value(lines, source, "IsLaserLinksFixEnabled", 0))
		Report(source, "IsLaserLinksFixEnabled isn't imported: no mod's model turns it on");
}

/* Writes the `weapons` section: the impact colours, when the mod installs
 * hook_weapon_color, and the laser rates, when it installs
 * hook_weapon_rate. */
static void write_weapons(FILE* f, const char* game, bool* impact, bool* rates) {
	uint32_t colors[XWAU_CRAFT_WEAPONS];
	char     path[1024];
	*impact = XwauCraft_ReadGlobalImpactColors(game, colors);
	*rates  = Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path);
	if (!*impact && !*rates)
		return;
	fputs("weapons:\n", f);
	if (*impact) {
		fputs("  impact: {", f);
		for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon)
			fprintf(f, "%s%d: 0x%06x", weapon ? ", " : "", XWAU_CRAFT_FIRST_WEAPON + weapon, colors[weapon]);
		fputs("}\n", f);
	}
	if (!*rates)
		return;
	XwauCraftWeaponRates defaults;
	XwauCraft_ReadGlobalWeaponRates(game, &defaults);
	XwauLines lines;
	char      source[64];
	hook_lines(game, "hook_weapon_rate", &lines, source, sizeof source);
	/* The hook's settings give the AI torpedo limits that the global
	   [WeaponRate] doesn't set. */
	static const char* const torpedo_keys[] = { "MaxTorpedoCountPerPass", "MaxTorpedoCountPerTarget" };
	for (int i = 0; i < 2; ++i) {
		const int key = i ? XWAU_CRAFT_TORPEDOES_PER_TARGET : XWAU_CRAFT_TORPEDOES_PER_PASS;
		if (defaults.value[key] >= 0 || !Xwau_FileHasText(path, torpedo_keys[i]))
			continue;
		const long long value = hook_value(&lines, source, torpedo_keys[i], -1);
		if (value < -1 || value > INT32_MAX)
			Report(source, "%s = %lld isn't imported", torpedo_keys[i], value);
		else
			defaults.value[key] = (int)value;
	}
	XwauCraft_WriteWeaponRates(f, "  ", &defaults);
	if (Xwau_FileHasText(path, "DechargeRatePercent"))
		fprintf(f, "  decharge_percent: %lld\n  recharge_percent: %lld\n",
				rate_percent(&lines, source, "DechargeRatePercent"),
				rate_percent(&lines, source, "RechargeRatePercent"));
	write_weapon_profile_settings(f, game, path, &lines, source);
	XwauLines_Free(&lines);
}

/* Writes the `shield_recharge` section, when the mod installs hook_shield:
 * its hook_shield.cfg, else the [hook_shield] section of Hooks.ini. */
static bool write_shield_recharge(FILE* f, const char* game) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_shield.dll", path, sizeof path) ||
		!Xwau_FileHasText(path, "RechargeRatePercent"))
		return false;
	XwauLines lines;
	char      source[64];
	hook_lines(game, "hook_shield", &lines, source, sizeof source);
	long long percent = hook_value(&lines, source, "RechargeRatePercent", 100);
	if (percent < 0) {
		Report(source, "RechargeRatePercent = %lld: a negative percent isn't imported; 100 is used", percent);
		percent = 100;
	}
	fprintf(f,
			"shield_recharge:\n  starships: %s\n  starships_on_easy: %s\n  double_starfighter_shields: %s\n"
			"  rate_percent: %lld\n",
			hook_value(&lines, source, "IsShieldRechargeForStarshipsEnabled", 1) ? "true" : "false",
			hook_value(&lines, source, "IsShieldRechargeForStarshipsOnEasyDifficultyEnabled", 0) ? "true"
																								 : "false",
			hook_value(&lines, source, "IsShieldStrengthForStarfighterDoubled", 0) ? "true" : "false",
			percent);
	XwauLines_Free(&lines);
	return true;
}

/* Writes the `docking` section, when the mod installs hook_dock. The hook's
 * ForceDockElevation (hook_dock.cfg, else Hooks.ini) isn't imported; it is
 * reported when it is set. */
static bool write_docking(FILE* f, const char* game) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_dock.dll", path, sizeof path))
		return false;
	XwauLines lines;
	char      source[64];
	hook_lines(game, "hook_dock", &lines, source, sizeof source);
	const long long force = hook_value(&lines, source, "ForceDockElevation", -1);
	if (force >= 0)
		Report(source, "ForceDockElevation = %lld isn't imported: the models' approach heights apply", force);
	XwauLines_Free(&lines);
	fputs("docking:\n  dock_to_small_sets_big: true\n  ai_offsets_own_dock_point: true\n"
		  "  carried_offsets_carrier_dock_point: true\n  player_approach_height: true\n"
		  "  player_snaps_to_target_orientation: true\n",
		  f);
	return true;
}

bool XwauFlightSettings_Write(const char* game, const char* path) {
	char               found[1024];
	const bool         extra_planets = Xwau_FindFile(game, "hook_backdrops.dll", found, sizeof found);
	XwauSettingsConfig config;
	if (!XwauSettingsConfig_Read(game, &config)) {
		XwauSettingsConfig_Free(&config);
		return false;
	}
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		XwauSettingsConfig_Free(&config);
		return false;
	}
	fputs("# Made by xwau_import missions.\nversion: 1\n", f);
	if (extra_planets)
		fputs("backdrops:\n  extra_planets:\n    - { ids: [104, 255], first_group: 6304 }\n", f);
	const bool settings   = XwauMissionSettings_WriteDefaults(f, &config);
	const bool headlights = write_headlights(f, game);
	bool       impact, rates;
	write_weapons(f, game, &impact, &rates);
	const bool shields    = write_shield_recharge(f, game);
	const bool docking    = write_docking(f, game);
	XwauSettingsConfig_Free(&config);
	const bool        ok        = !ferror(f) && fclose(f) == 0;
	char              what[300] = "";
	const char* const parts[]   = { extra_planets ? "backdrop ids 104 to 255 are planets" : NULL,
									settings ? "the mission rules and defaults" : NULL,
									headlights ? "the headlights" : NULL,
									impact ? "the impact colours" : NULL,
									rates ? "the laser rates" : NULL,
									shields ? "the shield recharge rules" : NULL,
									docking ? "the docking rules" : NULL };
	for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
		if (parts[i])
			snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s", what[0] ? ", " : "", parts[i]);
	}
	if (!ok)
		Report(NULL, "cannot write %s", path);
	else if (what[0])
		Report(NULL, "wrote %s: %s", path, what);
	else
		Report(NULL, "%s has no flight settings", path);
	return ok;
}
