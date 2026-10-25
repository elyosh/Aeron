#include "xwau_object_limits.h"

#include <stdint.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* The stock size of a player's block of projectile slots. */
#define STOCK_PLAYER_PROJECTILES 12

/* Reads the settings of `hook`: its .cfg file, else its section of
 * Hooks.ini. Writes the name of the file into `source`, for the reports.
 * Returns false when out of memory. */
static bool hook_lines(const char* game, const char* hook, XwauLines* lines, char* source, size_t cap) {
	char path[1024];
	snprintf(source, cap, "%s.cfg", hook);
	if (!XwauLines_Read(Xwau_FindFile(game, source, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						lines))
		return false;
	if (lines->count)
		return true;
	XwauLines_Free(lines);
	snprintf(source, cap, "Hooks.ini");
	return XwauLines_Read(Xwau_FindFile(game, "Hooks.ini", path, sizeof path) ? path : NULL, hook,
						  XWAU_RULES_CPP, lines);
}

/* Reads `key` as GetFileKeyValueInt does, or `fallback` when the key isn't
 * set. A value that isn't a number stops XWAU; it is reported. */
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

/* The path of hook_crafts_count.dll, when the mod turns the hook on. */
static bool enabled_dll(const char* game, char* dll, size_t cap) {
	if (!Xwau_FindFile(game, "hook_crafts_count.dll", dll, cap))
		return false;
	XwauLines lines = { 0 };
	char      source[64];
	if (!hook_lines(game, "hook_crafts_count", &lines, source, sizeof source))
		return false;
	const bool enabled = hook_value(&lines, source, "IsHookEnabled", 0) != 0;
	XwauLines_Free(&lines);
	return enabled;
}

bool XwauObjectLimits_HookEnabled(const char* game) {
	char dll[1024];
	return enabled_dll(game, dll, sizeof dll);
}

/* Writes `key: value`, or reports a value that would leave no slots. */
static void write_count(FILE* f, const char* source, const char* hook_key, const char* key, long long value) {
	if (value <= 0 || value > INT32_MAX) {
		Report(source, "%s = %lld isn't imported: the game keeps its own count", hook_key, value);
		return;
	}
	fprintf(f, "  %s: %lld\n", key, value);
}

bool XwauObjectLimits_Write(FILE* f, const char* game) {
	char       dll[1024];
	const bool crafts          = enabled_dll(game, dll, sizeof dll);
	long long  weapons         = STOCK_PLAYER_PROJECTILES;
	char       rate_source[64] = "hook_weapon_rate.cfg";
	if (Xwau_FindFile(game, "hook_weapon_rate.dll", dll, sizeof dll) &&
		Xwau_FileHasText(dll, "WeaponsCount")) {
		XwauLines lines = { 0 };
		if (hook_lines(game, "hook_weapon_rate", &lines, rate_source, sizeof rate_source))
			weapons = hook_value(&lines, rate_source, "WeaponsCount", STOCK_PLAYER_PROJECTILES);
		XwauLines_Free(&lines);
	}
	if (!crafts && weapons == STOCK_PLAYER_PROJECTILES)
		return false;
	fputs("object_limits:\n", f);
	if (weapons != STOCK_PLAYER_PROJECTILES)
		write_count(f, rate_source, "WeaponsCount", "player_projectiles", weapons);
	if (!crafts)
		return true;
	XwauLines lines = { 0 };
	char      source[64];
	if (!hook_lines(game, "hook_crafts_count", &lines, source, sizeof source))
		return true;
	write_count(f, source, "CraftsCount", "craft_per_region",
				hook_value(&lines, source, "CraftsCount", 3072));
	write_count(f, source, "ProjectilesCount", "ai_projectiles_per_region",
				hook_value(&lines, source, "ProjectilesCount", 1024));
	write_count(f, source, "ExplosionsCount", "explosions_per_region",
				hook_value(&lines, source, "ExplosionsCount", 256));
	write_count(f, source, "Radar2DItemsCount", "radar_blips",
				hook_value(&lines, source, "Radar2DItemsCount", 48));
	XwauLines_Free(&lines);
	if (weapons != STOCK_PLAYER_PROJECTILES)
		Report(
			rate_source,
			"WeaponsCount = %lld with hook_crafts_count: XWAU keeps each player's block at 12 slots but "
			"searches %lld, so a player's shots spill into the AI's slots. Each player gets a block of %lld "
			"slots of its own instead",
			weapons, weapons, weapons);
	return true;
}

bool XwauObjectLimits_ReadMission(const char* game, const char* base, XwauMissionLimits* out) {
	memset(out, 0, sizeof *out);
	char      rel[1200], path[1024], source[1200];
	XwauLines lines = { 0 };
	if (!PathFormat(rel, sizeof rel, "%s_CraftsCount.txt", base))
		return false;
	snprintf(source, sizeof source, "%s", rel);
	if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	if (!lines.count) {
		XwauLines_Free(&lines);
		if (!PathFormat(rel, sizeof rel, "%s.ini", base))
			return false;
		snprintf(source, sizeof source, "%s [CraftsCount]", rel);
		if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "CraftsCount",
							XWAU_RULES_CPP, &lines))
			return false;
	}
	if (!lines.count) {
		XwauLines_Free(&lines);
		return true;
	}
	char dll[1024];
	if (!enabled_dll(game, dll, sizeof dll)) {
		Report(source, "the counts aren't imported: hook_crafts_count is off");
		XwauLines_Free(&lines);
		return true;
	}
	if (!Xwau_FileHasText(dll, "_CraftsCount.txt")) {
		Report(source, "the counts aren't imported: the mod's build of hook_crafts_count doesn't read them");
		XwauLines_Free(&lines);
		return true;
	}
	out->craft          = hook_value(&lines, source, "CraftsCount", 0);
	out->ai_projectiles = hook_value(&lines, source, "ProjectilesCount", 0);
	out->explosions     = hook_value(&lines, source, "ExplosionsCount", 0);
	XwauLines_Free(&lines);
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
