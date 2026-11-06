#include "xwau_craft.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aeron/asset/flight_names.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_gunner_seats.h"
#include "xwau_mat.h"
#include "xwau_model.h"
#include "xwau_objects.h"
#include "xwau_sound_banks.h"
#include "xwau_stats_profiles.h"

/* The slots of XWAU's reticle hook (g_imageIndices in reticle.cpp). */
static const int g_reticleSlots[] = { 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20, 21, 22 };

uint32_t XwauCraft_HudColor(float r, float g, float b) {
	return 0xffu << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

void XwauCraft_ReadReticle(const char* reticle_txt, const char* ini, XwauCraftHud* hud) {
	XwauLines lines = { 0 };
	if (reticle_txt)
		XwauLines_Read(reticle_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
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
		if (!Xwau_StoiAuto(value, &number))
			continue;
		if (number == -1)
			continue;
		hud->has_reticle[slot] = true;
		hud->reticle[slot]     = (int)number;
	}
	/* The hook only looks up the keys of its slots, so other lines have no
	   effect. */
	XwauLines_Free(&lines);
}

/* Reads "<key> = r, g, b" as LoadGeneric3DCoords does: three numbers after
 * the '='. Like the wrapper, skips a line with fewer numbers. */
static bool read_hud_color(const char* ini, const char* line, const char* key, uint32_t* out) {
	const char* equals = strchr(line, '=');
	float       rgb[3];
	if (!equals || sscanf(equals + 1, "%f, %f, %f", &rgb[0], &rgb[1], &rgb[2]) < 3)
		return false;
	/* uint32_t(x * 255) has no meaning for a negative value. */
	if (rgb[0] < 0 || rgb[1] < 0 || rgb[2] < 0) {
		ReportFatal(ini, "%s has a negative value, which gives XWAU a colour that the importer can't compute",
					key);
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
		/* ddraw doesn't read other keys. */
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

/* Reads one line of engine glows. Returns false when a value isn't a number
 * that fits in an int, or when the glow is outside 0 to 255. */
static bool read_engine_glow(const char* source, const char* line, XwauCraftEngineGlows* glows) {
	XwauLines tokens = { 0 };
	long long values[3];
	bool      ok = true;
	if (!Xwau_Tokenize(line, XWAU_RULES_CPP, &tokens)) {
		Report(source, "out of memory");
		return false;
	}
	/* The hook skips a line with fewer than 3 values. */
	if (tokens.count < 3) {
		XwauLines_Free(&tokens);
		return true;
	}
	for (int i = 0; ok && i < 3; ++i)
		ok = Xwau_StoiAuto(tokens.items[i], &values[i]) && values[i] >= INT_MIN && values[i] <= INT_MAX;
	XwauLines_Free(&tokens);
	/* The hook reads the values with std::stoi(value, 0, 0), which throws. */
	if (!ok)
		return false;
	/* No model has a glow outside 0 to 255. */
	if (values[0] < 0 || values[0] >= XWAU_CRAFT_ENGINE_GLOWS)
		return false;
	const int glow = (int)values[0];
	/* The hook uses the first line of a glow. */
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
 * the key is missing, or when its value isn't a number, which XWAU fails
 * on. */
static int int_key(const XwauLines* lines, const char* key, int fallback) {
	char      value[256];
	long long number;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return fallback;
	if (!Xwau_StoiAuto(value, &number) || number < INT_MIN || number > INT_MAX)
		return fallback;
	return (int)number;
}

/* Reads a markings key. Returns -1 when it isn't set. */
static int markings_value(const XwauLines* lines, const char* key) { return int_key(lines, key, -1); }

/* Reads a colour key as GetFileKeyValueUnsignedInt does. Returns false when
 * the key is missing, or when its value isn't a hexadecimal number, which
 * XWAU fails on. */
static bool color_value(const XwauLines* lines, const char* key, uint32_t* out) {
	char value[256];
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return false;
	return Xwau_StoulHex(value, out);
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
static bool read_weapon_colors(const XwauLines* lines, int markings, XwauCraftWeaponColors* colors) {
	char     prefix[64], key[96];
	bool     any = false;
	uint32_t color;
	markings_key(prefix, sizeof prefix, "WeaponColor", markings);
	int variant = markings_value(lines, prefix);
	if (variant != -1) {
		colors->has_default     = true;
		colors->default_variant = (uint8_t)variant;
		any                     = true;
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		snprintf(key, sizeof key, "%s%d", prefix, XWAU_CRAFT_FIRST_WEAPON + weapon);
		variant = markings_value(lines, key);
		if (variant != -1) {
			colors->has_variant[weapon] = true;
			colors->variant[weapon]     = (uint8_t)variant;
			any                         = true;
		}
	}
	for (int group = 0; group < 5; ++group) {
		markings_key(key, sizeof key, g_impactGroupKeys[group], markings);
		/* Without _fg, a group key replaces the global colour, even with 0. */
		if (color_value(lines, key, &color) && (markings < 0 || color != 0)) {
			set_impact_group(colors, group, color);
			any = true;
		}
	}
	markings_key(prefix, sizeof prefix, "WeaponImpactColor", markings);
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		snprintf(key, sizeof key, "%s%d", prefix, XWAU_CRAFT_FIRST_WEAPON + weapon);
		if (color_value(lines, key, &color) && color != 0) {
			colors->has_impact[weapon] = true;
			colors->impact[weapon]     = XwauCraft_ImpactColor(color);
			any                        = true;
		}
	}
	for (int bar = 0; bar < XWAU_CRAFT_BARS; ++bar) {
		markings_key(key, sizeof key, g_barKeys[bar], markings);
		if (color_value(lines, key, &color) && color != 0) {
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
	XwauLines lines = { 0 };
	if (weapons_txt)
		XwauLines_Read(weapons_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		if (ini)
			XwauLines_Read(ini, "WeaponColor", XWAU_RULES_CPP, &lines);
	}
	read_weapon_colors(&lines, -1, &weapons->colors);
	const int switch_by_iff = int_key(&lines, "WeaponSwitchBasedOnIff", -1);
	weapons->switch_by_iff  = switch_by_iff == -1 ? -1 : switch_by_iff != 0;
	/* The hook doesn't read lines with other keys. */
	for (size_t i = 0; i < lines.count; ++i) {
		const int markings = line_markings(lines.items[i]);
		if (markings >= 0 && !weapons->has_markings[markings])
			weapons->has_markings[markings] =
				read_weapon_colors(&lines, markings, &weapons->by_markings[markings]);
	}
	XwauLines_Free(&lines);
}

/* Reads the global settings of hook_weapon_color: its hook_weapon_color.cfg,
 * else the [hook_weapon_color] section of Hooks.ini. */
static void global_weapon_lines(const char* game, XwauLines* lines) {
	char path[1024];
	memset(lines, 0, sizeof *lines);
	if (Xwau_FindFile(game, "hook_weapon_color.cfg", path, sizeof path))
		XwauLines_Read(path, "", XWAU_RULES_CPP, lines);
	if (lines->count == 0) {
		XwauLines_Free(lines);
		if (Xwau_FindFile(game, "Hooks.ini", path, sizeof path))
			XwauLines_Read(path, "hook_weapon_color", XWAU_RULES_CPP, lines);
	}
}

void XwauCraft_ReadShield(const char* shield_txt, const char* ini, XwauCraftShield* shield) {
	XwauLines lines = { 0 };
	memset(shield, 0, sizeof *shield);
	if (shield_txt)
		XwauLines_Read(shield_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		if (ini)
			XwauLines_Read(ini, "Shield", XWAU_RULES_CPP, &lines);
	}
	if (lines.count) {
		shield->has           = true;
		shield->per_generator = int_key(&lines, "UseGenerator", 0) == 1;
		shield->rate =
			int_key(&lines, shield->per_generator ? "PerGeneratorRechargeRate" : "RechargeRate", 0);
		shield->player_rate   = int_key(&lines, "PlayerRechargeRate", 0);
		shield->delay_seconds = int_key(&lines, "RechargeRateDelay", 0);
	}
	XwauLines_Free(&lines);
}

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
		/* A line that isn't a number would crash XWAU, so the section is
		   left out. */
		if (!Xwau_Stoi(lines.items[i], &value)) {
			memset(components->strength, 0xff, sizeof components->strength);
			break;
		}
		/* The hook doesn't use the lines past the mesh types. */
		if (i >= XWAU_CRAFT_MESH_TYPES)
			continue;
		if (value == -1)
			continue;
		if (value < -1) {
			/* -1 keeps the game's strength. The hook may use other negative
			   values, and the game can't hold them. */
			ReportFatal(source, "[SubcomponentsStrength]: %s = %lld is a negative strength other than -1",
						Aeron_FlightName(AERON_FLIGHT_MESH_TYPES, (uint32_t)i), value);
			continue;
		}
		if (value > 255) {
			ReportFatal(source, "[SubcomponentsStrength]: %s = %lld doesn't fit in a component's health",
						Aeron_FlightName(AERON_FLIGHT_MESH_TYPES, (uint32_t)i), value);
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
		fprintf(f, "%s%s: %d", first ? "" : ", ", Aeron_FlightName(AERON_FLIGHT_MESH_TYPES, (uint32_t)type),
				components->strength[type]);
		first = false;
	}
	fputs("}\n", f);
}

/* The keys of [WarheadLock], their limits, and where each one goes in the
 * model file. The keys of a map are distances, which the model file has in
 * metres. */
static const struct {
	const char* key;
	long long   min, max;
	const char* group; /* NULL for a key outside a map */
	const char* name;
} g_warheadLockKeys[XWAU_CRAFT_WARHEAD_LOCK_KEYS] = {
	{ "LockDistance0", 0, INT_MAX, "lock_range_m", "small" },
	{ "LockDistance1", 0, INT_MAX, "lock_range_m", "big" },
	{ "LockDistance2", 0, INT_MAX, "lock_range_m", "transport" },
	/* The HUD divides by LockTime / 200, and the lock ticks are 16-bit. */
	{ "LockTime", 200, 32767, NULL, "lock_time_ms" },
	/* The game divides by it. */
	{ "LockStrengthDivisor", 1, 32767, NULL, "homing_level_time_ms" },
	{ "AILockDistance0", 0, INT_MAX, "ai_lock_range_m", "small" },
	{ "AILockDistance1", 0, INT_MAX, "ai_lock_range_m", "big" },
	{ "AILockDistance2", 0, INT_MAX, "ai_lock_range_m", "big_top_tier" },
	{ "AILockTimeFactor", 0, 65535, NULL, "ai_lock_time_per_tier_ms" },
};

enum { WARHEAD_LOCK_TIME = 3 };

/* The game reads at most this many sounds in a weapon range list. */
enum { WEAPON_RANGE_SOUND_MAX = 16 };

void XwauCraft_ReadWarheadLock(const char* lock_txt, const char* ini, XwauCraftWarheadLock* lock) {
	XwauLines   lines  = { 0 };
	const char* source = lock_txt;
	memset(lock, 0, sizeof *lock);
	memset(lock->value, 0xff, sizeof lock->value);
	if (lock_txt)
		XwauLines_Read(lock_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "WarheadLock", XWAU_RULES_CPP, &lines);
	}
	lock->has = lines.count != 0;
	for (int i = 0; i < XWAU_CRAFT_WARHEAD_LOCK_KEYS; ++i) {
		char      value[256];
		long long number;
		XwauLines_Value(&lines, g_warheadLockKeys[i].key, XWAU_RULES_CPP, value, sizeof value);
		if (!value[0])
			continue;
		/* A value that isn't a number would crash XWAU. */
		if (!Xwau_StoiAuto(value, &number))
			continue;
		/* -1 keeps the game's value. */
		if (number == -1)
			continue;
		/* The HUD divides by LockTime / 200, and the game by
		   LockStrengthDivisor, so XWAU crashes when they are 0. */
		const bool crash =
			(i == WARHEAD_LOCK_TIME && number / 200 == 0) || (i == WARHEAD_LOCK_TIME + 1 && number == 0);
		if (crash)
			continue;
		if (number < g_warheadLockKeys[i].min || number > g_warheadLockKeys[i].max) {
			ReportFatal(source, "[WarheadLock]: %s = %lld isn't from %lld to %lld", g_warheadLockKeys[i].key,
						number, g_warheadLockKeys[i].min, g_warheadLockKeys[i].max);
			continue;
		}
		lock->value[i] = (int)number;
	}
	if (lock->has && lock->value[WARHEAD_LOCK_TIME] < 0)
		lock->value[WARHEAD_LOCK_TIME] = 708;
	/* The hook doesn't read lines without '=' or with other keys. */
	for (size_t i = 0; i < lines.count; ++i) {
		const char* equals = strchr(lines.items[i], '=');
		char        key[256];
		int         known = 0, profile = 0;
		if (!equals)
			continue;
		snprintf(key, sizeof key, "%.*s", (int)(equals - lines.items[i]), lines.items[i]);
		Xwau_Trim(key, XWAU_RULES_CPP);
		for (int k = 0; k < XWAU_CRAFT_WARHEAD_LOCK_KEYS && !known; ++k) {
			const size_t length = strlen(g_warheadLockKeys[k].key);
			known               = Xwau_SameName(key, g_warheadLockKeys[k].key);
			/* A key for an object profile is <key>_<profile>. */
			profile = profile || (strncasecmp(key, g_warheadLockKeys[k].key, length) == 0 &&
								  key[length] == '_' && key[length + 1]);
		}
		if (!known && profile)
			ReportUnsupported("[WarheadLock] keys for object profiles");
	}
	XwauLines_Free(&lines);
}

/* Writes the keys of `group` that are set, as one map. */
static void write_warhead_lock_map(FILE* f, const XwauCraftWarheadLock* lock, const char* group) {
	bool first = true;
	for (int i = 0; i < XWAU_CRAFT_WARHEAD_LOCK_KEYS; ++i) {
		if (g_warheadLockKeys[i].group != group || lock->value[i] < 0)
			continue;
		if (first)
			fprintf(f, "  %s: {", group);
		fprintf(f, "%s%s: ", first ? "" : ", ", g_warheadLockKeys[i].name);
		Xwau_WriteMetres(f, lock->value[i]);
		first = false;
	}
	if (!first)
		fputs("}\n", f);
}

void XwauCraft_WriteWarheadLock(FILE* f, const XwauCraftWarheadLock* lock) {
	fputs("warhead_lock:\n", f);
	write_warhead_lock_map(f, lock, "lock_range_m");
	for (int i = WARHEAD_LOCK_TIME; i < XWAU_CRAFT_WARHEAD_LOCK_KEYS; ++i) {
		if (!g_warheadLockKeys[i].group && lock->value[i] >= 0) {
			fprintf(f, "  %s: ", g_warheadLockKeys[i].name);
			Xwau_WriteUnit(f, XWAU_UNIT_MILLISECONDS, lock->value[i]);
			fputc('\n', f);
		}
	}
	write_warhead_lock_map(f, lock, "ai_lock_range_m");
}

/* The values of JammingDirection and JammingBehavior, and their names in
 * the model file. */
static const char* const g_jammingDirections[]     = { "All", "Up", "Down" };
static const char* const g_jammingDirectionNames[] = { "all", "up", "down" };
static const char* const g_jammingRules[]          = { "None", "HangarMeshCenter", "InsideHangarHardpoint",
													   "InsideHangarHardpointAndJammingPoints" };
static const char* const g_jammingRuleNames[]      = { "none", "hangar_mesh_center", "hangar_point",
													   "hangar_and_jamming_points" };

/* The index of `value` in `values`, compared as the hook compares it, with
 * its case. Without a match, `fallback`, as in the hook. */
static int jamming_name(const char* value, const char* const* values, int count, int fallback) {
	for (int i = 0; i < count; ++i) {
		if (strcmp(value, values[i]) == 0)
			return i;
	}
	return fallback;
}

void XwauCraft_ReadJamming(const char* jamming_txt, const char* ini, XwauCraftJamming* jamming) {
	XwauLines lines = { 0 };
	char      value[256];
	long long number;
	memset(jamming, 0, sizeof *jamming);
	if (jamming_txt)
		XwauLines_Read(jamming_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		if (ini)
			XwauLines_Read(ini, "Jamming", XWAU_RULES_CPP, &lines);
	}
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		return;
	}
	jamming->has     = true;
	jamming->enabled = true;
	jamming->range   = -1;
	/* A HasJamming or JammingDistance that isn't a number would crash XWAU,
	   so the section is then left out. */
	XwauLines_Value(&lines, "HasJamming", XWAU_RULES_CPP, value, sizeof value);
	if (value[0] && !Xwau_StoiAuto(value, &number)) {
		jamming->has = false;
	} else if (value[0]) {
		jamming->enabled = number != 0;
	}
	XwauLines_Value(&lines, "JammingDirection", XWAU_RULES_CPP, value, sizeof value);
	jamming->direction = jamming_name(value, g_jammingDirections, 3, 0);
	XwauLines_Value(&lines, "JammingDistance", XWAU_RULES_CPP, value, sizeof value);
	if (value[0] && !Xwau_StoiAuto(value, &number)) {
		jamming->has = false;
	} else if (value[0]) {
		jamming->range = (int)number;
	}
	XwauLines_Value(&lines, "JammingBehavior", XWAU_RULES_CPP, value, sizeof value);
	jamming->rule = jamming_name(value, g_jammingRules, 4, 3);
	/* A distance below -1 never jams. */
	if (jamming->has && jamming->enabled && jamming->range < -1)
		jamming->enabled = false;
	XwauLines_Free(&lines);
}

void XwauCraft_WriteJamming(FILE* f, const XwauCraftJamming* jamming) {
	if (!jamming->enabled) {
		fputs("jamming: {enabled: false}\n", f);
		return;
	}
	fprintf(f,
			"jamming: {enabled: true, direction: %s, range_m: ", g_jammingDirectionNames[jamming->direction]);
	if (jamming->range < 0)
		fputs("none", f);
	else
		Xwau_WriteMetres(f, jamming->range);
	fprintf(f, ", rule: %s}\n", g_jammingRuleNames[jamming->rule]);
}

void XwauCraft_ReadDock(const char* dock_txt, const char* ini, XwauCraftDock* dock) {
	XwauLines lines = { 0 };
	char      value[256];
	long long number;
	memset(dock, 0, sizeof *dock);
	if (dock_txt)
		XwauLines_Read(dock_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		if (ini)
			XwauLines_Read(ini, "Dock", XWAU_RULES_CPP, &lines);
	}
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		return;
	}
	dock->has             = true;
	dock->approach_height = 932;
	/* A DockElevation that isn't a number would crash XWAU, so the section
	   is then left out. */
	XwauLines_Value(&lines, "DockElevation", XWAU_RULES_CPP, value, sizeof value);
	if (value[0] && !Xwau_StoiAuto(value, &number)) {
		dock->has = false;
	} else if (value[0]) {
		dock->approach_height = (int)number;
	}
	XwauLines_Free(&lines);
}

void XwauCraft_WriteDock(FILE* f, const XwauCraftDock* dock) {
	fputs("docking: {approach_height_m: ", f);
	Xwau_WriteMetres(f, dock->approach_height);
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
	int value = int_key(&lines, "EngineSoundInterior", 0);
	if (value >= 1 && value <= 7)
		sound->interior = value;
	else if (value)
		ReportFatal(source, "EngineSoundInterior = %d plays nothing in XWAU, and the game can't do that",
					value);
	value = int_key(&lines, "EngineSoundFlyBy", 0);
	if (value % 10 >= 1 && value % 10 <= 8 && value / 10 <= 2 && value > 0) {
		sound->fly_by      = value % 10;
		sound->fly_by_mode = value / 10;
	} else if (value) {
		ReportFatal(source, "EngineSoundFlyBy = %d plays nothing in XWAU, and the game can't do that", value);
	}
	value = int_key(&lines, "EngineSoundWash", 0);
	if (value)
		sound->wash = value == 2 ? 2 : 1;
	value = int_key(&lines, "EngineSoundTakeOff", 0);
	if (value)
		sound->take_off = value == 2 ? 2 : 1;
	value = int_key(&lines, "HyperSoundBehavior", -1);
	if (value >= 0 && value <= 2)
		sound->hyper = value;
	else if (value != -1)
		ReportFatal(source,
					"HyperSoundBehavior = %d plays no start sound in XWAU, and the game can't do that",
					value);
	char text[256];
	XwauLines_Value(&lines, "WeaponSoundBehavior", XWAU_RULES_CPP, text, sizeof text);
	/* With a behaviour that the hook doesn't know, each weapon plays its own
	   sound. */
	if (text[0]) {
		sound->weapons = 0;
		for (int i = 1; i < (int)(sizeof g_behaviorKeys / sizeof g_behaviorKeys[0]); ++i)
			if (strcmp(text, g_behaviorKeys[i]) == 0)
				sound->weapons = i;
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		char start_key[64], count_key[64];
		snprintf(start_key, sizeof start_key, "WeaponSoundRangeStart_%d", XWAU_CRAFT_FIRST_WEAPON + weapon);
		snprintf(count_key, sizeof count_key, "WeaponSoundRangeCount_%d", XWAU_CRAFT_FIRST_WEAPON + weapon);
		const int first = int_key(&lines, start_key, -1);
		const int count = int_key(&lines, count_key, -1);
		/* XWAU ignores a range without both keys at 0 or more, crashes on a
		   count of 0, and drops a range that goes past the WeaponRange
		   bank. */
		if (first < 0 || count < 0 || count == 0 || first + count > range_bank)
			continue;
		if (count > WEAPON_RANGE_SOUND_MAX)
			ReportFatal(source, "%s = %d, but the game reads at most %d sounds in a range", count_key, count,
						WEAPON_RANGE_SOUND_MAX);
		sound->range_first[weapon] = first;
		sound->range_count[weapon] = count > WEAPON_RANGE_SOUND_MAX ? WEAPON_RANGE_SOUND_MAX : count;
	}
	XwauLines_Value(&lines, "EngineSlowing", XWAU_RULES_CPP, text, sizeof text);
	sound->engine_slowing = text[0] != 0;
	static const char* const offset_keys[3]   = { "AmbientSoundOffsetX", "AmbientSoundOffsetY",
												  "AmbientSoundOffsetZ" };
	static const char* const distance_keys[3] = { "AmbientSoundDistanceX", "AmbientSoundDistanceZ",
												  "AmbientSoundDistanceY" };
	for (int axis = 0; axis < 3; ++axis)
		sound->ambient_offset[axis] = int_key(&lines, offset_keys[axis], 0);
	const int  distance     = int_key(&lines, "AmbientSoundDistance", INT_MIN);
	const bool has_distance = distance != INT_MIN;
	for (int axis = 0; axis < 3; ++axis) {
		const int value = int_key(&lines, distance_keys[axis], 0);
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

static void write_sound(FILE* f, const XwauNames* names, const XwauCraftSound* sound) {
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
		/* The sound bank import names the weapon_range entries after their
		   position in the list. */
		fprintf(f, "%s%s: [", open ? ", " : "  weapon_ranges: {",
				XwauNames_WeaponType(names, XWAU_CRAFT_FIRST_WEAPON + weapon));
		for (int i = 0; i < sound->range_count[weapon]; ++i) {
			char name[32];
			XwauSoundBanks_RangeName(sound->range_first[weapon] + i, name, sizeof name);
			fprintf(f, "%s%s", i ? ", " : "", name);
		}
		fputc(']', f);
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
	/* For the system damage keys of weapons, the weapon type whose name is
	   the key. */
	int weapon_type;
} g_rateKeys[XWAU_CRAFT_WEAPON_RATES] = {
	{ "DechargeRate", "rates", "decharge", false, 0 },
	{ "RechargeRate", "rates", "recharge", false, 0 },
	{ "CooldownTimeFactor", "rates", "cooldown_ms", false, 0 },
	{ "DechargeRatePercent", "rates", "decharge_percent", false, 0 },
	{ "RechargeRatePercent", "rates", "recharge_percent", false, 0 },
	{ "EnergyTransferRate", "energy_transfer", "rate", true, 0 },
	{ "EnergyTransferRatePenalty", "energy_transfer", "penalty", true, 0 },
	{ "EnergyTransferWeaponLimit", "energy_transfer", "laser_limit", true, 0 },
	{ "EnergyTransferShieldLimit", "energy_transfer", "shield_limit", true, 0 },
	{ "MaxTorpedoCountPerPass", "ai_torpedoes", "per_pass", false, 0 },
	{ "MaxTorpedoCountPerTarget", "ai_torpedoes", "per_target", false, 0 },
	{ "IsImpactSpinningEnabled", "impact_spin", "enabled", false, 0 },
	{ "ImpactSpinningSpeedFactorPercent", "impact_spin", "speed_percent", false, 0 },
	{ "ImpactSpinningAngleFactorPercent", "impact_spin", "angle_percent", false, 0 },
	{ "MaxSystemDamages", "system_damage", "max", false, 0 },
	{ "Weapon284_LaserIon_Damages", "system_damage", NULL, false, 284 },
	{ "Weapon285_LaserIonTurbo_Damages", "system_damage", NULL, false, 285 },
	{ "Weapon290_LaserIonTurbo_Damages", "system_damage", NULL, false, 290 },
	{ "Weapon296_MagPulse_Damages", "system_damage", NULL, false, 296 },
};

/* Reads the rates of `lines`. */
static void read_weapon_rates(const XwauLines* lines, const char* source, XwauCraftWeaponRates* rates) {
	for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key) {
		rates->value[key] = int_key(lines, g_rateKeys[key].key, -1);
		/* The hook turns the spin on for any value but 0. */
		if (key == XWAU_CRAFT_IMPACT_SPIN && rates->value[key] != -1)
			rates->value[key] = rates->value[key] != 0;
		if (rates->value[key] < -1) {
			ReportFatal(source, "%s = %d is below 0, which the game can't use", g_rateKeys[key].key,
						rates->value[key]);
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

void XwauCraft_WriteWeaponRates(FILE* f, const XwauNames* names, const char* indent,
								const XwauCraftWeaponRates* rates) {
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
		const char* name = g_rateKeys[key].name ? g_rateKeys[key].name
												: XwauNames_WeaponType(names, g_rateKeys[key].weapon_type);
		if (key == XWAU_CRAFT_IMPACT_SPIN)
			fprintf(f, "%s: %s", name, rates->value[key] ? "true" : "false");
		else {
			fprintf(f, "%s: ", name);
			Xwau_WriteUnit(f, key == XWAU_CRAFT_COOLDOWN ? XWAU_UNIT_MILLISECONDS : XWAU_UNIT_NONE,
						   rates->value[key]);
		}
	}
	if (open)
		fputs("}\n", f);
}

bool XwauCraft_ReadGlobalImpactColors(const char* game, uint32_t colors[XWAU_CRAFT_WEAPONS]) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_weapon_color.dll", path, sizeof path))
		return false;
	XwauLines lines;
	uint32_t  groups[5];
	global_weapon_lines(game, &lines);
	for (int group = 0; group < 5; ++group) {
		if (!color_value(&lines, g_impactGroupKeys[group], &groups[group]))
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
	XwauLines lines;
	global_weapon_lines(game, &lines);
	*on = int_key(&lines, "WeaponSwitchBasedOnIff", 0) != 0;
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

/* Writes the `reticle` map. The threat indicators go in nested maps, one
   for each weapon mode. */
static void write_reticle(FILE* f, const XwauNames* names, const XwauCraftHud* hud) {
	static const char* const kThreatModes[] = { "laser_mode", "warhead_mode" };
	bool                     threats        = false;
	fputs("  reticle:\n", f);
	for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
		const char* name = XwauNames_Name(&names->reticle_slots, slot);
		if (hud->has_reticle[slot] && !name)
			ReportUnsupported("reticle images for slots that the game has no name for");
		if (!hud->has_reticle[slot] || !name)
			continue;
		if (strncmp(name, "threats.", 8) == 0)
			threats = true;
		else
			fprintf(f, "    %s: %d\n", name, hud->reticle[slot]);
	}
	if (threats)
		fputs("    threats:\n", f);
	for (int mode = 0; threats && mode < 2; ++mode) {
		char prefix[48];
		bool open = false;
		snprintf(prefix, sizeof prefix, "threats.%s.", kThreatModes[mode]);
		for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
			const char* name = XwauNames_Name(&names->reticle_slots, slot);
			if (!hud->has_reticle[slot] || !name || strncmp(name, prefix, strlen(prefix)) != 0)
				continue;
			if (open)
				fputs(", ", f);
			else
				fprintf(f, "      %s: {", kThreatModes[mode]);
			fprintf(f, "%s: %d", name + strlen(prefix), hud->reticle[slot]);
			open = true;
		}
		if (open)
			fputs("}\n", f);
	}
}

static void write_hud(FILE* f, const XwauNames* names, const XwauCraftHud* hud) {
	fprintf(f, "hud:\n");
	if (has_reticle(hud))
		write_reticle(f, names, hud);
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
static void write_weapon_colors(FILE* f, const XwauNames* names, const char* indent,
								const XwauCraftWeaponColors* colors) {
	bool open = false;
	if (colors->has_default) {
		map_entry(f, indent, "variant", &open);
		fprintf(f, "default: %u", colors->default_variant);
	}
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (colors->has_variant[weapon]) {
			map_entry(f, indent, "variant", &open);
			fprintf(f, "%s: %u", XwauNames_WeaponType(names, XWAU_CRAFT_FIRST_WEAPON + weapon),
					colors->variant[weapon]);
		}
	}
	map_end(f, open);
	open = false;
	for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon) {
		if (colors->has_impact[weapon]) {
			map_entry(f, indent, "impact", &open);
			fprintf(f, "%s: 0x%06x", XwauNames_WeaponType(names, XWAU_CRAFT_FIRST_WEAPON + weapon),
					colors->impact[weapon]);
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

static void write_weapons(FILE* f, const XwauNames* names, const XwauStockValues* stock,
						  const XwauCraftWeapons* weapons) {
	fprintf(f, "weapons:\n");
	if (weapons->switch_by_iff >= 0)
		fprintf(f, "  switch_by_iff: %s\n", weapons->switch_by_iff ? "all" : "none");
	XwauCraft_WriteWeaponRates(f, names, "  ", &weapons->rates);
	XwauWeaponStats_Write(f, names, stock, "  ", &weapons->stats);
	if (has_weapon_colors(&weapons->colors)) {
		fprintf(f, "  colors:\n");
		write_weapon_colors(f, names, "    ", &weapons->colors);
	}
	bool first = true;
	for (int markings = 0; markings < 256; ++markings) {
		if (!weapons->has_markings[markings])
			continue;
		if (first)
			fprintf(f, "  by_markings:\n");
		first = false;
		fprintf(f, "    %d:\n", markings);
		write_weapon_colors(f, names, "      ", &weapons->by_markings[markings]);
	}
}

/* The `surfaces` group: the rules of each surface tag of the model's
   material file (XwauMat_SurfaceRuleAt), from its conditions on the craft
   (EffectsRenderer::MainSceneHook). */
static void write_surfaces(FILE* f, const XwauMatFile* mat) {
	fprintf(f, "surfaces:\n");
	for (size_t i = 0; i < XwauMat_SurfaceRuleCount(mat); ++i) {
		const XwauMatParams* m = XwauMat_SurfaceRuleAt(mat, i);
		/* XWAU hides the surface while throttle / 65535 < DisplayIfThrottleGE
		   / 100. DisplayIfMissionSetSpeedGE also needs a throttle of 0.99. */
		int throttle = m->display_if_throttle_ge > 0.0f ? (int)ceilf(m->display_if_throttle_ge) : 0;
		if (m->display_if_mission_set_speed_ge > 0 && throttle < 99)
			throttle = 99;
		if (throttle > 100)
			throttle = 100;
		const char* separator = "";
		fprintf(f, "  %s: {", m->surface_tag);
		if (m->skip_when_disabled) {
			fprintf(f, "%shidden_when_disabled: true", separator);
			separator = ", ";
		}
		if (m->display_if_speed_ge > 0) {
			fprintf(f, "%smin_speed_mglt: %d", separator, m->display_if_speed_ge);
			separator = ", ";
		}
		if (throttle > 0) {
			fprintf(f, "%smin_throttle_percent: %d", separator, throttle);
			separator = ", ";
		}
		if (m->display_if_mission_set_speed_ge > 0)
			fprintf(f, "%smin_ordered_speed_mglt: %d", separator, m->display_if_mission_set_speed_ge);
		fprintf(f, "}\n");
	}
}

static bool write_craft_file(const char* path, const XwauNames* names, const XwauStockValues* stock,
							 const XwauCraftHud* hud, const XwauCraftEngineGlows* glows,
							 const XwauCraftWeapons* weapons, const XwauStatsProfiles* stats,
							 const XwauCraftShield* shield, const XwauCraftSound* sound,
							 const XwauCraftComponents* components, const XwauGunnerSeats* gunner,
							 const XwauCraftWarheadLock* lock, const XwauCraftJamming* jamming,
							 const XwauCraftDock* dock, const XwauCockpitFov* fov, const XwauMatFile* mat,
							 XwauLines* cockpits) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Made by the OpenXWA mod importer.\n");
	if (has_reticle(hud) || has_colors(hud))
		write_hud(f, names, hud);
	if (has_engine_glows(glows))
		write_engine_glows(f, glows);
	if (XwauMat_SurfaceRuleCount(mat))
		write_surfaces(f, mat);
	if (has_weapons(weapons))
		write_weapons(f, names, stock, weapons);
	XwauStatsProfiles_Write(f, stats);
	if (shield->has)
		write_shield(f, shield);
	if (XwauCraft_HasSound(sound))
		write_sound(f, names, sound);
	if (components->has)
		write_components(f, components);
	if (lock->has)
		XwauCraft_WriteWarheadLock(f, lock);
	if (jamming->has)
		XwauCraft_WriteJamming(f, jamming);
	if (dock->has)
		XwauCraft_WriteDock(f, dock);
	if (fov->has_large)
		fprintf(f, "cockpit_view: {wide_vertical_fov: %.2f}\n", (double)XwauModel_VerticalFov(fov->large));
	bool ok = XwauGunnerSeats_Write(f, gunner, cockpits);
	if (!ok)
		Report(NULL, "out of memory");
	ok = fclose(f) == 0 && ok;
	if (!ok)
		Report(path, "can't be written");
	return ok;
}

/* Writes the icon map `key`. It maps object type names to image numbers
   counted from 1, or to `none` for no icon, and only has the icons in
   `values` that differ from stock. The game stores an icon as its image
   number times 100, and no icon as 0. Returns the number of icons
   written. */
static size_t write_icons(FILE* f, const XwauNames* names, const char* key, const uint16_t* values,
						  size_t count, const uint16_t* stock, size_t stock_count) {
	size_t written = 0;
	for (size_t t = 0; t < count; ++t) {
		const uint16_t stock_icon = t < stock_count ? stock[t] : 0;
		const char*    name       = XwauNames_ObjectType(names, (int)t);
		if (values[t] == stock_icon)
			continue;
		/* No craft can use the icon of an object type that the game has no
		   name for. */
		if (!name)
			continue;
		if (values[t] % 100) {
			ReportFatal(key, "object type %s has icon %u, which isn't an image number times 100", name,
						values[t]);
			continue;
		}
		if (values[t] == 0)
			fprintf(f, "\n  %s: none", name);
		else
			fprintf(f, "\n  %s: %u", name, values[t] / 100);
		++written;
	}
	fputs(written ? "\n" : " {}\n", f);
	return written;
}

/* XWAU's edge filter on the target display (enable_wireframe_CMD). */
typedef struct TargetDisplay {
	bool  edges;
	float iff[6][3];
	float friend_color[3];
	float foe_color[3];
	float luminance[3];
	float contrast;
	bool  friend_foe;
} TargetDisplay;

/* Reads "x, y, z" after the '=' of `line`, as LoadGeneric3DCoords does. */
static void read_display_color(const char* line, float out[3]) {
	const char* value = strchr(line, '=');
	float       x, y, z;
	if (value && sscanf(value + 1, "%f, %f, %f", &x, &y, &z) == 3) {
		out[0] = x;
		out[1] = y;
		out[2] = z;
	}
}

/* Reads GreenAndRedForIFFColorsOnly as the ddraw reads it: from the
 * [hook_tourmultiplayer] section of Hooks.ini when that file exists, else
 * from hook_tourmultiplayer.cfg. The first line with the key wins. */
static bool read_friend_foe(const char* xwau_root) {
	char       path[1024], line[256], param[128], value[128];
	const bool ini = Xwau_FindFile(xwau_root, "Hooks.ini", path, sizeof path);
	if (!ini && !Xwau_FindFile(xwau_root, "hook_tourmultiplayer.cfg", path, sizeof path))
		return false;
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	bool section = !ini, on = false;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#')
			continue;
		if (ini && line[0] == '[') {
			char lower[256];
			LowerCopy(lower, sizeof lower, line);
			const bool was = section;
			section        = strstr(lower, "[hook_tourmultiplayer]") != NULL;
			if (was && !section)
				break;
			continue;
		}
		value[0] = 0;
		if (section && sscanf(line, "%127s = %127s", param, value) > 0 &&
			strcasecmp(param, "GreenAndRedForIFFColorsOnly") == 0) {
			on = atof(value) != 0.0;
			break;
		}
	}
	fclose(f);
	return on;
}

/* Reads Dynamic_Cockpit.cfg as LoadDCParams does, starting from the ddraw's
 * own values. Returns false when the installation has no such file. */
static bool read_target_display(const char* xwau_root, TargetDisplay* out) {
	static const float iff[6][3] = { { 0.1f, 0.7f, 0.1f }, { 0.5f, 0.1f, 0.1f }, { 0.1f, 0.1f, 0.5f },
									 { 0.3f, 0.2f, 0.1f }, { 0.1f, 0.1f, 0.1f }, { 0.5f, 0.1f, 0.5f } };
	char               path[1024], line[256], param[128], value[128];
	if (!Xwau_FindFile(xwau_root, "Dynamic_Cockpit.cfg", path, sizeof path))
		return false;
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	*out = (TargetDisplay) { .edges        = true,
							 .friend_color = { 0.1f, 0.6f, 0.1f },
							 .foe_color    = { 0.6f, 0.1f, 0.1f },
							 .luminance    = { 0.33f, 0.5f, 0.16f },
							 .contrast     = 3.0f };
	memcpy(out->iff, iff, sizeof out->iff);
	while (fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#')
			continue;
		value[0] = 0;
		if (sscanf(line, "%127s = %127s", param, value) <= 0)
			continue;
		if (strcasecmp(param, "enable_wireframe_CMD") == 0)
			out->edges = (float)atof(value) != 0.0f;
		else if (strcasecmp(param, "wireframe_contrast") == 0)
			out->contrast = (float)atof(value);
		else if (strcasecmp(param, "wireframe_luminance_vector") == 0)
			read_display_color(line, out->luminance);
		else if (strcasecmp(param, "wireframe_color_friend") == 0)
			read_display_color(line, out->friend_color);
		else if (strcasecmp(param, "wireframe_color_foe") == 0)
			read_display_color(line, out->foe_color);
		else if (strncasecmp(param, "wireframe_IFF_color_", 20) == 0 && param[20] >= '0' &&
				 param[20] <= '5' && !param[21])
			read_display_color(line, out->iff[param[20] - '0']);
	}
	fclose(f);
	out->friend_foe = read_friend_foe(xwau_root);
	return true;
}

static float srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* Writes an sRGB colour of XWAU's buffer as a linear one. */
static void write_display_color(FILE* f, const float srgb[3]) {
	fprintf(f, "[%.6g, %.6g, %.6g]", (double)srgb_to_linear(fmaxf(srgb[0], 0.0f)),
			(double)srgb_to_linear(fmaxf(srgb[1], 0.0f)), (double)srgb_to_linear(fmaxf(srgb[2], 0.0f)));
}

static void write_target_display(FILE* f, const TargetDisplay* display) {
	/* The ddraw's colour for a target without an IFF colour. */
	static const float other[3] = { 0.1f, 0.1f, 0.5f };
	fputs("target_display:\n", f);
	if (!display->edges) {
		fputs("  style: shaded\n", f);
		return;
	}
	fprintf(
		f,
		"  style: edges\n  edges:\n    luminance: [%.6g, %.6g, %.6g]\n    contrast: %.6g\n    colors: %s\n"
		"    iff: [",
		(double)display->luminance[0], (double)display->luminance[1], (double)display->luminance[2],
		(double)display->contrast, display->friend_foe ? "friend_foe" : "iff");
	for (int i = 0; i < 6; ++i) {
		fputs(i ? ", " : "", f);
		write_display_color(f, display->iff[i]);
	}
	fputs("]\n    friend: ", f);
	write_display_color(f, display->friend_color);
	fputs("\n    foe: ", f);
	write_display_color(f, display->foe_color);
	fputs("\n    other: ", f);
	write_display_color(f, other);
	fputs("\n", f);
}

/* The enhanced HUD settings of ddraw's SSAO.cfg, with ddraw's defaults. */
typedef struct EnhancedHud {
	bool   enabled, enhance_name_color, display_bars, vertical_bars, bg_text_box, depth_occlusion;
	int    font_size; /* 0 small, 1 medium, 2 large */
	double min_bracket, max_bracket, min_bar, max_bar, bar_frame;
	double shields[3], overshields[3], hull_full[3], hull_mid[3], hull_low[3], systems[3];
} EnhancedHud;

/* Reads one setting of SSAO.cfg, as ddraw's LoadSSAOParams does: the value
 * is read with atof, and a switch is on when it isn't 0. */
static void read_enhanced_hud_setting(const char* key, const char* value, void* data) {
	EnhancedHud* h = (EnhancedHud*)data;
	double       v = 0.0;
	Xwau_ReadSettingNumbers(value, 1, &v);
	const bool on = (float)v != 0.0f;
	if (strcasecmp(key, "enable_enhanced_hud") == 0) {
		h->enabled   = on;
		h->font_size = 1;
	} else if (strcasecmp(key, "enhanced_hud_enhance_name_col") == 0)
		h->enhance_name_color = on;
	else if (strcasecmp(key, "enhanced_hud_display_bars") == 0)
		h->display_bars = on;
	else if (strcasecmp(key, "enhanced_hud_vertical_bars") == 0)
		h->vertical_bars = on;
	else if (strcasecmp(key, "enhanced_hud_enable_bg_text_box") == 0)
		h->bg_text_box = on;
	else if (strcasecmp(key, "enhanced_hud_apply_depth_occlusion") == 0)
		h->depth_occlusion = on;
	else if (strcasecmp(key, "enhanced_hud_min_bracket_size") == 0)
		h->min_bracket = (int)(float)v;
	else if (strcasecmp(key, "enhanced_hud_max_bracket_size") == 0)
		h->max_bracket = (int)(float)v;
	else if (strcasecmp(key, "enhanced_hud_min_bar_length") == 0)
		h->min_bar = (float)v;
	else if (strcasecmp(key, "enhanced_hud_max_bar_length") == 0)
		h->max_bar = (float)v;
	else if (strcasecmp(key, "enhanced_hud_bar_frame_width") == 0)
		h->bar_frame = (float)v;
	else if (strcasecmp(key, "enhanced_hud_font_size") == 0) {
		const int size = (int)(float)v;
		h->font_size   = size == 0 || size == 1 ? 0 : (size == 3 ? 2 : 1);
	} else if (strcasecmp(key, "enhanced_hud_shields_col") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->shields);
	else if (strcasecmp(key, "enhanced_hud_overshds_col") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->overshields);
	else if (strcasecmp(key, "enhanced_hud_hull_col_full") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->hull_full);
	else if (strcasecmp(key, "enhanced_hud_hull_col_mid") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->hull_mid);
	else if (strcasecmp(key, "enhanced_hud_hull_col_low") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->hull_low);
	else if (strcasecmp(key, "enhanced_hud_sys_col") == 0)
		Xwau_ReadSettingNumbers(value, 3, h->systems);
}

/* Reads the enhanced HUD settings of SSAO.cfg. Returns whether the enhanced
 * HUD is on. */
static bool read_enhanced_hud(const char* xwau_root, EnhancedHud* out) {
	*out = (EnhancedHud) {
		.enhance_name_color = true,
		.display_bars       = true,
		.depth_occlusion    = true,
		.font_size          = 1,
		.min_bar            = 16.0,
		.max_bar            = 140.0,
		.bar_frame          = 3.0,
		.shields            = { 0.125, 0.5, 1.0 },
		.overshields        = { 0.49, 0.976, 1.0 },
		.hull_full          = { 0.0, 1.0, 0.0 },
		.hull_mid           = { 1.0, 1.0, 0.0 },
		.hull_low           = { 1.0, 0.0, 0.0 },
		.systems            = { 0.8, 0.753, 1.0 },
	};
	Xwau_ReadSettings(xwau_root, "SSAO.cfg", read_enhanced_hud_setting, out);
	return out->enabled;
}

static void write_readout_color(FILE* f, const char* key, const double srgb[3]) {
	const float color[3] = { (float)srgb[0], (float)srgb[1], (float)srgb[2] };
	fprintf(f, "  %s: ", key);
	write_display_color(f, color);
	fputs("\n", f);
}

static void write_readout_number(FILE* f, const char* key, double value) {
	fprintf(f, "  %s: ", key);
	Xwau_WriteDecimal(f, value);
	fputs("\n", f);
}

/* Writes the target_readout section, with every key, from the enhanced HUD
 * settings. */
static void write_target_readout(FILE* f, const EnhancedHud* h) {
	static const char* const sizes[] = { "small", "medium", "large" };
	fprintf(f, "target_readout:\n  enabled: true\n  text_size: %s\n", sizes[h->font_size]);
	/* The space that XWAU leaves under three lines of text: an identified
	   craft's name, cargo and distance. */
	write_readout_number(f, "text_gap", h->font_size == 2 ? 3.0 : 9.0);
	fprintf(f, "  brighten_text: %s\n  name_colors: single\n  bars: %s\n  layout: %s\n",
			h->enhance_name_color ? "true" : "false", h->display_bars ? "true" : "false",
			h->vertical_bars ? "vertical" : "horizontal");
	write_readout_number(f, "bar_length_min", h->min_bar);
	write_readout_number(f, "bar_length_max", h->max_bar);
	/* The bars' thickness and gap are tuned for OpenXWA, and don't come from
	   enhanced_hud_bar_thickness and enhanced_hud_bar_gap. */
	write_readout_number(f, "bar_thickness", 7.0);
	write_readout_number(f, "bar_gap", 4.0);
	write_readout_number(f, "bar_frame", h->bar_frame);
	write_readout_color(f, "shield_color", h->shields);
	write_readout_color(f, "overshield_color", h->overshields);
	write_readout_color(f, "hull_color_full", h->hull_full);
	write_readout_color(f, "hull_color_half", h->hull_mid);
	write_readout_color(f, "hull_color_low", h->hull_low);
	write_readout_color(f, "system_color", h->systems);
	write_readout_number(f, "component_box_min", h->min_bracket);
	write_readout_number(f, "component_box_max", h->max_bracket);
	fputs("  component_pulse: true\n", f);
	/* XWAU hides the readout where the scene is nearer than 25 m. */
	write_readout_number(f, "hidden_within_m", h->depth_occlusion ? 25.0 : 0.0);
	if (h->bg_text_box)
		ReportUnsupported("the SSAO.cfg key enhanced_hud_enable_bg_text_box");
}

/* Whether XWAU's ddraw draws the target boxes and the radar blips itself:
 * ddraw.cfg has Radar2DRendererEnabled, which is on by default. This function
 * reads the file as ddraw's Config does: the name is the text before '='
 * without its white space, the value is read with stoi, and the last line
 * wins. */
static bool read_radar_2d_renderer(const char* xwau_root) {
	char path[1024], line[512];
	if (!Xwau_FindFile(xwau_root, "ddraw.cfg", path, sizeof path))
		return false;
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	bool enabled = true;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == ';' || (line[0] == '/' && line[1] == '/'))
			continue;
		char* equals = strchr(line, '=');
		if (!equals)
			continue;
		char   name[128];
		size_t n = 0;
		for (const char* c = line; c < equals && n + 1 < sizeof name; ++c)
			if (!isspace((unsigned char)*c))
				name[n++] = *c;
		name[n] = 0;
		long long value;
		if (strcmp(name, "Radar2DRendererEnabled") == 0 && Xwau_Stoi(equals + 1, &value))
			enabled = value != 0;
	}
	fclose(f);
	return enabled;
}

static bool write_hud_file(const char* xwau_root, const char* output_dir, const XwauNames* names,
						   const XwauStockValues* stock_values) {
	char          hull_path[1024], map_path[1024], path[1024];
	TargetDisplay display;
	const bool    has_hull    = Xwau_FindFile(xwau_root, "HullIconList.txt", hull_path, sizeof hull_path);
	const bool    has_map     = Xwau_FindFile(xwau_root, "MapIconList.txt", map_path, sizeof map_path);
	const bool    has_display = read_target_display(xwau_root, &display);
	EnhancedHud   readout;
	const bool    has_readout = read_enhanced_hud(xwau_root, &readout);
	const bool    has_boxes   = read_radar_2d_renderer(xwau_root);
	if (!has_hull && !has_map && !has_display && !has_readout && !has_boxes)
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
		fputs("# Made by the OpenXWA mod importer.\n", f);
		if (has_hull) {
			fputs("hull_icons:", f);
			hull_count = write_icons(f, names, "hull_icons", hull, hull_count, stock_values->hull_icons,
									 stock_values->hull_icon_count);
		}
		if (has_map) {
			fputs("map_icons:", f);
			map_count = write_icons(f, names, "map_icons", map, map_count, stock_values->map_icons,
									stock_values->map_icon_count);
		}
		if (has_display)
			write_target_display(f, &display);
		/* XWAU's box lines are 2 pixels of the game's resolution, which the
		   importer doesn't know. The width is tuned for OpenXWA. */
		if (has_boxes)
			fputs("target_boxes:\n  line_width: 3\n", f);
		/* XWAU draws the blips as circles 4 pixels wide, and the current
		   target as a yellow circle 8 pixels wide. */
		if (has_boxes)
			fputs("radar:\n  blip_size: 4\n  target_size: 8\n", f);
		if (has_readout)
			write_target_readout(f, &readout);
		ok = fclose(f) == 0;
		if (!ok)
			Report(path, "can't be written");
		else
			Report(NULL,
				   "hud.yaml: %zu new hull icons, %zu new map icons, target display %s, target boxes %s, "
				   "radar blips %s, target readout %s",
				   hull_count, map_count,
				   !has_display    ? "not set"
				   : display.edges ? "with edges"
								   : "shaded",
				   has_boxes ? "wide" : "original", has_boxes ? "round" : "original",
				   !has_readout            ? "not set"
				   : readout.vertical_bars ? "on, vertical"
										   : "on, horizontal");
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
										   "WeaponHardpointTypes.txt",
										   "WarheadLock.txt",
										   "Jamming.txt",
										   "Dock.txt" };

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
	SIDE_WARHEAD_LOCK,
	SIDE_JAMMING,
	SIDE_DOCK,
	SIDE_FILES
};

/* Whether `name` is the first side file of a model that has no INI file, in
 * which case `model` gets the model's name. A model with several side files
 * is then read once. */
/* Whether `name` is the OPT of a model that has no .ini and no side file, so
   that only its material file can give it settings. Sets `model`. */
static bool opt_without_ini(char* const* names, size_t count, const char* name, char* model, size_t cap) {
	const size_t length = strlen(name);
	if (!EndsWithNoCase(name, ".opt") || length <= 4 || length - 4 >= cap)
		return false;
	memcpy(model, name, length - 4);
	model[length - 4] = '\0';
	if (has_side_file(names, count, model, ".ini"))
		return false;
	for (int side = 0; side < SIDE_FILES; ++side) {
		if (has_side_file(names, count, model, g_sideFiles[side]))
			return false;
	}
	return true;
}

/* Whether `txt` or else the section `section` of `ini` has lines. Either
 * path can be NULL. */
static bool has_section_lines(const char* txt, const char* ini, const char* section) {
	XwauLines lines = { 0 };
	if (txt)
		XwauLines_Read(txt, "", XWAU_RULES_CPP, &lines);
	bool any = lines.count != 0;
	XwauLines_Free(&lines);
	if (!any && ini) {
		XwauLines_Read(ini, section, XWAU_RULES_CPP, &lines);
		any = lines.count != 0;
		XwauLines_Free(&lines);
	}
	return any;
}

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

bool XwauCraft_Write(const char* xwau_root, const char* output_dir, const XwauNames* game_names,
					 const XwauStockValues* stock_values, const XwauGameTables* stock_tables,
					 XwauLines* cockpits) {
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
	int    written = 0, reticles = 0, colors = 0, engine_glows = 0, weapon_colors = 0;
	int    stats_profiles = 0, shields = 0, sounds = 0, components = 0;
	int    weapon_profiles = 0, warhead_locks = 0, jammings = 0, docks = 0, surfaces = 0, cockpit_views = 0;
	/* The materials that the .mat files start from (as opt2gltf reads them). */
	XwauMatParams global;
	char          global_path[1024];
	XwauMat_CodeDefaults(&global);
	if (Xwau_FindFile(xwau_root, "Materials/DefaultGlobalMaterial.mat", global_path, sizeof global_path))
		XwauMat_ReadGlobalDefault(global_path, &global);
	/* hook_dock reads the [Dock] sections. */
	char       dock_dll[1024];
	const bool dock_hook = Xwau_FindFile(xwau_root, "hook_dock.dll", dock_dll, sizeof dock_dll);
	/* hook_jamming reads the [Jamming] sections. */
	char       jamming_dll[1024];
	const bool jamming_hook = Xwau_FindFile(xwau_root, "hook_jamming.dll", jamming_dll, sizeof jamming_dll);
	/* hook_weapon_lock reads the [WarheadLock] sections. */
	char       weapon_lock_dll[1024];
	const bool weapon_lock =
		Xwau_FindFile(xwau_root, "hook_weapon_lock.dll", weapon_lock_dll, sizeof weapon_lock_dll);
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
	/* The genus of the object types that load each model in the import. The
	   units of the stats profiles depend on it. */
	XwauGameTables* tables = XwauGameTables_Load(xwau_root);
	const char*     models[XWAU_GAME_OBJECT_TYPES];
	XwauStatsGenus  genus = { 0 };

	ok = (!tables || XwauObjects_ImportedModels(stock_tables, tables, models)) &&
		 XwauStatsGenus_Read(xwau_root, tables, models, &genus);
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
			path[1100];
		bool         has_side[SIDE_FILES];
		const size_t length  = strlen(names[i]);
		bool         has_ini = false;
		if (EndsWithNoCase(names[i], ".ini") && length > 4 && length - 4 < sizeof model) {
			memcpy(model, names[i], length - 4);
			model[length - 4] = '\0';
			has_ini           = true;
		} else if (!first_side_file_without_ini(names, count, names[i], model, sizeof model) &&
				   !opt_without_ini(names, count, names[i], model, sizeof model)) {
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
		/* default.ini isn't a model, and the importer doesn't read these
		   sections there. The sections that no hook of the installation
		   reads have no effect in XWAU. */
		if (strcasecmp(model, "default") == 0) {
			if (jamming_hook && has_section_lines(side_path[SIDE_JAMMING], has_ini ? ini : NULL, "Jamming"))
				ReportUnsupported("the [Jamming] section of default.ini");
			if (dock_hook && has_section_lines(side_path[SIDE_DOCK], has_ini ? ini : NULL, "Dock"))
				ReportUnsupported("the [Dock] section of default.ini");
		}

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
			/* When the global section has lines, the hook only reads the
			   energy transfer keys of the model, so the other values aren't
			   written. */
			if (global_rate_lines)
				for (int key = 0; key < XWAU_CRAFT_WEAPON_RATES; ++key)
					if (!g_rateKeys[key].ship_first)
						weapons->rates.value[key] = -1;
			if (!XwauWeaponStats_Read(xwau_root, model, &weapons->stats)) {
				Report(NULL, "out of memory");
				ok = false;
				break;
			}
		} else {
			memset(weapons->rates.value, 0xff, sizeof weapons->rates.value);
		}
		XwauCraftShield shield;
		XwauCraft_ReadShield(side_path[SIDE_SHIELD], has_ini ? ini : NULL, &shield);
		XwauCraftSound sound;
		XwauCraft_ReadSound(engine_sound ? side_path[SIDE_SOUND] : NULL, engine_sound && has_ini ? ini : NULL,
							range_bank, &sound);
		XwauCraftComponents strengths;
		XwauCraft_ReadComponents(strength ? side_path[SIDE_SUBCOMPONENTS] : NULL,
								 strength && has_ini ? ini : NULL, &strengths);
		/* The [WarheadLock] section of default.ini holds global settings. */
		XwauCraftWarheadLock lock;
		const bool           model_lock = weapon_lock && strcasecmp(model, "default") != 0;
		XwauCraft_ReadWarheadLock(model_lock ? side_path[SIDE_WARHEAD_LOCK] : NULL,
								  model_lock && has_ini ? ini : NULL, &lock);
		XwauCraftJamming jamming;
		const bool       model_jamming = jamming_hook && strcasecmp(model, "default") != 0;
		XwauCraft_ReadJamming(model_jamming ? side_path[SIDE_JAMMING] : NULL,
							  model_jamming && has_ini ? ini : NULL, &jamming);
		XwauCraftDock dock;
		const bool    model_dock = dock_hook && strcasecmp(model, "default") != 0;
		XwauCraft_ReadDock(model_dock ? side_path[SIDE_DOCK] : NULL, model_dock && has_ini ? ini : NULL,
						   &dock);
		XwauCockpitFov fov;
		XwauModel_CockpitFov(xwau_root, model, &fov);
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
		char         mat_name[300], mat_path[1100];
		XwauMatFile* mat = NULL;
		snprintf(mat_name, sizeof mat_name, "Materials/%s.mat", model);
		if (Xwau_FindFile(xwau_root, mat_name, mat_path, sizeof mat_path))
			mat = XwauMat_Load(mat_path, &global);
		for (size_t r = 0; r < XwauMat_SurfaceRuleCount(mat); ++r) {
			if (XwauMat_SurfaceRuleAt(mat, r)->increase_brightness_with_mission_set_speed)
				ReportUnsupported("the .mat key IncreaseBrightnessWithMissionSetSpeed");
		}
		if (!has_reticle(&hud) && !has_colors(&hud) && !has_engine_glows(&glows) && !has_weapons(weapons) &&
			!stats.count && !shield.has && !XwauCraft_HasSound(&sound) && !strengths.has && !gunner.has &&
			!lock.has && !jamming.has && !dock.has && !fov.has_large && !XwauMat_SurfaceRuleCount(mat)) {
			XwauMat_Free(mat);
			XwauStatsProfiles_Free(&stats);
			XwauWeaponStats_Free(&weapons->stats);
			continue;
		}

		surfaces += XwauMat_SurfaceRuleCount(mat) != 0;
		if (!PathFormat(path, sizeof path, "%s/%s.yaml", craft_dir, key) || !MakeDirs(craft_dir) ||
			!write_craft_file(path, game_names, stock_values, &hud, &glows, weapons, &stats, &shield, &sound,
							  &strengths, &gunner, &lock, &jamming, &dock, &fov, mat, cockpits)) {
			XwauMat_Free(mat);
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
		warhead_locks += lock.has;
		jammings += jamming.has;
		docks += dock.has;
		cockpit_views += fov.has_large;
		XwauMat_Free(mat);
		XwauStatsProfiles_Free(&stats);
		written++;
		reticles += has_reticle(&hud);
		colors += has_colors(&hud);
		engine_glows += has_engine_glows(&glows);
		weapon_colors += has_weapons(weapons);
	}
	free(weapons);
	XwauStatsGenus_Free(&genus);
	XwauGameTables_Free(tables);
	FreeList(names, count);
	Report(
		NULL,
		"model settings: %d files, %d with a reticle, %d with HUD colours, %d with engine glows, %d with "
		"weapon settings, %d with weapon profiles, %d with stats profiles, %d with a shield recharge rate, "
		"%d with sounds, %d with component strengths, %d with gunner seats, %d with warhead lock settings, "
		"%d with jamming rules, %d with a docking approach height, %d with surface rules, %d with a wide "
		"cockpit view",
		written, reticles, colors, engine_glows, weapon_colors, weapon_profiles, stats_profiles, shields,
		sounds, components, gunner_seats, warhead_locks, jammings, docks, surfaces, cockpit_views);
	if (weapon_lock) {
		/* The global section only sets the AI's ranges by the category of
		   the AI's craft. */
		XwauLines global = { 0 };
		char      global_path[1024];
		if (Xwau_FindFile(xwau_root, "FlightModels/WarheadLock.txt", global_path, sizeof global_path))
			XwauLines_Read(global_path, "", XWAU_RULES_CPP, &global);
		if (!global.count &&
			Xwau_FindFile(xwau_root, "FlightModels/default.ini", global_path, sizeof global_path))
			XwauLines_Read(global_path, "WarheadLock", XWAU_RULES_CPP, &global);
		if (global.count)
			ReportUnsupported("the global [WarheadLock] section");
		XwauLines_Free(&global);
	}
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
			ReportUnsupported("the global AmbientSoundDistance");
		XwauLines_Free(&global);
	}
	return write_hud_file(xwau_root, output_dir, game_names, stock_values) && ok;
}
