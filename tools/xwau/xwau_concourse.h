#ifndef AERON_XWAU_CONCOURSE_H
#define AERON_XWAU_CONCOURSE_H

/*
 * The front planet of XWAU's hook_concourse, as the `concourse` section of
 * the game's mission files (concourse_planet.h in the game):
 *
 *     concourse:
 *       front_planet: { x: 319, y: 78, image: 15010 }
 *
 * hook_concourse reads three keys from a mission's <mission>_concourse.txt,
 * else from the [concourse] section of <mission>.ini:
 *   - FrontPlanetIndex, written as `image`: the image group of the planet,
 *     such as 15010;
 *   - FrontPlanetPositionX and FrontPlanetPositionY, written as `x` and
 *     `y`: the planet's centre in 640x480 units.
 * -1 means "not set", and a key that isn't set isn't written. As in the
 * hook, a key is found in any case, the first line with it wins, and values
 * are read as std::stoi(value, 0, 0) reads them. A value that isn't a number
 * stops XWAU; it is reported and left out.
 *
 * Without hook_concourse.dll, there is no front planet.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A mission's front planet. -1 means "not set". */
typedef struct XwauFrontPlanet {
	int image, x, y;
} XwauFrontPlanet;

/* Reads the front planet of mission `base` (such as "Missions/1b7m2w") of
 * the installation `game`. Problems are reported with `base` as their
 * label. Returns false when out of memory. */
bool XwauFrontPlanet_Read(const char* game, const char* base, XwauFrontPlanet* out);

/* Whether the front planet writes anything. */
bool XwauFrontPlanet_Any(const XwauFrontPlanet* planet);

/* Writes the `concourse` section, when there is anything to write. */
void XwauFrontPlanet_Write(FILE* f, const XwauFrontPlanet* planet);

/* The image groups of the DATs that the game loads: those of the mod's
 * RESDATA.TXT, else of the stock game's. A DAT is found in the mod, else in
 * the stock game. */
typedef struct XwauDatGroups {
	int*   ids;
	size_t count;
} XwauDatGroups;

/* Reads the groups. A DAT that can't be read is reported and skipped.
 * Returns false when out of memory. */
bool XwauDatGroups_Read(const char* game, const char* stock_game, XwauDatGroups* out);
bool XwauDatGroups_Has(const XwauDatGroups* groups, int group);
void XwauDatGroups_Free(XwauDatGroups* groups);

#endif /* AERON_XWAU_CONCOURSE_H */
