#include "xwau_warhead_pools.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* The hook's names of the warhead types, and the mission file's. */
static const char* const g_hookNames[XWAU_WARHEAD_TYPES] = {
	"SpaceBombs",       "HeavyRockets", "Missiles", "ProtonTorpedos",   "AdvancedMissiles",
	"AdvancedTorpedos", "MagPulse",     "IonPulse", "AdvancedMagPulse", "ClusterBombs",
};
static const char* const g_names[XWAU_WARHEAD_TYPES] = {
	"space_bombs",        "heavy_rockets", "missiles",   "torpedoes",           "advanced_missiles",
	"advanced_torpedoes", "mag_pulses",    "ion_pulses", "advanced_mag_pulses", "cluster_bombs",
};

#define POOL_PREFIX "WarheadTypeCount_fg_"

/* Reads the lines of the mission's pools, or else of the global ones. */
static bool pool_lines(const char* game, const char* base, XwauLines* lines, char* source, size_t cap) {
	char rel[1200], path[1024];
	memset(lines, 0, sizeof *lines);
	if (!PathFormat(rel, sizeof rel, "%s_WarheadTypeCount.txt", base))
		return false;
	snprintf(source, cap, "%s", rel);
	if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, lines))
		return false;
	if (!lines->count) {
		if (!PathFormat(rel, sizeof rel, "%s.ini", base))
			return false;
		snprintf(source, cap, "%s", rel);
		if (!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "WarheadTypeCount",
							XWAU_RULES_CPP, lines))
			return false;
	}
	if (!lines->count) {
		snprintf(source, cap, "FlightModels/WarheadTypeCount.txt");
		if (!XwauLines_Read(Xwau_FindFile(game, source, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
							lines))
			return false;
	}
	if (!lines->count) {
		snprintf(source, cap, "FlightModels/default.ini");
		if (!XwauLines_Read(Xwau_FindFile(game, source, path, sizeof path) ? path : NULL, "WarheadTypeCount",
							XWAU_RULES_CPP, lines))
			return false;
	}
	return true;
}

/* Reads the flight group and the warhead type of the key of `line`. Returns
 * false when the key isn't one that the hook reads. The hook builds its keys
 * with std::to_string, so a flight group with a leading 0 never matches. */
static bool parse_key(const char* line, int* fg, int* type) {
	const char* eq = strchr(line, '=');
	char        name[256];
	if (!eq || (size_t)(eq - line) >= sizeof name)
		return false;
	snprintf(name, sizeof name, "%.*s", (int)(eq - line), line);
	Xwau_Trim(name, XWAU_RULES_CPP);
	const size_t prefix = strlen(POOL_PREFIX);
	if (strncasecmp(name, POOL_PREFIX, prefix) != 0)
		return false;
	const char* digits = name + prefix;
	const char* end    = digits;
	while (isdigit((unsigned char)*end))
		++end;
	if (end == digits || *end != '_' || (end - digits > 1 && *digits == '0') || end - digits > 3)
		return false;
	*fg = atoi(digits);
	for (*type = 0; *type < XWAU_WARHEAD_TYPES; ++*type) {
		if (Xwau_SameName(end + 1, g_hookNames[*type]))
			return true;
	}
	return false;
}

static int compare_pools(const void* a, const void* b) {
	const int x = ((const XwauWarheadPool*)a)->fg, y = ((const XwauWarheadPool*)b)->fg;
	return (x > y) - (x < y);
}

bool XwauWarheadPools_Read(const char* game, const char* base, bool report, XwauWarheadPools* out) {
	memset(out, 0, sizeof *out);
	char path[1024], source[1300];
	if (!Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path) ||
		!Xwau_FileHasText(path, POOL_PREFIX))
		return true;
	XwauLines lines;
	if (!pool_lines(game, base, &lines, source, sizeof source)) {
		Report(NULL, "out of memory");
		return false;
	}
	bool   ok    = true;
	size_t other = 0;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		int fg, type;
		if (!strchr(lines.items[i], '='))
			continue;
		if (!parse_key(lines.items[i], &fg, &type)) {
			++other;
			continue;
		}
		XwauWarheadPool* pool = NULL;
		for (size_t k = 0; !pool && k < out->count; ++k)
			pool = out->items[k].fg == fg ? &out->items[k] : NULL;
		if (pool)
			continue; /* this flight group's keys are already read */
		pool = (XwauWarheadPool*)ARRAY_PUSH(out->items, out->count);
		if (!pool) {
			ok = false;
			break;
		}
		pool->fg = fg;
		for (int t = 0; t < XWAU_WARHEAD_TYPES; ++t) {
			char      key[64], value[256];
			long long number;
			snprintf(key, sizeof key, POOL_PREFIX "%d_%s", fg, g_hookNames[t]);
			XwauLines_Value(&lines, key, XWAU_RULES_CPP, value, sizeof value);
			pool->count[t] = -1;
			if (!value[0])
				continue;
			if (!Xwau_StoiAuto(value, &number) || number < -1 || number > 1000000000) {
				if (report)
					Report(source, "%s = %s isn't a number of warheads; it is left out", key, value);
				continue;
			}
			pool->count[t] = (int)number;
		}
	}
	XwauLines_Free(&lines);
	if (!ok) {
		Report(NULL, "out of memory");
		XwauWarheadPools_Free(out);
		return false;
	}
	if (other && report)
		Report(
			source,
			"[WarheadTypeCount]: %zu lines have keys that hook_weapon_rate doesn't read; they are left out",
			other);
	if (out->count > 1)
		qsort(out->items, out->count, sizeof *out->items, compare_pools);
	return true;
}

void XwauWarheadPools_Free(XwauWarheadPools* pools) {
	free(pools->items);
	memset(pools, 0, sizeof *pools);
}

const XwauWarheadPool* XwauWarheadPools_Find(const XwauWarheadPools* pools, int fg) {
	for (size_t i = 0; i < pools->count; ++i) {
		if (pools->items[i].fg == fg)
			return &pools->items[i];
	}
	return NULL;
}

bool XwauWarheadPools_Any(const XwauWarheadPool* pool) {
	for (int t = 0; pool && t < XWAU_WARHEAD_TYPES; ++t) {
		if (pool->count[t] >= 0)
			return true;
	}
	return false;
}

void XwauWarheadPools_Write(FILE* f, const XwauWarheadPool* pool) {
	bool first = true;
	for (int t = 0; pool && t < XWAU_WARHEAD_TYPES; ++t) {
		if (pool->count[t] < 0)
			continue;
		fprintf(f, "%s%s: %d", first ? ", warheads: { " : ", ", g_names[t], pool->count[t]);
		first = false;
	}
	if (!first)
		fputs(" }", f);
}
