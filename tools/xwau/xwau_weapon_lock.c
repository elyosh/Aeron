#include "xwau_weapon_lock.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* The hook's timers are in seconds of 236 ticks, and the game's timer is
 * 16-bit. */
enum { HOOK_TICKS_PER_SECOND = 236, MAX_DISABLE_SECONDS = 65535 / HOOK_TICKS_PER_SECOND };

/* The game takes up to 0x4000 off the AI's laser range when the target is
 * fast and flies the same way. The range is unsigned, so a smaller range
 * wraps around, and the AI then fires from any distance. */
enum { LASER_RANGE_WRAP = 0x4000 };

/* The game doubles the break range of small targets when the two craft face
 * apart, so a larger value would overflow. */
enum { SMALL_RANGE_MAX = INT_MAX / 2 };

bool XwauWeaponLock_Features(const char* game, XwauWeaponLockFeatures* out) {
	char path[1024];
	memset(out, 0, sizeof *out);
	if (!Xwau_FindFile(game, "hook_weapon_lock.dll", path, sizeof path))
		return false;
	out->attack_break = Xwau_FileHasText(path, "AttackBreakDistance");
	out->avoid_hits   = out->attack_break && Xwau_FileHasText(path, "AIAvoidHitOrder");
	out->mag_pulse    = Xwau_FileHasText(path, "WeaponLockBehaviors");
	out->global_lock  = Xwau_FileHasText(path, "AILockDistance1_Starship");
	return true;
}

/* Writes a key of a YAML map: as it is when it only has lower-case letters,
 * digits and '_', else quoted. */
static void write_key(FILE* f, const char* key) {
	bool plain = key[0] != '\0';
	for (const char* c = key; *c && plain; ++c)
		plain = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
	if (plain)
		fputs(key, f);
	else
		Xwau_WriteString(f, key);
}

/* The keys of [AttackBreakDistance], in the order of XwauAiAttackValue. */
static const char* const g_attackKeys[XWAU_AI_ATTACK_VALUES + 1] = {
	"Warhead",
	"SmallObject_NoviceRank",
	"SmallObject_OfficerRank",
	"SmallObject_VeteranRank",
	"SmallObject_AceRank",
	"SmallObject_TopAceRank",
	"SmallObject_SuperAceRank",
	"LargeObject_MaxDistance",
	"MaxRange_Novice",
	"MaxRange_Officer",
	"MaxRange_Veteran",
	"MaxRange_Ace",
	"MaxRange_TopAce",
	"MaxRange_SuperAce",
	"AIAvoidHitOrder",
	NULL,
};

static const char* const g_attackSection = "AttackBreakDistance";

/* Reads the Model_<name> keys of `lines` into `out`. The hook reads the
 * first line of each key, for each model of the game. Returns false when
 * out of memory. */
static bool read_offsets(const char* game, const XwauSection* section, XwauAiAttack* out) {
	for (size_t i = 0; i < section->lines.count; ++i) {
		char      key[256], opt[300], path[1024];
		long long value;
		if (!XwauSection_KeyAt(section, i, key, sizeof key) || strncasecmp(key, "Model_", 6) != 0 ||
			!key[6] || XwauSection_Int(section, key, &value) != XWAU_VALUE_SET)
			continue;
		snprintf(opt, sizeof opt, "FlightModels/%s.opt", key + 6);
		if (!Xwau_FindFile(game, opt, path, sizeof path)) {
			Report(section->source, "%s names no model of FlightModels, so it is ignored", key);
			continue;
		}
		if (strlen(key + 6) >= sizeof out->offsets[0].model) {
			Report(section->source, "%s has a model name that is too long. It is left out", key);
			continue;
		}
		if (!ArrayGrow((void**)&out->offsets, out->offset_count, sizeof *out->offsets))
			return false;
		XwauAiAttackOffset* offset = &out->offsets[out->offset_count++];
		LowerCopy(offset->model, sizeof offset->model, key + 6);
		offset->offset = (int)value;
	}
	return true;
}

/* Reads the [AttackBreakDistance] section `section` into `out`. */
static bool read_attack(const char* game, const XwauSection* section, const XwauWeaponLockFeatures* features,
						XwauAiAttack* out) {
	const char* source = section->source;
	memset(out, 0, sizeof *out);
	for (int i = 0; i < XWAU_AI_ATTACK_VALUES; ++i) {
		long long value;
		if (i == XWAU_AI_ATTACK_AVOID_HITS && !features->avoid_hits)
			continue;
		if (XwauSection_Int(section, g_attackKeys[i], &value) != XWAU_VALUE_SET)
			continue;
		if (i >= XWAU_AI_ATTACK_SMALL && i < XWAU_AI_ATTACK_BIG_MAX) {
			/* The craft never breaks off by distance with 0 or less. */
			if (value < 0)
				value = 0;
			if (value > SMALL_RANGE_MAX) {
				ReportFatal(source, "%s = %lld is above %d, which the game can't use", g_attackKeys[i], value,
							SMALL_RANGE_MAX);
				continue;
			}
		} else if (i >= XWAU_AI_ATTACK_LASER && i < XWAU_AI_ATTACK_AVOID_HITS) {
			if (value < 0) {
				ReportFatal(source, "%s = %lld is negative, which the game can't use", g_attackKeys[i],
							value);
				continue;
			}
			if (value < LASER_RANGE_WRAP)
				Report(source,
					   "%s = %lld is below %d (400 m). As in XWAU, the AI then fires its lasers from any "
					   "distance at a fast target that flies the same way",
					   g_attackKeys[i], value, LASER_RANGE_WRAP);
		} else if (i == XWAU_AI_ATTACK_AVOID_HITS) {
			value = value != 0;
		}
		out->value[i] = (int)value;
		out->set[i]   = true;
		out->has      = true;
	}
	if (!read_offsets(game, section, out)) {
		Report(NULL, "out of memory");
		return false;
	}
	out->has = out->has || out->offset_count != 0;
	/* AIAvoidHitOrder is the last key. Older DLLs don't read it. */
	const char* known[XWAU_AI_ATTACK_VALUES + 1];
	memcpy(known, g_attackKeys, sizeof known);
	if (!features->avoid_hits)
		known[XWAU_AI_ATTACK_AVOID_HITS] = NULL;
	static const char* const prefixes[] = { "Model_", NULL };
	XwauSection_ReportUnknownKeys(section, known, prefixes);
	return true;
}

bool XwauAiAttack_Read(const char* game, const char* txt, const char* ini,
					   const XwauWeaponLockFeatures* features, XwauAiAttack* out) {
	XwauSection section;
	if (!XwauSection_ReadPaths(txt, ini, g_attackSection, XWAU_RULES_CPP, &section))
		return false;
	const bool ok = read_attack(game, &section, features, out);
	XwauSection_Free(&section);
	return ok;
}

bool XwauAiAttack_ReadGlobal(const char* game, const XwauWeaponLockFeatures* features, XwauAiAttack* out) {
	XwauSection section;
	if (!XwauSection_ReadGlobal(game, g_attackSection, XWAU_RULES_CPP, &section))
		return false;
	const bool ok = read_attack(game, &section, features, out);
	XwauSection_Free(&section);
	return ok;
}

void XwauAiAttack_Free(XwauAiAttack* attack) {
	free(attack->offsets);
	attack->offsets      = NULL;
	attack->offset_count = 0;
}

/* Writes the values of `attack` from `first`, by rank, as the map `name`.
 * `separator` is written before the map, and becomes ", ". */
static void write_ranks(FILE* f, const XwauNames* names, const XwauAiAttack* attack, int first,
						const char* name, const char** separator) {
	bool open = false;
	for (int rank = 0; rank < XWAU_AI_ATTACK_RANKS; ++rank) {
		if (!attack->set[first + rank])
			continue;
		if (!open)
			fprintf(f, "%s%s: {", *separator, name);
		fprintf(f, "%s%s: ", open ? ", " : "", XwauNames_Name(&names->ranks, rank));
		Xwau_WriteMetres(f, attack->value[first + rank]);
		open = true;
	}
	if (open) {
		fputc('}', f);
		*separator = ", ";
	}
}

void XwauAiAttack_Write(FILE* f, const XwauNames* names, const XwauAiAttack* attack) {
	const char* separator = "  break_range_m: {";
	fputs("ai_attack:\n", f);
	if (attack->set[XWAU_AI_ATTACK_WARHEAD]) {
		fprintf(f, "%swarhead: ", separator);
		Xwau_WriteMetres(f, attack->value[XWAU_AI_ATTACK_WARHEAD]);
		separator = ", ";
	}
	write_ranks(f, names, attack, XWAU_AI_ATTACK_SMALL, "small", &separator);
	if (attack->set[XWAU_AI_ATTACK_BIG_MAX]) {
		fprintf(f, "%sbig_max: ", separator);
		Xwau_WriteMetres(f, attack->value[XWAU_AI_ATTACK_BIG_MAX]);
		separator = ", ";
	}
	if (attack->offset_count) {
		fprintf(f, "%starget_offset: {", separator);
		for (size_t i = 0; i < attack->offset_count; ++i) {
			fputs(i ? ", " : "", f);
			write_key(f, attack->offsets[i].model);
			fputs(": ", f);
			Xwau_WriteMetres(f, attack->offsets[i].offset);
		}
		fputc('}', f);
		separator = ", ";
	}
	if (separator[0] == ',')
		fputs("}\n", f);
	separator = "  ";
	write_ranks(f, names, attack, XWAU_AI_ATTACK_LASER, "laser_range_m", &separator);
	if (separator[0] == ',')
		fputc('\n', f);
	if (attack->set[XWAU_AI_ATTACK_AVOID_HITS])
		fprintf(f, "  avoid_hits: %s\n", attack->value[XWAU_AI_ATTACK_AVOID_HITS] ? "true" : "false");
}

static const char* const g_pulseSection = "WeaponLockBehaviors";

/* Reads a disable time of [WeaponLockBehaviors], in hook seconds, as ticks.
 * -1 stays -1 when `default_allowed`. */
static int read_disable_ticks(const XwauSection* section, const char* key, bool default_allowed) {
	long long value;
	if (XwauSection_Int(section, key, &value) != XWAU_VALUE_SET)
		return -1;
	if (value == -1 && default_allowed)
		return -1;
	if (value < 0 || value > MAX_DISABLE_SECONDS) {
		ReportFatal(section->source, "%s = %lld isn't from 0 to %d seconds, which the game can't use", key,
					value, MAX_DISABLE_SECONDS);
		return -1;
	}
	return (int)value * HOOK_TICKS_PER_SECOND;
}

/* Reads MagPulse_DrainWeaponEnergy. The hook only changes the hit when it
 * is 1. In a model's section, -1 isn't set. */
static int read_drain(const XwauSection* section, bool model) {
	long long value;
	if (XwauSection_Int(section, "MagPulse_DrainWeaponEnergy", &value) != XWAU_VALUE_SET)
		return -1;
	if (model && value == -1)
		return -1;
	return value == 1;
}

static void read_pulse(const XwauSection* section, bool global, XwauMagPulse* out) {
	static const char* const model_keys[]  = { "MagPulse_DrainWeaponEnergy", "MagPulse_CraftDisableTimer",
											   NULL };
	static const char* const global_keys[] = { "MagPulse_DrainWeaponEnergy",
											   "MagPulse_SmallCraftDisableTimer",
											   "MagPulse_LargeCraftDisableTimer", NULL };
	out->drain_only                        = read_drain(section, !global);
	if (global) {
		out->disable_ticks[0] = read_disable_ticks(section, global_keys[1], false);
		out->disable_ticks[1] = read_disable_ticks(section, global_keys[2], false);
	} else {
		out->disable_ticks[0] = read_disable_ticks(section, model_keys[1], true);
		out->disable_ticks[1] = -1;
	}
	out->has = out->drain_only >= 0 || out->disable_ticks[0] >= 0 || out->disable_ticks[1] >= 0;
	XwauSection_ReportUnknownKeys(section, global ? global_keys : model_keys, NULL);
}

void XwauMagPulse_Read(const char* txt, const char* ini, XwauMagPulse* out) {
	XwauSection section;
	XwauSection_ReadPaths(txt, ini, g_pulseSection, XWAU_RULES_CPP, &section);
	read_pulse(&section, false, out);
	XwauSection_Free(&section);
}

void XwauMagPulse_ReadGlobal(const char* game, XwauMagPulse* out) {
	XwauSection section;
	XwauSection_ReadGlobal(game, g_pulseSection, XWAU_RULES_CPP, &section);
	read_pulse(&section, true, out);
	XwauSection_Free(&section);
}

void XwauMagPulse_Write(FILE* f, const XwauMagPulse* pulse, bool global) {
	const char* separator = "mag_pulse: {";
	if (pulse->drain_only >= 0) {
		fprintf(f, "%sdrain_only: %s", separator, pulse->drain_only ? "true" : "false");
		separator = ", ";
	}
	if (!global && pulse->disable_ticks[0] >= 0) {
		fprintf(f, "%sdisable_time_ms: ", separator);
		Xwau_WriteUnit(f, XWAU_UNIT_MILLISECONDS, pulse->disable_ticks[0]);
		separator = ", ";
	} else if (global && (pulse->disable_ticks[0] >= 0 || pulse->disable_ticks[1] >= 0)) {
		static const char* const sizes[] = { "small", "large" };
		const char*              inner   = "disable_time_ms: {";
		fputs(separator, f);
		for (int size = 0; size < 2; ++size) {
			if (pulse->disable_ticks[size] < 0)
				continue;
			fprintf(f, "%s%s: ", inner, sizes[size]);
			Xwau_WriteUnit(f, XWAU_UNIT_MILLISECONDS, pulse->disable_ticks[size]);
			inner = ", ";
		}
		fputc('}', f);
		separator = ", ";
	}
	if (separator[0] == ',')
		fputs("}\n", f);
}

/* The craft categories of the global [WarheadLock] keys, and their genus. */
static const struct {
	const char* category;
	int         genus;
} g_lockCategories[] = {
	{ "Starship", 4 },  { "Platform", 5 },   { "Freighter", 3 },
	{ "Transport", 1 }, { "Container", 17 }, { "Armament", 19 },
};

enum { LOCK_CATEGORIES = sizeof g_lockCategories / sizeof g_lockCategories[0] };

/* Writes the `warhead_lock` section from the global [WarheadLock] section of
 * `game`. Returns whether it was written. */
static bool write_global_lock(FILE* f, const char* game, const XwauNames* names) {
	static const char* const tiers[] = { "big", "big_top_tier" };
	char                     known_keys[2 * LOCK_CATEGORIES][64];
	const char*              known[2 * LOCK_CATEGORIES + 1];
	int                      value[LOCK_CATEGORIES][2];
	bool                     any = false;
	XwauSection              section;
	XwauSection_ReadGlobal(game, "WarheadLock", XWAU_RULES_CPP, &section);
	for (int c = 0; c < LOCK_CATEGORIES; ++c) {
		for (int tier = 0; tier < 2; ++tier) {
			char*     key = known_keys[2 * c + tier];
			long long number;
			snprintf(key, sizeof known_keys[0], "AILockDistance%d_%s", tier + 1,
					 g_lockCategories[c].category);
			known[2 * c + tier] = key;
			value[c][tier]      = -1;
			if (XwauSection_Int(&section, key, &number) != XWAU_VALUE_SET)
				continue;
			if (number < 0) {
				ReportFatal(section.source, "%s = %lld is negative, which the game can't use", key, number);
				continue;
			}
			value[c][tier] = (int)number;
			any            = true;
		}
	}
	known[2 * LOCK_CATEGORIES] = NULL;
	XwauSection_ReportUnknownKeys(&section, known, NULL);
	XwauSection_Free(&section);
	if (!any)
		return false;
	fputs("warhead_lock:\n  ai_lock_range_m:\n", f);
	for (int c = 0; c < LOCK_CATEGORIES; ++c) {
		const char* separator = "";
		const char* genus     = XwauNames_Name(&names->genus, g_lockCategories[c].genus);
		if (!genus || (value[c][0] < 0 && value[c][1] < 0))
			continue;
		fprintf(f, "    %s: {", genus);
		for (int tier = 0; tier < 2; ++tier) {
			if (value[c][tier] < 0)
				continue;
			fprintf(f, "%s%s: ", separator, tiers[tier]);
			Xwau_WriteMetres(f, value[c][tier]);
			separator = ", ";
		}
		fputs("}\n", f);
	}
	return true;
}

bool XwauWeaponLock_WriteFlightSettings(FILE* f, const char* game, const XwauNames* names, bool* ai_attack,
										bool* mag_pulse, bool* warhead_lock) {
	XwauWeaponLockFeatures features;
	*ai_attack = *mag_pulse = *warhead_lock = false;
	if (!XwauWeaponLock_Features(game, &features))
		return true;
	if (features.attack_break) {
		/* The hook's laser ranges by rank, which it uses even with no
		   data. The stock game's ranges differ. */
		static const int laser_defaults[XWAU_AI_ATTACK_RANKS] = { 24576, 32768, 40960, 40960, 40960, 40960 };
		XwauAiAttack     global;
		if (!XwauAiAttack_ReadGlobal(game, &features, &global))
			return false;
		for (int rank = 0; rank < XWAU_AI_ATTACK_RANKS; ++rank) {
			if (!global.set[XWAU_AI_ATTACK_LASER + rank]) {
				global.value[XWAU_AI_ATTACK_LASER + rank] = laser_defaults[rank];
				global.set[XWAU_AI_ATTACK_LASER + rank]   = true;
			}
		}
		XwauAiAttack_Write(f, names, &global);
		XwauAiAttack_Free(&global);
		*ai_attack = true;
	}
	if (features.mag_pulse) {
		XwauMagPulse pulse;
		XwauMagPulse_ReadGlobal(game, &pulse);
		if (pulse.has) {
			XwauMagPulse_Write(f, &pulse, true);
			*mag_pulse = true;
		}
	}
	if (features.global_lock)
		*warhead_lock = write_global_lock(f, game, names);
	return true;
}
