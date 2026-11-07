#include "xwau_object_limits.h"

#include <stdint.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* The stock size of a player's block of projectile slots. */
#define STOCK_PLAYER_PROJECTILES 12

/* The path of hook_crafts_count.dll, when the mod turns the hook on. */
static bool enabled_dll(const char* game, char* dll, size_t cap) {
	if (!Xwau_FindFile(game, "hook_crafts_count.dll", dll, cap))
		return false;
	XwauSection settings;
	if (!XwauSection_ReadHook(game, "hook_crafts_count", &settings))
		return false;
	const bool enabled = XwauSection_IntOr(&settings, "IsHookEnabled", 0) != 0;
	XwauSection_Free(&settings);
	return enabled;
}

bool XwauObjectLimits_HookEnabled(const char* game) {
	char dll[1024];
	return enabled_dll(game, dll, sizeof dll);
}

/* Writes `key: value`. A value that would leave no slots, or that doesn't
 * fit, isn't written, and the game keeps its own count. */
static void write_count(FILE* f, const char* key, long long value) {
	if (value <= 0 || value > INT32_MAX)
		return;
	fprintf(f, "  %s: %lld\n", key, value);
}

bool XwauObjectLimits_Write(FILE* f, const char* game) {
	char       dll[1024];
	const bool crafts            = enabled_dll(game, dll, sizeof dll);
	long long  weapons           = STOCK_PLAYER_PROJECTILES;
	char       rate_source[1100] = "hook_weapon_rate.cfg";
	if (Xwau_DllHasText(game, "hook_weapon_rate.dll", "WeaponsCount")) {
		XwauSection settings;
		if (XwauSection_ReadHook(game, "hook_weapon_rate", &settings)) {
			weapons = XwauSection_IntOr(&settings, "WeaponsCount", STOCK_PLAYER_PROJECTILES);
			if (settings.source[0])
				snprintf(rate_source, sizeof rate_source, "%s", settings.source);
		}
		XwauSection_Free(&settings);
	}
	if (!crafts && weapons == STOCK_PLAYER_PROJECTILES)
		return false;
	fputs("object_limits:\n", f);
	if (weapons != STOCK_PLAYER_PROJECTILES)
		write_count(f, "player_projectiles", weapons);
	if (!crafts)
		return true;
	XwauSection settings;
	if (!XwauSection_ReadHook(game, "hook_crafts_count", &settings))
		return true;
	write_count(f, "craft_per_region", XwauSection_IntOr(&settings, "CraftsCount", 3072));
	write_count(f, "ai_projectiles_per_region", XwauSection_IntOr(&settings, "ProjectilesCount", 1024));
	write_count(f, "explosions_per_region", XwauSection_IntOr(&settings, "ExplosionsCount", 256));
	write_count(f, "radar_blips", XwauSection_IntOr(&settings, "Radar2DItemsCount", 48));
	XwauSection_Free(&settings);
	if (weapons != STOCK_PLAYER_PROJECTILES)
		ReportDetail(
			rate_source,
			"WeaponsCount = %lld with hook_crafts_count: XWAU keeps each player's block at 12 slots but "
			"searches %lld, so a player's shots spill into the AI's slots. Each player gets a block of %lld "
			"slots of its own instead",
			weapons, weapons, weapons);
	return true;
}

bool XwauObjectLimits_ReadMission(const char* game, const char* base, XwauMissionLimits* out) {
	memset(out, 0, sizeof *out);
	XwauSection mission;
	if (!XwauSection_ReadMission(game, base, "CraftsCount", XWAU_RULES_CPP, &mission))
		return false;
	if (!mission.lines.count) {
		XwauSection_Free(&mission);
		return true;
	}
	/* XWAU doesn't read the counts when hook_crafts_count is off, or when
	   the mod's build of the hook doesn't read them. */
	char dll[1024];
	if (!enabled_dll(game, dll, sizeof dll) || !Xwau_FileHasText(dll, "_CraftsCount.txt")) {
		XwauSection_Free(&mission);
		return true;
	}
	out->craft          = XwauSection_IntOr(&mission, "CraftsCount", 0);
	out->ai_projectiles = XwauSection_IntOr(&mission, "ProjectilesCount", 0);
	out->explosions     = XwauSection_IntOr(&mission, "ExplosionsCount", 0);
	XwauSection_Free(&mission);
	/* XWAU can't use a count below 0 or above INT32_MAX. */
	long long* counts[] = { &out->craft, &out->ai_projectiles, &out->explosions };
	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
		if (*counts[i] < 0 || *counts[i] > INT32_MAX)
			*counts[i] = 0;
	}
	return true;
}

bool XwauObjectLimits_Any(const XwauMissionLimits* limits) {
	return limits->craft > 0 || limits->ai_projectiles > 0 || limits->explosions > 0;
}

void XwauObjectLimits_WriteMission(FILE* f, const XwauMissionLimits* limits) {
	if (!XwauObjectLimits_Any(limits))
		return;
	const char* const names[]  = { "craft_per_region", "ai_projectiles_per_region", "explosions_per_region" };
	const long long   values[] = { limits->craft, limits->ai_projectiles, limits->explosions };
	bool              open     = false;
	for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
		if (values[i] <= 0)
			continue;
		fprintf(f, "%s%s: %lld", open ? ", " : "object_limits: { ", names[i], values[i]);
		open = true;
	}
	fputs(" }\n", f);
}
