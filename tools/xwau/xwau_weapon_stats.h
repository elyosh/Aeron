#ifndef AERON_XWAU_WEAPON_STATS_H
#define AERON_XWAU_WEAPON_STATS_H

/*
 * Reads the weapon profiles of hook_weapon_rate and turns them into
 * OpenXWA's weapon profiles.
 *
 * A model's sections are [WeaponStats], [WarheadStats], [EnergyStats],
 * [LinkingStats] and [WeaponHardpointTypes] in FlightModels/<Model>.ini. A
 * file FlightModels/<Model><Section>.txt wins over its section. The global
 * sections are in FlightModels/<Section>.txt, or else in
 * FlightModels/default.ini. The hook reads the model's lines first, then
 * the global lines, and the first line with a key wins. So a model's key
 * only hides the global key with the same name. Keys can be written in any
 * case. A value is read the way std::stoi(value, 0, 0) reads it, and -1
 * means "not set".
 *
 * [WeaponStats] keys look like
 * [<Profile>_]Weapon<NNN>[_Player][_Easy|_Medium|_Hard]_<Stat>, for weapon
 * types 280 to 307. The stats are DurationIntegerPart, DurationDecimalPart,
 * Score, Speed, Power, PowerSpeedPercent, FireRate, DurationOffset,
 * FireRatio, Range, IsPrecise, DegreeOfSpreadMask, DechargeRate,
 * RechargeRate and EnergyLowHighSeparation. Side and SideModel aren't
 * imported, since no mod sets them. When the hook's EnableWeaponStatsProfiles
 * is 0, the hook ignores the section, and so does the importer.
 *
 * [WarheadStats] keys look like [<Profile>_]Warhead<T>_CapacityPercent. A
 * value below -1 counts as 0, as in the hook.
 *
 * [EnergyStats] keys are [<Profile>_]SpeedFactor and
 * [<Profile>_]SpeedFactorForPlayer, a percent of the top speed. The hook
 * reads them as float32 values (std::stof(value) * 0.01f, kept between 0
 * and 1). The importer multiplies this value by 65536 in float32, as the
 * hook does before it passes the value to the game.
 *
 * [LinkingStats] keys are [<Profile>_]LaserLink,
 * PreventAILasersIonWhenDisabling, PreventAIIonWhenNotDisabling and
 * PreventPlayerLinkedLasersIon.
 *
 * [WeaponHardpointTypes] has one number per line: the kind of each OPT
 * hardpoint type, from type 0. The hook reads the model's lines, then the
 * global lines, and keeps the first 40. The types after the last line get
 * kind 0.
 *
 * A model's values go into the `weapons` group of its model file, and the
 * global values into the `weapons` section of flight/flight.yaml:
 *
 *   weapons:
 *     stats:
 *       default:
 *         280: {range: 65536, fire_rate: 78}
 *         282: {power: 250, player_easy: {power: 350}}
 *       "victory2b7m2r":
 *         303: {power: 900}
 *     warhead_capacity_percent: {default: {1: 50}}
 *     energy_speed_q16: {default: {player: 7864}}
 *     linking: {default: {laser_link: 2}}
 *     hardpoint_kinds: [0, 1, 1, ...]
 *
 * Profile names are written in lower case. The hook never reads a profile
 * named Default, because Default in a mission means "no profile". Keys of
 * such a profile are reported and left out. So are keys that are set twice
 * and keys that the hook doesn't read.
 *
 * A mission picks a profile for a flight group with WeaponProfile_fg_<N> in
 * <mission>_WeaponProfiles.txt, or else in the [WeaponProfiles] section of
 * <mission>.ini. Without either, the hook reads
 * FlightModels/WeaponProfiles.txt or the section of
 * FlightModels/default.ini. WarheadProfiles, EnergyProfiles and
 * LinkingProfiles work the same way. The names go into the flight group's
 * entry in the mission file:
 *
 *   flight_groups:
 *     13: { name: "Devastator", craft: 26, weapon_profile: "devastatorb7m2r" }
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum {
	XWAU_WEAPON_STATS_STATS,
	XWAU_WEAPON_STATS_WARHEADS,
	XWAU_WEAPON_STATS_ENERGY,
	XWAU_WEAPON_STATS_LINKING,
	XWAU_WEAPON_STATS_GROUPS
};

enum { XWAU_WEAPON_STATS_HARDPOINT_TYPES = 40, XWAU_WEAPON_STATS_NAME_MAX = 48 };

/* One value read from a section. */
typedef struct XwauWeaponStatsValue {
	char      profile[XWAU_WEAPON_STATS_NAME_MAX]; /* in lower case, "" when there is no profile */
	int       group;                               /* XWAU_WEAPON_STATS_* */
	int       index; /* the weapon type, the warhead type, all (0) or player (1), or the linking key */
	int       form;  /* the stat's form: plain, player, easy... */
	int       stat;
	long long value;
} XwauWeaponStatsValue;

typedef struct XwauWeaponStats {
	XwauWeaponStatsValue* items;
	size_t                count;
	/* The kinds of the hardpoint type lines. */
	int  hardpoint_kinds[XWAU_WEAPON_STATS_HARDPOINT_TYPES];
	int  hardpoint_kind_count;
	bool has_hardpoint_kinds;
} XwauWeaponStats;

/* Reads the sections of model `model` ("TieFighter") in the installation
 * `game`, or the global sections when `model` is NULL. Reads nothing when
 * the mod doesn't install hook_weapon_rate. As in the hook, the global
 * hardpoint kinds come after the model's. Returns false when out of
 * memory. */
bool XwauWeaponStats_Read(const char* game, const char* model, XwauWeaponStats* out);
void XwauWeaponStats_Free(XwauWeaponStats* stats);
bool XwauWeaponStats_Any(const XwauWeaponStats* stats);

/* Writes the groups of `stats` into a `weapons` map, at `indent`. */
void XwauWeaponStats_Write(FILE* f, const char* indent, const XwauWeaponStats* stats);

/* Returns whether model `model` or the global sections have profile
 * `profile` in `group`. */
bool XwauWeaponStats_HasProfile(const char* game, const char* model, int group, const char* profile);

/* The profiles that a mission picks for a flight group. */
typedef struct XwauWeaponProfileChoice {
	int  fg;
	char name[XWAU_WEAPON_STATS_GROUPS][XWAU_WEAPON_STATS_NAME_MAX]; /* "" for none */
} XwauWeaponProfileChoice;

typedef struct XwauWeaponProfileChoices {
	XwauWeaponProfileChoice* items; /* sorted by flight group */
	size_t                   count;
} XwauWeaponProfileChoices;

/* Reads the profiles that mission `base` ("Missions/1b7m2tftc-R") picks.
 * Returns false when out of memory. */
bool XwauWeaponStats_ReadChoices(const char* game, const char* base, XwauWeaponProfileChoices* out);
void XwauWeaponStats_FreeChoices(XwauWeaponProfileChoices* choices);
const XwauWeaponProfileChoice* XwauWeaponStats_FindChoice(const XwauWeaponProfileChoices* choices, int fg);
/* Reports the profiles in `choice` that model `model` doesn't have. `model`
 * is a model key, after the mission's swaps. XWAU gives such a flight group
 * the stock values, and OpenXWA gives it the `default` values. */
void XwauWeaponStats_ReportChoice(const char* game, const char* base, const char* model,
								  const XwauWeaponProfileChoice* choice);
bool XwauWeaponStats_AnyChoice(const XwauWeaponProfileChoice* choice);
/* Writes the profile keys of a flight group's entry, such as
 * ", weapon_profile: \"x\"". */
void XwauWeaponStats_WriteChoice(FILE* f, const XwauWeaponProfileChoice* choice);

/* Reads a float of hook_weapon_rate's settings the way std::stof reads it.
 * Returns false when the text has no number. */
bool XwauWeaponStats_Stof(const char* text, float* out);

#endif /* AERON_XWAU_WEAPON_STATS_H */
