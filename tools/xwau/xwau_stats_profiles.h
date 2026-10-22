#ifndef AERON_XWAU_STATS_PROFILES_H
#define AERON_XWAU_STATS_PROFILES_H

/*
 * XWAU's stats profiles (hook_mission_tie), as OpenXWA's `stats_profiles`
 * (stats_profiles.h in the game).
 *
 * A model's profiles are the sections [StatsProfile_<name>] of
 * FlightModels/<Model>.ini, or the files
 * FlightModels/<Model>StatsProfile_<name>.txt, which win over a section of
 * the same name. A section counts whatever its case, and a missing or empty
 * one counts as missing. Each becomes one profile of the model file, named
 * <name> in lower case:
 *
 *   stats_profiles:
 *     default: {speed: 86, hull: 3045, has_shields: true}
 *     default_player_easy: {...}
 *
 * The keys are read as hook_mission_tie reads them (std::stoi(value, 0, 0),
 * -1 means not set, the first line of a key wins) and converted to the
 * craft record's units, as the hook does:
 *   - Speed, Acceleration, Deceleration (MGLT): (int)(v * 2.25f + 0.5f);
 *   - Pitch, Roll, Yaw: v * 256;
 *   - CriticalDamageThreshold (else ExplosionStrength) and HullStrength:
 *     v * 105, ShieldStrength: v * 50; then / 16 for the object types of
 *     genus 4 and 5 (starships, platforms), / 4 for genus 3 and 17
 *     (freighters, containers);
 *   - SystemStrength, SpeedIncrement, SpeedDecrement: as they are;
 *   - HasShieldGenerator, HasHyperdrive: true when not 0;
 *   - IsWarheadCollisionDamagesEnabled: written only when 0.
 * The genus is that of the object types that load the model, through their
 * list rows or the missions' [Objects] swaps. A profile that needs the
 * division is left out, and reported, when no object type loads the model
 * or object types of different genus classes load it. Sections that look
 * like profiles but that the hook never reads ([StatsProfile_Default;],
 * [;StatsProfile_Default]) are counted, and keys that the hook doesn't read
 * and [StatsModifiers*] sections, which the game doesn't have, are
 * reported.
 *
 * A mission's choices are the lines of <mission>_StatsProfiles.txt, else of
 * the [StatsProfiles] section of <mission>.ini:
 *   <Model>_fg_player = name    the profile of a player's craft
 *   <Model>_fgc_<N> = name      the profile of craft with markings N
 *   <Model> = name              the profile of the model's other craft
 *   <Key>Percent = p            a percent for every craft, and
 *   Player<Key>Percent = p      for players' craft
 * <Model> is the model that the craft loads, after the swaps. Keys count
 * whatever their case, and the first line of a key wins. A value keeps any
 * comment, as in the hook, so such a name finds no profile. A name that the
 * model has no profile for is kept, and reported: it still keeps the model's
 * default profile from applying. A name longer than 63 characters is cut.
 * <Model>_fg_<N> keys, which the hook doesn't
 * read, and markings written another way than the hook writes them
 * (_fgc_02) are reported and left out. They are written as:
 *
 *   stats_profiles:
 *     models:
 *       escorttransport: {player: x1, markings: {2: etrmod}, all: rebel}
 *     percent: {hull: 120}
 *     player_percent: {hull: 400}
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "xwau_config.h"
#include "xwau_game_tables.h"

enum { XWAU_STATS_FIELDS = 15, XWAU_STATS_PERCENT_FIELDS = 10, XWAU_STATS_NAME_MAX = 64 };

typedef struct XwauStatsProfile {
	char      name[XWAU_STATS_NAME_MAX];
	bool      set[XWAU_STATS_FIELDS];
	long long value[XWAU_STATS_FIELDS];
} XwauStatsProfile;

typedef struct XwauStatsProfiles {
	XwauStatsProfile* items;
	size_t            count;
	/* Sections that look like profiles but that the hook never reads, such
	 * as [StatsProfile_Default;]. */
	size_t disabled;
} XwauStatsProfiles;

/* The genus class of each model: 1 divides by 4, 2 by 16, 0 neither. */
typedef struct XwauStatsGenus {
	struct XwauStatsGenusModel {
		char key[256];
		int  genus_class; /* -2 when object types of different classes load it */
	}* items;

	size_t count;
} XwauStatsGenus;

/* Finds the genus class of the models of the installation `game` from
 * `tables` (NULL when they can't be read) and the [Objects] swaps of the
 * missions. Returns false when out of memory. */
bool XwauStatsGenus_Read(const char* game, const XwauGameTables* tables, XwauStatsGenus* out);
void XwauStatsGenus_Free(XwauStatsGenus* genus);
/* The genus class of model `key`, -1 when no object type loads it, -2 when
 * the classes differ. */
int XwauStatsGenus_Find(const XwauStatsGenus* genus, const char* key);

/* Reads the profiles of model `model` (the INI's name without ".ini", in
 * any case) of `game`, whose FlightModels folder lists `names`, with the
 * genus class `genus_class` (XwauStatsGenus_Find). Returns false when out
 * of memory. */
bool XwauStatsProfiles_Read(const char* game, const char* model, char* const* names, size_t name_count,
							int genus_class, XwauStatsProfiles* out);
void XwauStatsProfiles_Free(XwauStatsProfiles* profiles);
/* Writes the `stats_profiles` group of a model file. */
void XwauStatsProfiles_Write(FILE* f, const XwauStatsProfiles* profiles);

/* A mission's choices. */
typedef struct XwauMissionStats {
	struct XwauMissionStatsModel {
		char key[256];
		char player[XWAU_STATS_NAME_MAX];
		char all[XWAU_STATS_NAME_MAX];
		bool has_markings[256];
		char markings[256][XWAU_STATS_NAME_MAX];
	}* models;

	size_t    model_count;
	long long percent[XWAU_STATS_PERCENT_FIELDS]; /* -1 when not set */
	long long player_percent[XWAU_STATS_PERCENT_FIELDS];
} XwauMissionStats;

/* Reads the choices of mission `base` ("Missions/1b2m5g") of `game`.
 * Returns false when out of memory. */
bool XwauMissionStats_Read(const char* game, const char* base, XwauMissionStats* out);
bool XwauMissionStats_Any(const XwauMissionStats* stats);
void XwauMissionStats_Write(FILE* f, const XwauMissionStats* stats);
void XwauMissionStats_Free(XwauMissionStats* stats);

#endif /* AERON_XWAU_STATS_PROFILES_H */
