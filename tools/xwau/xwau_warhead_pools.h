#ifndef AERON_XWAU_WARHEAD_POOLS_H
#define AERON_XWAU_WARHEAD_POOLS_H

/*
 * Reads the warhead pools of a mission, which hook_weapon_rate uses. They
 * are in <mission>_WarheadTypeCount.txt, or else in the [WarheadTypeCount]
 * section of <mission>.ini. Without either, the hook uses
 * FlightModels/WarheadTypeCount.txt, or else the [WarheadTypeCount] section
 * of FlightModels/default.ini, for every mission. A line gives the warheads
 * of one type that a flight group has in its hangar for the whole mission:
 *
 *   WarheadTypeCount_fg_0_Missiles = 80
 *
 * The types are SpaceBombs, HeavyRockets, Missiles, ProtonTorpedos,
 * AdvancedMissiles, AdvancedTorpedos, MagPulse, IonPulse, AdvancedMagPulse
 * and ClusterBombs. Keys can be written in any case, and the first line of a
 * key wins. -1 means no limit, so it isn't written. Other values below 0
 * would give a craft a wrong number of warheads; they are reported and left
 * out. Lines with other keys, such as TFTC's ..._MagPulseCount, are
 * reported.
 *
 * The values go into the flight group's entry of the mission file:
 *
 *   flight_groups:
 *     0: { name: "Alpha", craft: 5, warheads: { missiles: 80 } }
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum { XWAU_WARHEAD_TYPES = 10 };

/* The pools of one flight group. -1 means no limit. */
typedef struct XwauWarheadPool {
	int fg;
	int count[XWAU_WARHEAD_TYPES];
} XwauWarheadPool;

typedef struct XwauWarheadPools {
	XwauWarheadPool* items; /* sorted by flight group */
	size_t           count;
} XwauWarheadPools;

/* Reads the warhead pools of mission `base` ("Missions/1b13m2tftc") of the
 * installation `game`, when its hook_weapon_rate.dll has them. With
 * `report`, the lines left out are reported. Returns false when out of
 * memory. */
bool XwauWarheadPools_Read(const char* game, const char* base, bool report, XwauWarheadPools* out);
void XwauWarheadPools_Free(XwauWarheadPools* pools);

/* The pools of flight group `fg`, or NULL. */
const XwauWarheadPool* XwauWarheadPools_Find(const XwauWarheadPools* pools, int fg);

/* Writes ", warheads: {missiles: 80}", the entry's key, when `pool` sets a
 * value. */
void XwauWarheadPools_Write(FILE* f, const XwauWarheadPool* pool);

/* Whether `pool` sets a value. */
bool XwauWarheadPools_Any(const XwauWarheadPool* pool);

#endif /* AERON_XWAU_WARHEAD_POOLS_H */
