#ifndef AERON_XWAU_OBJECT_LIMITS_H
#define AERON_XWAU_OBJECT_LIMITS_H

/*
 * Reads the object counts of hook_crafts_count and the WeaponsCount of
 * hook_weapon_rate, as the `object_limits` section of the game's
 * flight/flight.yaml and of a mission's file:
 *
 *   object_limits:
 *     craft_per_region: 3072
 *     player_projectiles: 128
 *     ai_projectiles_per_region: 1024
 *     explosions_per_region: 256
 *     radar_blips: 512
 *
 * hook_crafts_count only changes the game when its settings say
 * IsHookEnabled = 1. They are in hook_crafts_count.cfg, or else in the
 * [hook_crafts_count] section of Hooks.ini. Its CraftsCount,
 * ProjectilesCount, ExplosionsCount and Radar2DItemsCount become
 * craft_per_region, ai_projectiles_per_region, explosions_per_region and
 * radar_blips. A mission can change the first three in
 * <mission>_CraftsCount.txt, or else in the [CraftsCount] section of
 * <mission>.ini, with builds of the hook that read these files. As in the
 * hook, a mission's value of 0 or less keeps the global value.
 *
 * WeaponsCount (12 by default) becomes player_projectiles when it isn't 12.
 * With both hooks, XWAU keeps each player's block at 12 slots but searches
 * WeaponsCount slots, so a player's shots spill into the AI's slots. The game
 * gives each player a block of WeaponsCount slots instead, and the importer
 * notes it in the details.
 *
 * Counts of 0 or less would leave the game without slots, so they are left
 * out.
 */

#include <stdbool.h>
#include <stdio.h>

/* Whether the installation `game` turns hook_crafts_count on. */
bool XwauObjectLimits_HookEnabled(const char* game);

/* Writes the `object_limits` section of flight/flight.yaml. Returns whether
 * it wrote anything. */
bool XwauObjectLimits_Write(FILE* f, const char* game);

/* A mission's counts. 0 means "not written". */
typedef struct XwauMissionLimits {
	long long craft, ai_projectiles, explosions;
} XwauMissionLimits;

/* Reads the counts of mission `base` ("Missions/1b7m3w") of the installation
 * `game`. Returns false when out of memory. */
bool XwauObjectLimits_ReadMission(const char* game, const char* base, XwauMissionLimits* out);

/* Whether `limits` sets a count. */
bool XwauObjectLimits_Any(const XwauMissionLimits* limits);

/* Writes the mission's `object_limits` section, when it sets a count. */
void XwauObjectLimits_WriteMission(FILE* f, const XwauMissionLimits* limits);

#endif /* AERON_XWAU_OBJECT_LIMITS_H */
