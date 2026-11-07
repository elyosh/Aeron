#include "xwau_warhead_pools.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* The hook's names for the warhead types. */
static const char* const g_hookNames[XWAU_WARHEAD_TYPES] = {
	"SpaceBombs",       "HeavyRockets", "Missiles", "ProtonTorpedos",   "AdvancedMissiles",
	"AdvancedTorpedos", "MagPulse",     "IonPulse", "AdvancedMagPulse", "ClusterBombs",
};

#define POOL_PREFIX "WarheadTypeCount_fg_"

/* Reads the mission's pools, or else the global ones. */
static bool pool_lines(const char* game, const char* base, XwauSection* out) {
	if (!XwauSection_ReadMission(game, base, "WarheadTypeCount", XWAU_RULES_CPP, out))
		return false;
	if (out->lines.count)
		return true;
	XwauSection_Free(out);
	return XwauSection_ReadGlobal(game, "WarheadTypeCount", XWAU_RULES_CPP, out);
}

/* Reads the flight group and the warhead type of the key of `line`. Returns
 * false when the key isn't one that the hook reads. The hook builds its keys
 * with std::to_string, so a flight group with a leading 0 never matches. */
static bool parse_key(const char* line, int* fg, int* type) {
	char name[256];
	if (!Xwau_LineKey(line, XWAU_RULES_CPP, name, sizeof name))
		return false;
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
	if (!Xwau_DllHasText(game, "hook_weapon_rate.dll", POOL_PREFIX))
		return true;
	XwauSection section;
	if (!pool_lines(game, base, &section)) {
		Report(NULL, "out of memory");
		return false;
	}
	bool ok = true;
	/* hook_weapon_rate doesn't read lines with other keys, and reads the
	   first line of a key only. */
	for (size_t i = 0; ok && i < section.lines.count; ++i) {
		int fg, type;
		if (!parse_key(section.lines.items[i], &fg, &type))
			continue;
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
			char      key[64];
			long long number;
			snprintf(key, sizeof key, POOL_PREFIX "%d_%s", fg, g_hookNames[t]);
			pool->count[t] = -1;
			if (XwauSection_Int(&section, key, &number) != XWAU_VALUE_SET)
				continue;
			if (number < -1 || number > 1000000000) {
				if (report)
					ReportFatal(section.source, "%s = %lld isn't a number of warheads that OpenXWA can use",
								key, number);
				continue;
			}
			pool->count[t] = (int)number;
		}
	}
	XwauSection_Free(&section);
	if (!ok) {
		Report(NULL, "out of memory");
		XwauWarheadPools_Free(out);
		return false;
	}
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

void XwauWarheadPools_Write(FILE* f, const XwauNames* names, const XwauWarheadPool* pool) {
	bool first = true;
	for (int t = 0; pool && t < XWAU_WARHEAD_TYPES; ++t) {
		if (pool->count[t] < 0)
			continue;
		/* Pool 0 is the first warhead after `none`. */
		fprintf(f, "%s%s: %d", first ? ", warheads: { " : ", ", XwauNames_Name(&names->warheads, t + 1),
				pool->count[t]);
		first = false;
	}
	if (!first)
		fputs(" }", f);
}
