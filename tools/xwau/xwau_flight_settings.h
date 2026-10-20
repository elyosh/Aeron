#ifndef AERON_XWAU_FLIGHT_SETTINGS_H
#define AERON_XWAU_FLIGHT_SETTINGS_H

/*
 * Writes flight/flight.yaml, the settings of a mod that apply to every
 * mission (xwau_import missions --flight-settings FILE). They come from the
 * hooks that the mod installs:
 *
 *     # Made by xwau_import missions.
 *     version: 1
 *     backdrops:
 *       extra_planets:
 *         - { ids: [104, 255], first_group: 6304 }
 *
 * `extra_planets` is written when the mod installs XWAU's backdrop hook,
 * hook_backdrops.dll. With it, a mission's backdrop id N from 104 to 255
 * draws the planet of image group 6304 + (N - 104), and the mod's missions
 * use these ids (mission_backdrops.h in the game). XWAU's hook loader loads
 * every hook_*.dll of the game folder, and this hook has no setting that
 * turns this off.
 *
 * Without any of these hooks, the file only has its version, and the game
 * plays as stock.
 */

#include <stdbool.h>

/* Writes the settings of the installation `game` into the file `path`.
 * Returns false on an error. */
bool XwauFlightSettings_Write(const char* game, const char* path);

#endif /* AERON_XWAU_FLIGHT_SETTINGS_H */
