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

/* Writes the `weapons` section, when the mod installs hook_weapon_color. */
static bool write_weapons(FILE* f, const char* game) {
	uint32_t colors[XWAU_CRAFT_WEAPONS];
	if (!XwauCraft_ReadGlobalImpactColors(game, colors))
		return false;
	fputs("weapons:\n  impact: {", f);
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon)
		fprintf(f, "%s%d: 0x%06x", weapon ? ", " : "", XWAU_CRAFT_FIRST_WEAPON + weapon, colors[weapon]);
	fputs("}\n", f);
	return true;
}

/* Reads `key` of hook_shield's settings as GetFileKeyValueInt does. */
static long long shield_value(const XwauLines* lines, const char* source, const char* key,
							  long long fallback) {
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

/* Writes the `shield_recharge` section, when the mod installs hook_shield:
 * its hook_shield.cfg, else the [hook_shield] section of Hooks.ini. */
static bool write_shield_recharge(FILE* f, const char* game) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_shield.dll", path, sizeof path) ||
		!Xwau_FileHasText(path, "RechargeRatePercent"))
		return false;
	XwauLines   lines  = { 0 };
	const char* source = "hook_shield.cfg";
	if (Xwau_FindFile(game, "hook_shield.cfg", path, sizeof path))
		XwauLines_Read(path, "", XWAU_RULES_CPP, &lines);
	if (!lines.count) {
		XwauLines_Free(&lines);
		source = "Hooks.ini";
		if (Xwau_FindFile(game, "Hooks.ini", path, sizeof path))
			XwauLines_Read(path, "hook_shield", XWAU_RULES_CPP, &lines);
	}
	long long percent = shield_value(&lines, source, "RechargeRatePercent", 100);
	if (percent < 0) {
		Report(source, "RechargeRatePercent = %lld: a negative percent isn't imported; 100 is used", percent);
		percent = 100;
	}
	fprintf(f,
			"shield_recharge:\n  starships: %s\n  starships_on_easy: %s\n  double_starfighter_shields: %s\n"
			"  rate_percent: %lld\n",
			shield_value(&lines, source, "IsShieldRechargeForStarshipsEnabled", 1) ? "true" : "false",
			shield_value(&lines, source, "IsShieldRechargeForStarshipsOnEasyDifficultyEnabled", 0) ? "true"
																								   : "false",
			shield_value(&lines, source, "IsShieldStrengthForStarfighterDoubled", 0) ? "true" : "false",
			percent);
	XwauLines_Free(&lines);
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
	const bool weapons    = write_weapons(f, game);
	const bool shields    = write_shield_recharge(f, game);
	XwauSettingsConfig_Free(&config);
	const bool        ok        = !ferror(f) && fclose(f) == 0;
	char              what[300] = "";
	const char* const parts[] = { extra_planets ? "backdrop ids 104 to 255 are planets" : NULL,
								  settings ? "the mission rules and defaults" : NULL,
								  headlights ? "the headlights" : NULL, weapons ? "the impact colours" : NULL,
								  shields ? "the shield recharge rules" : NULL };
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
