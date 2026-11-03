#ifndef AERON_XWAU_CONCOURSE_H
#define AERON_XWAU_CONCOURSE_H

/*
 * Imports the front planet of XWAU's hook_concourse into
 * `concourse.front_planet` in the game's mission files.
 *
 * hook_concourse reads three keys from <mission>_concourse.txt, or else
 * from the [concourse] section of <mission>.ini:
 *   - FrontPlanetIndex becomes `image`, the image group of the planet, such
 *     as 15010.
 *   - FrontPlanetPositionX and FrontPlanetPositionY become `position`, the
 *     centre of the planet. The hook gives it in 640x480 units, and the
 *     importer converts it to design pixels.
 * A value of -1 means that the key isn't set, and the importer doesn't
 * write it. When only one of the two position keys is set, or a value is
 * out of range, the import fails.
 *
 * The importer reads the keys like the hook does: key names can be in any
 * case, the first line with a key wins, and values are read like
 * std::stoi(value, 0, 0). A value that isn't a number makes XWAU stop, so
 * the importer leaves the key out.
 *
 * Without hook_concourse.dll, there is no front planet, and the keys have no
 * effect.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A mission's front planet. An image of -1 means that it isn't set. */
typedef struct XwauFrontPlanet {
	int  image;
	bool has_position;
	int  position[2]; /* in design pixels */
} XwauFrontPlanet;

/* Reads the front planet of mission `base` (such as "Missions/1b7m2w") in
 * the installation `game`. `design_scale` is how many design pixels make one
 * 640x480 unit, across and down. Values that make the import fail are
 * reported with `base` as their label. Returns false when out of memory. */
bool XwauFrontPlanet_Read(const char* game, const char* base, const int design_scale[2],
						  XwauFrontPlanet* out);

/* Whether the front planet writes anything. */
bool XwauFrontPlanet_Any(const XwauFrontPlanet* planet);

/* Writes the `concourse` section, when there is anything to write. */
void XwauFrontPlanet_Write(FILE* f, const XwauFrontPlanet* planet);

#endif /* AERON_XWAU_CONCOURSE_H */
