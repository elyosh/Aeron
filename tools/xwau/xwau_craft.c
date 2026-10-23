#include "xwau_craft.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_gunner_seats.h"
#include "xwau_sound_banks.h"
#include "xwau_stats_profiles.h"

/* The slots of XWAU's reticle hook (g_imageIndices in reticle.cpp). */
static const int g_reticleSlots[] = { 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20, 21, 22 };

uint32_t XwauCraft_HudColor(float r, float g, float b) {
	return 0xffu << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

void XwauCraft_ReadReticle(const char* reticle_txt, const char* ini, XwauCraftHud* hud) {
	XwauLines   lines  = { 0 };
	const char* source = reticle_txt;
	if (reticle_txt)
		XwauLines_Read(reticle_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "Reticle", XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; i < sizeof g_reticleSlots / sizeof g_reticleSlots[0]; ++i) {
		const int slot = g_reticleSlots[i];
		char      key[32], value[64];
		long long number;
		snprintf(key, sizeof key, "Reticle_%d", slot);
		XwauLines_Value(&lines, key, XWAU_RULES_CPP, value, sizeof value);
		if (!value[0])
			continue;
		/* The hook reads the value with std::stoi(value, 0, 0), which throws
		   when there is no number. */
		if (!Xwau_StoiAuto(value, &number)) {
			Report(source, "%s = %s isn't a number; the slot keeps its own image", key, value);
			continue;
		}
		if (number == -1)
			continue;
		hud->has_reticle[slot] = true;
		hud->reticle[slot]     = (int)number;
	}
	XwauLines_Free(&lines);
}

/* Reads "<key> = r, g, b" as LoadGeneric3DCoords does: three numbers after
 * the '='. */
static bool read_hud_color(const char* ini, const char* line, const char* key, uint32_t* out) {
	const char* equals = strchr(line, '=');
	float       rgb[3];
	if (!equals || sscanf(equals + 1, "%f, %f, %f", &rgb[0], &rgb[1], &rgb[2]) < 3) {
		Report(ini, "%s needs three numbers; it is ignored", key);
		return false;
	}
	/* uint32_t(x * 255) has no meaning for a negative value. */
	if (rgb[0] < 0 || rgb[1] < 0 || rgb[2] < 0) {
		Report(ini, "%s has a negative value; it is ignored", key);
		return false;
	}
	*out = XwauCraft_HudColor(rgb[0], rgb[1], rgb[2]);
	return true;
}

void XwauCraft_ReadHudColors(const char* ini, XwauCraftHud* hud) {
	FILE* f = ini ? fopen(ini, "r") : NULL;
	if (!f)
		return;
	/* The wrapper reads lines into a 256-byte buffer. */
	char line[256];
	bool in_section = false;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#' || line[0] == '\0')
			continue;
		if (line[0] == '[')
			in_section = strstr(line, "[HUDColor]") != NULL;
		char param[128], value[128];
		if (!in_section || sscanf(line, "%127s = %127s", param, value) < 1)
			continue;
		uint32_t color;
		if (strcasecmp(param, "HUDBorderColor") == 0 && read_hud_color(ini, line, param, &color)) {
			hud->has_border = true;
			hud->border     = color;
		} else if (strcasecmp(param, "HUDInnerColor") == 0 && read_hud_color(ini, line, param, &color)) {
			hud->has_inner = true;
			hud->inner     = color;
		}
	}
	fclose(f);
}

/* The largest MGLT limit that the file holds. XWAU's limit is mglt * 2.25
 * in raw speed units, and a speed is at most 65535, so a larger limit hides
 * the glow all the same. */
#define XWAU_CRAFT_MAX_MGLT 100000000

/* Reads one line of engine glows. Returns false, with a report, when a value
 * isn't a number that fits in an int. */
static bool read_engine_glow(const char* source, const char* line, XwauCraftEngineGlows* glows) {
	XwauLines tokens = { 0 };
	long long values[3];
	bool      ok = true;
	if (!Xwau_Tokenize(line, XWAU_RULES_CPP, &tokens)) {
		Report(source, "out of memory");
		return false;
	}
	if (tokens.count < 3) {
		XwauLines_Free(&tokens);
		return true;
	}
	for (int i = 0; ok && i < 3; ++i)
		ok = Xwau_StoiAuto(tokens.items[i], &values[i]) && values[i] >= INT_MIN && values[i] <= INT_MAX;
	XwauLines_Free(&tokens);
	if (!ok) {
		/* The hook reads the values with std::stoi(value, 0, 0), which throws. */
		Report(source, "engine glow line '%s' has a value that isn't a number; it is ignored", line);
		return false;
	}
	if (values[0] < 0 || values[0] >= XWAU_CRAFT_ENGINE_GLOWS) {
		Report(source, "engine glow line '%s' names a glow outside 0 to 255; it is ignored", line);
		return false;
	}
	const int glow = (int)values[0];
	if (glows->has[glow])
		return true;
	glows->has[glow]         = true;
	glows->min_percent[glow] = values[1] > 0 ? (int)values[1] : 0;
	glows->min_mglt[glow] =
		values[2] > 0 ? (int)(values[2] < XWAU_CRAFT_MAX_MGLT ? values[2] : XWAU_CRAFT_MAX_MGLT) : 0;
	return true;
}

void XwauCraft_ReadEngineGlows(const char* glows_txt, const char* ini, XwauCraftEngineGlows* glows) {
	XwauLines   lines  = { 0 };
	const char* source = glows_txt;
	if (glows_txt)
		XwauLines_Read(glows_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "EngineGlows", XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; i < lines.count; ++i)
		read_engine_glow(source, lines.items[i], glows);
	XwauLines_Free(&lines);
}

/* The impact colour group of each weapon type 280 to 307 (hook_weapon_color,
 * GetWeaponImpactColor): 0 is 3100 (blue) to 4, 3500 (yellow). */
static const uint8_t     g_impactGroups[XWAU_CRAFT_WEAPONS] = { 1, 1, 2, 2, 0, 0, 4, 4, 1, 2, 0, 4, 4, 4,
																4, 3, 0, 1, 3, 4, 4, 1, 1, 2, 2, 2, 2, 4 };
static const char* const g_impactGroupKeys[5] = { "WeaponImpactColor_3100", "WeaponImpactColor_3200",
												  "WeaponImpactColor_3300", "WeaponImpactColor_3400",
												  "WeaponImpactColor_3500" };
/* The hook's colours when its settings don't set a group. */
static const uint32_t    g_impactGroupDefaults[5]   = { 0xff0000ffu, 0xffff0000u, 0xff00ff00u, 0xffff00ffu,
														0xffffff00u };
static const char* const g_barKeys[XWAU_CRAFT_BARS] = { "EnergyBarColorLaserHigh", "EnergyBarColorLaserLow",
														"EnergyBarColorIonHigh", "EnergyBarColorIonLow" };

int XwauCraft_ImpactGroup(int weapon) { return g_impactGroups[weapon]; }

uint32_t XwauCraft_ImpactColor(uint32_t argb) {
	const uint32_t r = argb >> 16 & 0xff, g = argb >> 8 & 0xff, b = argb & 0xff;
	/* The hook stores the colour as RGB565, rounded, and keeps the stock
	 * mark when that is 0. */
	if ((r * 62 + 255) / 510 == 0 && (g * 126 + 255) / 510 == 0 && (b * 62 + 255) / 510 == 0)
		return 0;
	return argb & 0xffffff;
}

/* Writes the hook's key for markings `markings` (GetMarkingsKey): `key`, or
 * `key`_fg<markings>_. */
static void markings_key(char* out, size_t cap, const char* key, int markings) {
	if (markings < 0)
		snprintf(out, cap, "%s", key);
	else
		snprintf(out, cap, "%s_fg%d_", key, markings);
}

/* Reads a number key as GetFileKeyValueInt does. Returns `fallback` when
 * the key is missing, or when its value isn't a number, which is
 * reported. */
static int int_key(const XwauLines* lines, const char* source, const char* key, int fallback) {
	char      value[256];
	long long number;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return fallback;
	if (!Xwau_StoiAuto(value, &number) || number < INT_MIN || number > INT_MAX) {
		Report(source, "%s = %s isn't a number; it is ignored", key, value);
		return fallback;
	}
	return (int)number;
}

/* Reads a markings key. Returns -1 when it isn't set. */
static int markings_value(const XwauLines* lines, const char* source, const char* key) {
	return int_key(lines, source, key, -1);
}

/* Reads a colour key as GetFileKeyValueUnsignedInt does. Returns false when
 * the key is missing, or when its value isn't a number, which is
 * reported. */
static bool color_value(const XwauLines* lines, const char* source, const char* key, uint32_t* out) {
	char value[256];
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return false;
	if (!Xwau_StoulHex(value, out)) {
		Report(source, "%s = %s isn't a hexadecimal number; it is ignored", key, value);
		return false;
	}
	return true;
}

static void set_impact_group(XwauCraftWeaponColors* colors, int group, uint32_t argb) {
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (g_impactGroups[weapon] == group) {
			colors->has_impact[weapon] = true;
			colors->impact[weapon]     = XwauCraft_ImpactColor(argb);
		}
	}
}

/* Reads the keys of markings `markings`, or the keys without _fg when it is
 * -1. Returns whether a key sets something. */
static bool read_weapon_colors(const XwauLines* lines, const char* source, int markings,
							   XwauCraftWeaponColors* colors) {
	char     prefix[64], key[96];
	bool     any = false;
	uint32_t color;
	markings_key(prefix, sizeof prefix, "WeaponColor", markings);
	int variant = markings_value(lines, source, prefix);
	if (variant != -1) {
		colors->has_default     = true;
		colors->default_variant = (uint8_t)variant;
		any                     = true;
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		snprintf(key, sizeof key, "%s%d", prefix, XWAU_CRAFT_FIRST_WEAPON + weapon);
		variant = markings_value(lines, source, key);
		if (variant != -1) {
			colors->has_variant[weapon] = true;
			colors->variant[weapon]     = (uint8_t)variant;
			any                         = true;
		}
	}
	for (int group = 0; group < 5; ++group) {
		markings_key(key, sizeof key, g_impactGroupKeys[group], markings);
		/* Without _fg, a group key replaces the global colour, even with 0. */
		if (color_value(lines, source, key, &color) && (markings < 0 || color != 0)) {
			set_impact_group(colors, group, color);
			any = true;
		}
	}
	markings_key(prefix, sizeof prefix, "WeaponImpactColor", markings);
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		snprintf(key, sizeof key, "%s%d", prefix, XWAU_CRAFT_FIRST_WEAPON + weapon);
		if (color_value(lines, source, key, &color) && color != 0) {
			colors->has_impact[weapon] = true;
			colors->impact[weapon]     = XwauCraft_ImpactColor(color);
			any                        = true;
		}
	}
	for (int bar = 0; bar < XWAU_CRAFT_BARS; ++bar) {
		markings_key(key, sizeof key, g_barKeys[bar], markings);
		if (color_value(lines, source, key, &color) && color != 0) {
			colors->has_bar[bar] = true;
			colors->bar[bar]     = color;
			any                  = true;
		}
	}
	return any;
}

/* Returns the markings value m of a line whose key has "_fg<m>_", written
 * as the hook writes it, or -1. */
static int line_markings(const char* line) {
	const char* equals = strchr(line, '=');
	for (const char* fg = line; (fg = strstr(fg, "_fg")) != NULL && (!equals || fg < equals); ++fg) {
		const char* digit = fg + 3;
		int         value = 0, count = 0;
		for (; *digit >= '0' && *digit <= '9' && count < 4; ++digit, ++count)
			value = value * 10 + (*digit - '0');
		if (count && *digit == '_' && (count == 1 || fg[3] != '0') && value <= 255)
			return value;
	}
	return -1;
}

void XwauCraft_ReadWeapons(const char* weapons_txt, const char* ini, XwauCraftWeapons* weapons) {
	XwauLines   lines  = { 0 };
	const char* source = weapons_txt;
	if (weapons_txt)
		XwauLines_Read(weapons_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "WeaponColor", XWAU_RULES_CPP, &lines);
	}
	read_weapon_colors(&lines, source, -1, &weapons->colors);
	const int switch_by_iff = int_key(&lines, source, "WeaponSwitchBasedOnIff", -1);
	weapons->switch_by_iff  = switch_by_iff == -1 ? -1 : switch_by_iff != 0;
	for (size_t i = 0; i < lines.count; ++i) {
		const int markings = line_markings(lines.items[i]);
		if (markings >= 0 && !weapons->has_markings[markings])
			weapons->has_markings[markings] =
				read_weapon_colors(&lines, source, markings, &weapons->by_markings[markings]);
		const char* key = lines.items[i];
		while (*key == ' ' || *key == '\t')
			++key;
		if (strncasecmp(key, "WeaponLightColor", 16) == 0)
			weapons->light_colors = true;
	}
	XwauLines_Free(&lines);
}

/* Reads the global settings of hook_weapon_color: its hook_weapon_color.cfg,
 * else the [hook_weapon_color] section of Hooks.ini. Returns their file's
 * name. */
static const char* global_weapon_lines(const char* game, XwauLines* lines) {
	char        path[1024];
	const char* source = "hook_weapon_color.cfg";
	memset(lines, 0, sizeof *lines);
	if (Xwau_FindFile(game, "hook_weapon_color.cfg", path, sizeof path))
		XwauLines_Read(path, "", XWAU_RULES_CPP, lines);
	if (lines->count == 0) {
		XwauLines_Free(lines);
		source = "Hooks.ini";
		if (Xwau_FindFile(game, "Hooks.ini", path, sizeof path))
			XwauLines_Read(path, "hook_weapon_color", XWAU_RULES_CPP, lines);
	}
	return source;
}

void XwauCraft_ReadShield(const char* shield_txt, const char* ini, XwauCraftShield* shield) {
	XwauLines   lines  = { 0 };
	const char* source = shield_txt;
	memset(shield, 0, sizeof *shield);
	if (shield_txt)
		XwauLines_Read(shield_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "Shield", XWAU_RULES_CPP, &lines);
	}
	if (lines.count) {
		shield->has           = true;
		shield->per_generator = int_key(&lines, source, "UseGenerator", 0) == 1;
		shield->rate =
			int_key(&lines, source, shield->per_generator ? "PerGeneratorRechargeRate" : "RechargeRate", 0);
		shield->player_rate   = int_key(&lines, source, "PlayerRechargeRate", 0);
		shield->delay_seconds = int_key(&lines, source, "RechargeRateDelay", 0);
	}
	XwauLines_Free(&lines);
}

/* The mesh types, in the game's order, as the model file names them. */
static const char* const g_meshTypeNames[XWAU_CRAFT_MESH_TYPES] = {
	"default",
	"main_hull",
	"wing",
	"fuselage",
	"gun_turret",
	"small_gun",
	"engine",
	"bridge",
	"shield_generator",
	"energy_generator",
	"launcher",
	"communication_system",
	"beam_system",
	"command_system",
	"docking_platform",
	"landing_platform",
	"hangar",
	"cargo_pod",
	"misc_hull",
	"antenna",
	"rotary_wing",
	"rotary_gun_turret",
	"rotary_launcher",
	"rotary_comm_system",
	"rotary_beam_system",
	"rotary_command_system",
	"hatch",
	"custom",
	"weapon_system_1",
	"weapon_system_2",
	"power_regenerator",
	"reactor",
};

void XwauCraft_ReadComponents(const char* strength_txt, const char* ini, XwauCraftComponents* components) {
	XwauLines   lines  = { 0 };
	const char* source = strength_txt;
	memset(components, 0, sizeof *components);
	memset(components->strength, 0xff, sizeof components->strength);
	if (strength_txt)
		XwauLines_Read(strength_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "SubcomponentsStrength", XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; i < lines.count; ++i) {
		long long value;
		if (!Xwau_Stoi(lines.items[i], &value)) {
			Report(source,
				   "[SubcomponentsStrength]: \"%s\" isn't a number, which would crash XWAU; the section is "
				   "left out",
				   lines.items[i]);
			memset(components->strength, 0xff, sizeof components->strength);
			break;
		}
		if (i >= XWAU_CRAFT_MESH_TYPES || value < 0)
			continue;
		if (value > 255) {
			Report(source,
				   "[SubcomponentsStrength]: %s = %lld doesn't fit in a component's health; it is left out",
				   g_meshTypeNames[i], value);
			continue;
		}
		components->strength[i] = (int)value;
	}
	for (int type = 0; type < XWAU_CRAFT_MESH_TYPES; ++type)
		components->has = components->has || components->strength[type] >= 0;
	XwauLines_Free(&lines);
}

static void write_components(FILE* f, const XwauCraftComponents* components) {
	bool first = true;
	fputs("component_strength: {", f);
	for (int type = 0; type < XWAU_CRAFT_MESH_TYPES; ++type) {
		if (components->strength[type] < 0)
			continue;
		fprintf(f, "%s%s: %d", first ? "" : ", ", g_meshTypeNames[type], components->strength[type]);
		first = false;
	}
	fputs("}\n", f);
}

static const char* const g_engineSoundNames[] = { "xwing",   "ywing",     "awing",  "tie",
												  "gunboat", "corellian", "falcon", "slave" };
static const char* const g_washNames[]        = { "normal", "starship" };
static const char* const g_takeOffNames[]     = { "gear", "falcon" };
static const char* const g_hyperNames[]       = { "rebel", "imperial", "imperial_zoom" };
/* The weapon behaviours of hook_engine_sound, and their names in the model
 * file. The first one, `plain`, is for any value that isn't in the list. */
static const char* const g_behaviorKeys[] = {
	"",       "CorellianTransport", "TieFighter", "TieFighterWithIon", "Rebel", "RebelTurbo",
	"Empire", "EmpireTurbo",        "Falcon",     "FalconTurbo",       "Ion",   "IonTurbo"
};
static const char* const g_behaviorNames[] = {
	"plain",  "corellian_transport", "tie_fighter", "tie_fighter_with_ion", "rebel", "rebel_turbo",
	"empire", "empire_turbo",        "falcon",      "falcon_turbo",         "ion",   "ion_turbo"
};

void XwauCraft_ReadSound(const char* sound_txt, const char* ini, int range_bank, XwauCraftSound* sound) {
	XwauLines   lines  = { 0 };
	const char* source = sound_txt;
	memset(sound, 0, sizeof *sound);
	sound->hyper   = -1;
	sound->weapons = -1;
	if (sound_txt)
		XwauLines_Read(sound_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "Sound", XWAU_RULES_CPP, &lines);
	}
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		return;
	}
	int value = int_key(&lines, source, "EngineSoundInterior", 0);
	if (value >= 1 && value <= 7)
		sound->interior = value;
	else if (value)
		Report(source, "EngineSoundInterior = %d plays nothing in XWAU; it is left out", value);
	value = int_key(&lines, source, "EngineSoundFlyBy", 0);
	if (value % 10 >= 1 && value % 10 <= 8 && value / 10 <= 2 && value > 0) {
		sound->fly_by      = value % 10;
		sound->fly_by_mode = value / 10;
	} else if (value) {
		Report(source, "EngineSoundFlyBy = %d plays nothing in XWAU; it is left out", value);
	}
	value = int_key(&lines, source, "EngineSoundWash", 0);
	if (value)
		sound->wash = value == 2 ? 2 : 1;
	value = int_key(&lines, source, "EngineSoundTakeOff", 0);
	if (value)
		sound->take_off = value == 2 ? 2 : 1;
	value = int_key(&lines, source, "HyperSoundBehavior", -1);
	if (value >= 0 && value <= 2)
		sound->hyper = value;
	else if (value != -1)
		Report(source, "HyperSoundBehavior = %d plays no start sound in XWAU; it is left out", value);
	char text[256];
	XwauLines_Value(&lines, "WeaponSoundBehavior", XWAU_RULES_CPP, text, sizeof text);
	if (text[0]) {
		sound->weapons = 0;
		for (int i = 1; i < (int)(sizeof g_behaviorKeys / sizeof g_behaviorKeys[0]); ++i)
			if (strcmp(text, g_behaviorKeys[i]) == 0)
				sound->weapons = i;
		if (sound->weapons == 0 && strcmp(text, "0") != 0)
			Report(source,
				   "WeaponSoundBehavior = %s isn't a behaviour that XWAU knows, so each weapon plays its own "
				   "sound",
				   text);
	}
	char past[256] = "";
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		char start_key[64], count_key[64];
		snprintf(start_key, sizeof start_key, "WeaponSoundRangeStart_%d", XWAU_CRAFT_FIRST_WEAPON + weapon);
		snprintf(count_key, sizeof count_key, "WeaponSoundRangeCount_%d", XWAU_CRAFT_FIRST_WEAPON + weapon);
		const int first = int_key(&lines, source, start_key, -1);
		const int count = int_key(&lines, source, count_key, -1);
		if (first < 0 || count < 0)
			continue;
		if (count == 0)
			Report(source, "%s = 0 would crash XWAU; the range is left out", count_key);
		else if (first + count > range_bank)
			snprintf(past + strlen(past), sizeof past - strlen(past), "%s%d", past[0] ? ", " : "",
					 XWAU_CRAFT_FIRST_WEAPON + weapon);
		else {
			sound->range_first[weapon] = first;
			sound->range_count[weapon] = count;
		}
	}
	if (past[0])
		Report(
			source,
			"the weapon sound ranges of weapon types %s are past the %d sounds of the WeaponRange bank; XWAU "
			"drops them, and so does the import",
			past, range_bank);
	XwauLines_Value(&lines, "EngineSlowing", XWAU_RULES_CPP, text, sizeof text);
	sound->engine_slowing = text[0] != 0;
	static const char* const offset_keys[3]   = { "AmbientSoundOffsetX", "AmbientSoundOffsetY",
												  "AmbientSoundOffsetZ" };
	static const char* const distance_keys[3] = { "AmbientSoundDistanceX", "AmbientSoundDistanceZ",
												  "AmbientSoundDistanceY" };
	for (int axis = 0; axis < 3; ++axis)
		sound->ambient_offset[axis] = int_key(&lines, source, offset_keys[axis], 0);
	const int  distance     = int_key(&lines, source, "AmbientSoundDistance", INT_MIN);
	const bool has_distance = distance != INT_MIN;
	for (int axis = 0; axis < 3; ++axis) {
		const int value = int_key(&lines, source, distance_keys[axis], 0);
		if (value != 0) {
			sound->has_ambient_distance[axis] = true;
			sound->ambient_distance[axis]     = value;
		} else if (has_distance) {
			sound->has_ambient_distance[axis] = true;
			sound->ambient_distance[axis]     = distance != 0 ? distance : 1;
		}
	}
	XwauLines_Free(&lines);
}

bool XwauCraft_HasSound(const XwauCraftSound* sound) {
	bool set = sound->interior || sound->fly_by || sound->wash || sound->take_off || sound->hyper >= 0 ||
			   sound->weapons >= 0;
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon)
		set = set || sound->range_count[weapon];
	for (int axis = 0; axis < 3; ++axis)
		set = set || sound->ambient_offset[axis] || sound->has_ambient_distance[axis];
	return set;
}

static void write_sound(FILE* f, const XwauCraftSound* sound) {
	fputs("sound:\n", f);
	if (sound->interior)
		fprintf(f, "  interior: %s\n", g_engineSoundNames[sound->interior - 1]);
	if (sound->fly_by)
		fprintf(f, "  fly_by: %s\n", g_engineSoundNames[sound->fly_by - 1]);
	if (sound->fly_by && sound->fly_by_mode)
		fprintf(f, "  fly_by_mode: %s\n", sound->fly_by_mode == 1 ? "2d" : "3d");
	if (sound->wash)
		fprintf(f, "  engine_wash: %s\n", g_washNames[sound->wash - 1]);
	if (sound->take_off)
		fprintf(f, "  take_off: %s\n", g_takeOffNames[sound->take_off - 1]);
	if (sound->weapons >= 0)
		fprintf(f, "  weapons: %s\n", g_behaviorNames[sound->weapons]);
	bool open = false;
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (!sound->range_count[weapon])
			continue;
		fprintf(f, "%s%d: {first: %d, count: %d}", open ? ", " : "  weapon_ranges: {",
				XWAU_CRAFT_FIRST_WEAPON + weapon, sound->range_first[weapon], sound->range_count[weapon]);
		open = true;
	}
	if (open)
		fputs("}\n", f);
	if (sound->hyper >= 0)
		fprintf(f, "  hyper: %s\n", g_hyperNames[sound->hyper]);
	const int*  offset     = sound->ambient_offset;
	const int*  distance   = sound->ambient_distance;
	const bool* has        = sound->has_ambient_distance;
	const bool  has_offset = offset[0] || offset[1] || offset[2];
	if (!has_offset && !has[0] && !has[1] && !has[2])
		return;
	fputs("  ambient: {", f);
	if (has_offset)
		fprintf(f, "offset_m: [%d, %d, %d]", offset[0], offset[1], offset[2]);
	if (has[0] && has[1] && has[2] && distance[0] == distance[1] && distance[1] == distance[2]) {
		fprintf(f, "%sdistance_m: %d", has_offset ? ", " : "", distance[0]);
	} else if (has[0] || has[1] || has[2]) {
		fprintf(f, "%sdistance_m: [", has_offset ? ", " : "");
		for (int axis = 0; axis < 3; ++axis) {
			if (has[axis])
				fprintf(f, "%s%d", axis ? ", " : "", distance[axis]);
			else
				fprintf(f, "%snull", axis ? ", " : "");
		}
		fputc(']', f);
	}
	fputs("}\n", f);
}

static void write_shield(FILE* f, const XwauCraftShield* shield) {
	fprintf(f, "shield: {%s: %lld", shield->per_generator ? "per_generator_rate" : "rate", shield->rate);
	if (shield->player_rate)
		fprintf(f, ", player_rate: %lld", shield->player_rate);
	if (shield->delay_seconds)
		fprintf(f, ", delay_seconds: %lld", shield->delay_seconds);
	fputs("}\n", f);
}

/* The [WeaponRate] keys, and their groups and names in the model and flight
 * files. `ship_first` marks the keys that hook_weapon_rate reads from the
 * model's section first, also when the global section has lines. */
static const struct {
	const char* key;
	const char* group;
	const char* name;
	bool        ship_first;
} g_rateKeys[XWAU_CRAFT_WEAPON_RATES] = {
	{ "DechargeRate", "rates", "decharge", false },
	{ "RechargeRate", "rates", "recharge", false },
	{ "CooldownTimeFactor", "rates", "cooldown", false },
	{ "DechargeRatePercent", "rates", "decharge_percent", false },
	{ "RechargeRatePercent", "rates", "recharge_percent", false },
	{ "EnergyTransferRate", "energy_transfer", "rate", true },
	{ "EnergyTransferRatePenalty", "energy_transfer", "penalty", true },
	{ "EnergyTransferWeaponLimit", "energy_transfer", "laser_limit", true },
	{ "EnergyTransferShieldLimit", "energy_transfer", "shield_limit", true },
	{ "MaxTorpedoCountPerPass", "ai_torpedoes", "per_pass", false },
	{ "MaxTorpedoCountPerTarget", "ai_torpedoes", "per_target", false },
	{ "IsImpactSpinningEnabled", "impact_spin", "enabled", false },
	{ "ImpactSpinningSpeedFactorPercent", "impact_spin", "speed_percent", false },
	{ "ImpactSpinningAngleFactorPercent", "impact_spin", "angle_percent", false },
	{ "MaxSystemDamages", "system_damage", "max", false },
	{ "Weapon284_LaserIon_Damages", "system_damage", "284", false },
	{ "Weapon285_LaserIonTurbo_Damages", "system_damage", "285", false },
	{ "Weapon290_LaserIonTurbo_Damages", "system_damage", "290", false },
	{ "Weapon296_MagPulse_Damages", "system_damage", "296", false },
};

/* Reads the rates of `lines`. */
static void read_weapon_rates(const XwauLines* lines, const char* source, XwauCraftWeaponRates* rates) {
	for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key) {
		rates->value[key] = int_key(lines, source, g_rateKeys[key].key, -1);
		/* The hook turns the spin on for any value but 0. */
		if (key == XWAU_CRAFT_IMPACT_SPIN && rates->value[key] != -1)
			rates->value[key] = rates->value[key] != 0;
		if (rates->value[key] < -1) {
			Report(source, "%s = %d: a value below 0 isn't imported", g_rateKeys[key].key, rates->value[key]);
			rates->value[key] = -1;
		}
	}
}

void XwauCraft_ReadWeaponRates(const char* rate_txt, const char* ini, XwauCraftWeaponRates* rates) {
	XwauLines   lines  = { 0 };
	const char* source = rate_txt;
	memset(rates->value, 0xff, sizeof rates->value);
	if (rate_txt)
		XwauLines_Read(rate_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "WeaponRate", XWAU_RULES_CPP, &lines);
	}
	read_weapon_rates(&lines, source, rates);
	XwauLines_Free(&lines);
}

bool XwauCraft_ReadGlobalWeaponRates(const char* game, XwauCraftWeaponRates* rates) {
	XwauLines   lines = { 0 };
	char        path[1024];
	const char* source = "FlightModels/WeaponRate.txt";
	memset(rates->value, 0xff, sizeof rates->value);
	if (Xwau_FindFile(game, source, path, sizeof path))
		XwauLines_Read(path, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = "FlightModels/default.ini";
		if (Xwau_FindFile(game, source, path, sizeof path))
			XwauLines_Read(path, "WeaponRate", XWAU_RULES_CPP, &lines);
	}
	read_weapon_rates(&lines, source, rates);
	const bool has_lines = lines.count != 0;
	XwauLines_Free(&lines);
	return has_lines;
}

bool XwauCraft_HasWeaponRates(const XwauCraftWeaponRates* rates) {
	for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key) {
		if (rates->value[key] >= 0)
			return true;
	}
	return false;
}

bool XwauCraft_HasDefaultFirstRates(const XwauCraftWeaponRates* rates) {
	for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key) {
		if (rates->value[key] >= 0 && !g_rateKeys[key].ship_first)
			return true;
	}
	return false;
}

void XwauCraft_WriteWeaponRates(FILE* f, const char* indent, const XwauCraftWeaponRates* rates) {
	bool open = false;
	for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key) {
		/* The keys of a group follow each other in the table. */
		if (open && strcmp(g_rateKeys[key - 1].group, g_rateKeys[key].group) != 0) {
			fputs("}\n", f);
			open = false;
		}
		if (rates->value[key] < 0)
			continue;
		if (open)
			fputs(", ", f);
		else
			fprintf(f, "%s%s: {", indent, g_rateKeys[key].group);
		open = true;
		if (key == XWAU_CRAFT_IMPACT_SPIN)
			fprintf(f, "%s: %s", g_rateKeys[key].name, rates->value[key] ? "true" : "false");
		else
			fprintf(f, "%s: %d", g_rateKeys[key].name, rates->value[key]);
	}
	if (open)
		fputs("}\n", f);
}

bool XwauCraft_ReadGlobalImpactColors(const char* game, uint32_t colors[XWAU_CRAFT_WEAPONS]) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_color.dll", path, sizeof path))
		return false;
	XwauLines         lines;
	const char* const source = global_weapon_lines(game, &lines);
	uint32_t          groups[5];
	for (int group = 0; group < 5; ++group) {
		if (!color_value(&lines, source, g_impactGroupKeys[group], &groups[group]))
			groups[group] = g_impactGroupDefaults[group];
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon)
		colors[weapon] = XwauCraft_ImpactColor(groups[g_impactGroups[weapon]]);
	XwauLines_Free(&lines);
	return true;
}

bool XwauCraft_ReadGlobalSwitchByIff(const char* game, bool* on) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_color.dll", path, sizeof path) ||
		!Xwau_FileHasText(path, "WeaponSwitchBasedOnIff"))
		return false;
	XwauLines         lines;
	const char* const source = global_weapon_lines(game, &lines);
	*on                      = int_key(&lines, source, "WeaponSwitchBasedOnIff", 0) != 0;
	XwauLines_Free(&lines);
	return true;
}

bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count) {
	XwauLines lines = { 0 };
	*values         = NULL;
	*count          = 0;
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines)) {
		Report(path, "out of memory");
		return false;
	}
	uint16_t* list = (uint16_t*)calloc(lines.count ? lines.count : 1, sizeof *list);
	bool      ok   = list != NULL;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		long long number;
		/* The hook reads each line with std::stoi, base 10. */
		ok = Xwau_Stoi(lines.items[i], &number);
		if (!ok)
			Report(path, "line '%s' isn't a number", lines.items[i]);
		else
			list[i] = (uint16_t)number;
	}
	if (ok) {
		*values = list;
		*count  = lines.count;
	} else {
		free(list);
	}
	XwauLines_Free(&lines);
	return ok;
}

static bool has_reticle(const XwauCraftHud* hud) {
	for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
		if (hud->has_reticle[slot])
			return true;
	}
	return false;
}

static bool has_colors(const XwauCraftHud* hud) { return hud->has_inner || hud->has_border; }

static bool has_glow_limit(const XwauCraftEngineGlows* glows, int glow) {
	return glows->min_percent[glow] > 0 || glows->min_mglt[glow] > 0;
}

static bool has_engine_glows(const XwauCraftEngineGlows* glows) {
	for (int glow = 0; glow < XWAU_CRAFT_ENGINE_GLOWS; ++glow) {
		if (has_glow_limit(glows, glow))
			return true;
	}
	return false;
}

static void write_hud(FILE* f, const XwauCraftHud* hud) {
	fprintf(f, "hud:\n");
	bool first = true;
	for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
		if (!hud->has_reticle[slot])
			continue;
		fprintf(f, "%s%d: %d", first ? "  reticle: {" : ", ", slot, hud->reticle[slot]);
		first = false;
	}
	if (!first)
		fprintf(f, "}\n");
	if (hud->has_inner || hud->has_border) {
		fprintf(f, "  colors: {");
		if (hud->has_inner)
			fprintf(f, "inner: 0x%08x", hud->inner);
		if (hud->has_border)
			fprintf(f, "%sborder: 0x%08x", hud->has_inner ? ", " : "", hud->border);
		fprintf(f, "}\n");
	}
}

static void write_engine_glows(FILE* f, const XwauCraftEngineGlows* glows) {
	fprintf(f, "engine_glows:\n");
	for (int glow = 0; glow < XWAU_CRAFT_ENGINE_GLOWS; ++glow) {
		if (!has_glow_limit(glows, glow))
			continue;
		fprintf(f, "  %d: {", glow);
		if (glows->min_percent[glow] > 0)
			fprintf(f, "min_speed_percent: %d", glows->min_percent[glow]);
		if (glows->min_mglt[glow] > 0)
			fprintf(f, "%smin_speed_mglt: %d", glows->min_percent[glow] > 0 ? ", " : "",
					glows->min_mglt[glow]);
		fprintf(f, "}\n");
	}
}

static bool has_weapon_colors(const XwauCraftWeaponColors* colors) {
	if (colors->has_default)
		return true;
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (colors->has_variant[weapon] || colors->has_impact[weapon])
			return true;
	}
	for (int bar = 0; bar < XWAU_CRAFT_BARS; ++bar) {
		if (colors->has_bar[bar])
			return true;
	}
	return false;
}

static bool has_weapons(const XwauCraftWeapons* weapons) {
	if (weapons->switch_by_iff >= 0 || has_weapon_colors(&weapons->colors) ||
		XwauCraft_HasWeaponRates(&weapons->rates) || XwauWeaponStats_Any(&weapons->stats))
		return true;
	for (int markings = 0; markings < 256; ++markings) {
		if (weapons->has_markings[markings])
			return true;
	}
	return false;
}

static const char* const g_barNames[XWAU_CRAFT_BARS] = { "laser_high", "laser_low", "ion_high", "ion_low" };

/* Starts the flow map `name` before its first entry, or separates two
 * entries. */
static void map_entry(FILE* f, const char* indent, const char* name, bool* open) {
	if (*open)
		fputs(", ", f);
	else
		fprintf(f, "%s%s: {", indent, name);
	*open = true;
}

static void map_end(FILE* f, bool open) {
	if (open)
		fputs("}\n", f);
}

/* Writes the keys of `colors` at `indent`. */
static void write_weapon_colors(FILE* f, const char* indent, const XwauCraftWeaponColors* colors) {
	bool open = false;
	if (colors->has_default) {
		map_entry(f, indent, "variant", &open);
		fprintf(f, "default: %u", colors->default_variant);
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (colors->has_variant[weapon]) {
			map_entry(f, indent, "variant", &open);
			fprintf(f, "%d: %u", XWAU_CRAFT_FIRST_WEAPON + weapon, colors->variant[weapon]);
		}
	}
	map_end(f, open);
	open = false;
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (colors->has_impact[weapon]) {
			map_entry(f, indent, "impact", &open);
			fprintf(f, "%d: 0x%06x", XWAU_CRAFT_FIRST_WEAPON + weapon, colors->impact[weapon]);
		}
	}
	map_end(f, open);
	open = false;
	for (int bar = 0; bar < XWAU_CRAFT_BARS; ++bar) {
		if (colors->has_bar[bar]) {
			map_entry(f, indent, "energy_bars", &open);
			fprintf(f, "%s: 0x%08x", g_barNames[bar], colors->bar[bar]);
		}
	}
	map_end(f, open);
}

static void write_weapons(FILE* f, const XwauCraftWeapons* weapons) {
	fprintf(f, "weapons:\n");
	if (weapons->switch_by_iff >= 0)
		fprintf(f, "  switch_by_iff: %s\n", weapons->switch_by_iff ? "true" : "false");
	XwauCraft_WriteWeaponRates(f, "  ", &weapons->rates);
	XwauWeaponStats_Write(f, "  ", &weapons->stats);
	if (has_weapon_colors(&weapons->colors)) {
		fprintf(f, "  colors:\n");
		write_weapon_colors(f, "    ", &weapons->colors);
	}
	bool first = true;
	for (int markings = 0; markings < 256; ++markings) {
		if (!weapons->has_markings[markings])
			continue;
		if (first)
			fprintf(f, "  by_markings:\n");
		first = false;
		fprintf(f, "    %d:\n", markings);
		write_weapon_colors(f, "      ", &weapons->by_markings[markings]);
	}
}

static bool write_craft_file(const char* path, const char* sources, const XwauCraftHud* hud,
							 const XwauCraftEngineGlows* glows, const XwauCraftWeapons* weapons,
							 const XwauStatsProfiles* stats, const XwauCraftShield* shield,
							 const XwauCraftSound* sound, const XwauCraftComponents* components,
							 const XwauGunnerSeats* gunner, XwauLines* cockpits) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Written by xwau_import craft from %s.\nversion: 1\n", sources);
	if (has_reticle(hud) || has_colors(hud))
		write_hud(f, hud);
	if (has_engine_glows(glows))
		write_engine_glows(f, glows);
	if (has_weapons(weapons))
		write_weapons(f, weapons);
	XwauStatsProfiles_Write(f, stats);
	if (shield->has)
		write_shield(f, shield);
	if (XwauCraft_HasSound(sound))
		write_sound(f, sound);
	if (components->has)
		write_components(f, components);
	bool ok = XwauGunnerSeats_Write(f, gunner, cockpits);
	if (!ok)
		Report(NULL, "out of memory");
	ok = fclose(f) == 0 && ok;
	if (!ok)
		Report(path, "can't be written");
	return ok;
}

static void write_list(FILE* f, const char* key, const uint16_t* values, size_t count) {
	fprintf(f, "%s: [", key);
	for (size_t i = 0; i < count; ++i)
		fprintf(f, "%s%s%u", i ? "," : "", i % 16 ? " " : "\n  ", values[i]);
	fprintf(f, "]\n");
}

static bool write_icon_lists(const char* xwau_root, const char* output_dir) {
	char       hull_path[1024], map_path[1024], path[1024];
	const bool has_hull = Xwau_FindFile(xwau_root, "HullIconList.txt", hull_path, sizeof hull_path);
	const bool has_map  = Xwau_FindFile(xwau_root, "MapIconList.txt", map_path, sizeof map_path);
	if (!has_hull && !has_map)
		return true;
	uint16_t* hull       = NULL;
	uint16_t* map        = NULL;
	size_t    hull_count = 0, map_count = 0;
	bool      ok = (!has_hull || XwauCraft_ReadIconList(hull_path, &hull, &hull_count)) &&
				   (!has_map || XwauCraft_ReadIconList(map_path, &map, &map_count)) &&
				   PathFormat(path, sizeof path, "%s/hud.yaml", output_dir) && MakeDirs(output_dir);
	FILE*     f  = ok ? fopen(path, "wb") : NULL;
	if (ok && !f) {
		Report(path, "can't be written");
		ok = false;
	}
	if (f) {
		fprintf(f,
				"# Written by xwau_import craft from %s%s%s.\n"
				"# Each list gives the image id for each object type.\nversion: 1\n",
				has_hull ? "HullIconList.txt" : "", has_hull && has_map ? " and " : "",
				has_map ? "MapIconList.txt" : "");
		if (has_hull)
			write_list(f, "hull_icons", hull, hull_count);
		if (has_map)
			write_list(f, "map_icons", map, map_count);
		ok = fclose(f) == 0;
		if (!ok)
			Report(path, "can't be written");
		else
			Report(NULL, "hud.yaml: %zu hull icons, %zu map icons", hull_count, map_count);
	}
	free(hull);
	free(map);
	return ok;
}

/* Whether `name` is "<model><suffix>" for a model that has no INI file, in
 * which case `model` gets the model's name. */
static bool side_file_without_ini(char* const* names, size_t count, const char* name, const char* suffix,
								  char* model, size_t cap) {
	const size_t length        = strlen(name);
	const size_t suffix_length = strlen(suffix);
	if (length <= suffix_length || !EndsWithNoCase(name, suffix) || length - suffix_length >= cap)
		return false;
	memcpy(model, name, length - suffix_length);
	model[length - suffix_length] = '\0';
	for (size_t i = 0; i < count; ++i) {
		const size_t other = strlen(names[i]);
		if (other == length - suffix_length + 4 && EndsWithNoCase(names[i], ".ini") &&
			strncasecmp(names[i], model, length - suffix_length) == 0)
			return false;
	}
	return true;
}

/* Whether the folder has "<model><suffix>". */
static bool has_side_file(char* const* names, size_t count, const char* model, const char* suffix) {
	const size_t length = strlen(model);
	for (size_t i = 0; i < count; ++i) {
		if (strncasecmp(names[i], model, length) == 0 && Xwau_SameName(names[i] + length, suffix))
			return true;
	}
	return false;
}

/* The files that can give a model's settings beside its INI file, as
 * <model><suffix>. */
static const char* const g_sideFiles[] = { "Reticle.txt",
										   "EngineGlows.txt",
										   "WeaponColor.txt",
										   "Shield.txt",
										   "WeaponRate.txt",
										   "Sound.txt",
										   "SubcomponentsStrength.txt",
										   "Turrets.txt",
										   "WeaponStats.txt",
										   "WarheadStats.txt",
										   "EnergyStats.txt",
										   "LinkingStats.txt",
										   "WeaponHardpointTypes.txt" };

enum {
	SIDE_RETICLE,
	SIDE_ENGINE_GLOWS,
	SIDE_WEAPON_COLOR,
	SIDE_SHIELD,
	SIDE_WEAPON_RATE,
	SIDE_SOUND,
	SIDE_SUBCOMPONENTS,
	SIDE_TURRETS,
	SIDE_WEAPON_STATS,
	SIDE_WARHEAD_STATS,
	SIDE_ENERGY_STATS,
	SIDE_LINKING_STATS,
	SIDE_HARDPOINT_TYPES,
	SIDE_FILES
};

/* Whether `name` is the first side file of a model that has no INI file, in
 * which case `model` gets the model's name. A model with several side files
 * is then read once. */
static bool first_side_file_without_ini(char* const* names, size_t count, const char* name, char* model,
										size_t cap) {
	for (int side = 0; side < SIDE_FILES; ++side) {
		if (!side_file_without_ini(names, count, name, g_sideFiles[side], model, cap))
			continue;
		for (int earlier = 0; earlier < side; ++earlier) {
			if (has_side_file(names, count, model, g_sideFiles[earlier]))
				return false;
		}
		return true;
	}
	return false;
}

/* Appends `name` to the list of source files. */
static void add_source(char* sources, size_t cap, const char* name) {
	const size_t length = strlen(sources);
	snprintf(sources + length, cap - length, "%s%s", length ? " and " : "", name);
}

bool XwauCraft_Write(const char* xwau_root, const char* output_dir, XwauLines* cockpits) {
	char models_dir[1024], craft_dir[1024];
	if (!Xwau_FindDir(xwau_root, "FlightModels", models_dir, sizeof models_dir)) {
		Report(xwau_root, "has no FlightModels folder");
		return false;
	}
	if (!PathFormat(craft_dir, sizeof craft_dir, "%s/models", output_dir))
		return false;
	size_t count   = 0;
	char** names   = ListDir(models_dir, true, &count);
	bool   ok      = true;
	int    written = 0, reticles = 0, colors = 0, engine_glows = 0, weapon_colors = 0, light_colors = 0;
	int    stats_profiles = 0, shields = 0, weapon_rates = 0, sounds = 0, engine_slowing = 0, components = 0;
	int    weapon_profiles = 0;
	/* hook_opt_limit reads the [SubcomponentsStrength] sections. */
	char       opt_limit_dll[1024];
	const bool strength =
		Xwau_FindFile(xwau_root, "hook_opt_limit.dll", opt_limit_dll, sizeof opt_limit_dll) &&
		Xwau_FileHasText(opt_limit_dll, "SubcomponentsStrength");
	/* hook_engine_sound reads the [Sound] sections. */
	char       engine_sound_dll[1024];
	const bool engine_sound =
		Xwau_FindFile(xwau_root, "hook_engine_sound.dll", engine_sound_dll, sizeof engine_sound_dll);
	const int range_bank = XwauSoundBanks_LineCount(xwau_root, "WeaponRange");
	/* If the global rates have lines, hook_weapon_rate ignores the models'
	   rates. */
	XwauCraftWeaponRates global_rates;
	const bool           global_rate_lines = XwauCraft_ReadGlobalWeaponRates(xwau_root, &global_rates);
	size_t disabled_profiles = 0;
	/* The genus of the object types that load each model, for the units of
	   the stats profiles. */
	XwauGameTables* tables = XwauGameTables_Load(xwau_root);
	XwauStatsGenus  genus;
	ok = XwauStatsGenus_Read(xwau_root, tables, &genus);
	/* hook_mission_objects reads the [Turrets] sections. */
	char              mission_objects_dll[1024];
	const bool        turrets      = Xwau_FindFile(xwau_root, "hook_mission_objects.dll", mission_objects_dll,
												   sizeof mission_objects_dll) &&
									 Xwau_FileHasText(mission_objects_dll, "Turrets.txt");
	int               gunner_seats = 0;
	XwauCraftWeapons* weapons = (XwauCraftWeapons*)malloc(sizeof *weapons);
	if (!weapons) {
		XwauStatsGenus_Free(&genus);
		XwauGameTables_Free(tables);
		FreeList(names, count);
		Report(NULL, "out of memory");
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		char model[256], ini[1100], side_names[SIDE_FILES][300], side_paths[SIDE_FILES][1100], key[256],
			path[1100], sources[1400];
		bool         has_side[SIDE_FILES];
		const size_t length  = strlen(names[i]);
		bool         has_ini = false;
		if (EndsWithNoCase(names[i], ".ini") && length > 4 && length - 4 < sizeof model) {
			memcpy(model, names[i], length - 4);
			model[length - 4] = '\0';
			has_ini           = true;
		} else if (!first_side_file_without_ini(names, count, names[i], model, sizeof model)) {
			continue;
		}
		snprintf(ini, sizeof ini, "%s/%s.ini", models_dir, model);
		for (int side = 0; side < SIDE_FILES; ++side) {
			snprintf(side_names[side], sizeof side_names[side], "FlightModels/%s%s", model,
					 g_sideFiles[side]);
			has_side[side] =
				Xwau_FindFile(xwau_root, side_names[side], side_paths[side], sizeof side_paths[side]);
		}
		const char* side_path[SIDE_FILES];
		for (int side = 0; side < SIDE_FILES; ++side)
			side_path[side] = has_side[side] ? side_paths[side] : NULL;

		XwauCraftHud         hud;
		XwauCraftEngineGlows glows;
		memset(&hud, 0, sizeof hud);
		memset(&glows, 0, sizeof glows);
		memset(weapons, 0, sizeof *weapons);
		XwauCraft_ReadReticle(side_path[SIDE_RETICLE], has_ini ? ini : NULL, &hud);
		if (has_ini)
			XwauCraft_ReadHudColors(ini, &hud);
		XwauCraft_ReadEngineGlows(side_path[SIDE_ENGINE_GLOWS], has_ini ? ini : NULL, &glows);
		XwauCraft_ReadWeapons(side_path[SIDE_WEAPON_COLOR], has_ini ? ini : NULL, weapons);
		/* The sections of default.ini hold the global rates and weapon
		   profiles. */
		if (strcasecmp(model, "default") != 0) {
			XwauCraft_ReadWeaponRates(side_path[SIDE_WEAPON_RATE], has_ini ? ini : NULL, &weapons->rates);
			if (!XwauWeaponStats_Read(xwau_root, model, &weapons->stats)) {
				Report(NULL, "out of memory");
				ok = false;
				break;
			}
		} else {
			memset(weapons->rates.value, 0xff, sizeof weapons->rates.value);
		}
		light_colors += weapons->light_colors;
		XwauCraftShield shield;
		XwauCraft_ReadShield(side_path[SIDE_SHIELD], has_ini ? ini : NULL, &shield);
		XwauCraftSound sound;
		XwauCraft_ReadSound(engine_sound ? side_path[SIDE_SOUND] : NULL, engine_sound && has_ini ? ini : NULL,
							range_bank, &sound);
		engine_slowing += sound.engine_slowing;
		XwauCraftComponents strengths;
		XwauCraft_ReadComponents(strength ? side_path[SIDE_SUBCOMPONENTS] : NULL,
								 strength && has_ini ? ini : NULL, &strengths);
		XwauGunnerSeats gunner;
		memset(&gunner, 0, sizeof gunner);
		if (turrets)
			XwauGunnerSeats_Read(xwau_root, tables, model, side_path[SIDE_TURRETS], has_ini ? ini : NULL,
								 &gunner);
		XwauStatsProfiles stats;
		LowerCopy(key, sizeof key, model);
		if (!XwauStatsProfiles_Read(xwau_root, model, names, count, XwauStatsGenus_Find(&genus, key),
									&stats)) {
			XwauWeaponStats_Free(&weapons->stats);
			ok = false;
			break;
		}
		disabled_profiles += stats.disabled;
		if (!has_reticle(&hud) && !has_colors(&hud) && !has_engine_glows(&glows) && !has_weapons(weapons) &&
			!stats.count && !shield.has && !XwauCraft_HasSound(&sound) && !strengths.has && !gunner.has) {
			XwauStatsProfiles_Free(&stats);
			XwauWeaponStats_Free(&weapons->stats);
			continue;
		}

		sources[0] = '\0';
		for (int side = 0; side < SIDE_FILES; ++side) {
			if (has_side[side])
				add_source(sources, sizeof sources, side_names[side]);
		}
		if (has_ini) {
			char ini_name[300];
			snprintf(ini_name, sizeof ini_name, "FlightModels/%s.ini", model);
			add_source(sources, sizeof sources, ini_name);
		}
		if (!PathFormat(path, sizeof path, "%s/%s.yaml", craft_dir, key) || !MakeDirs(craft_dir) ||
			!write_craft_file(path, sources, &hud, &glows, weapons, &stats, &shield, &sound, &strengths,
							  &gunner, cockpits)) {
			XwauStatsProfiles_Free(&stats);
			XwauWeaponStats_Free(&weapons->stats);
			ok = false;
			continue;
		}
		weapon_profiles += XwauWeaponStats_Any(&weapons->stats);
		XwauWeaponStats_Free(&weapons->stats);
		stats_profiles += stats.count != 0;
		shields += shield.has;
		sounds += XwauCraft_HasSound(&sound);
		components += strengths.has;
		gunner_seats += gunner.has;
		XwauStatsProfiles_Free(&stats);
		written++;
		reticles += has_reticle(&hud);
		colors += has_colors(&hud);
		engine_glows += has_engine_glows(&glows);
		weapon_colors += has_weapons(weapons);
		weapon_rates += XwauCraft_HasDefaultFirstRates(&weapons->rates);
	}
	free(weapons);
	XwauStatsGenus_Free(&genus);
	XwauGameTables_Free(tables);
	FreeList(names, count);
	Report(
		NULL,
		"model settings: %d files, %d with a reticle, %d with HUD colours, %d with engine glows, %d with "
		"weapon settings, %d with weapon profiles, %d with stats profiles, %d with a shield recharge rate, "
		"%d with sounds, %d with component strengths, %d with gunner seats",
		written, reticles, colors, engine_glows, weapon_colors, weapon_profiles, stats_profiles, shields,
		sounds, components, gunner_seats);
	if (engine_slowing)
		Report(NULL, "%d models set EngineSlowing in [Sound], which no hook reads", engine_slowing);
	if (engine_sound) {
		/* The default ambient distance of hook_engine_sound. */
		XwauLines global = { 0 };
		char      global_path[1024], value[256];
		if (Xwau_FindFile(xwau_root, "FlightModels/Sound.txt", global_path, sizeof global_path))
			XwauLines_Read(global_path, "", XWAU_RULES_CPP, &global);
		if (!global.count &&
			Xwau_FindFile(xwau_root, "FlightModels/default.ini", global_path, sizeof global_path))
			XwauLines_Read(global_path, "Sound", XWAU_RULES_CPP, &global);
		XwauLines_Value(&global, "AmbientSoundDistance", XWAU_RULES_CPP, value, sizeof value);
		if (value[0])
			Report(
				NULL,
				"the global AmbientSoundDistance = %s isn't imported: a starship's ambient sound reaches its "
				"size, or its model's distance",
				value);
		XwauLines_Free(&global);
	}
	if (disabled_profiles)
		Report(NULL,
			   "%zu sections such as [StatsProfile_Default;] or [;StatsProfile_Default], which XWAU never "
			   "reads, are left out",
			   disabled_profiles);
	if (weapon_rates && global_rate_lines)
		Report(NULL,
			   "%d models have [WeaponRate] values, which XWAU ignores because the global [WeaponRate] has "
			   "lines; OpenXWA uses them",
			   weapon_rates);
	if (light_colors)
		Report(NULL,
			   "%d models set WeaponLightColor, which isn't imported: a bolt's light comes from its model",
			   light_colors);
	return write_icon_lists(xwau_root, output_dir) && ok;
}
