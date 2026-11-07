#ifndef AERON_XWAU_FLIGHT_SETTINGS_H
#define AERON_XWAU_FLIGHT_SETTINGS_H

/*
 * Writes flight/flight.yaml, the settings of a mod that apply to every
 * mission (the mod importer's missions phase). They come from the hooks and
 * the ddraw that the mod installs, and from their settings files (the
 * hook's .cfg file, else its section of Hooks.ini). Without any of them, the
 * file only has its header comment, and the game plays as stock.
 *
 * The groups:
 *   - `backdrops.extra_planets` comes with hook_backdrops.dll. A backdrop id
 *     N from 104 to 255 then draws the planet of image group
 *     6304 + (N - 104).
 *   - `rules` holds the rules that the hooks change in every mission. Most
 *     rules come from a hook DLL alone. Some depend on a setting of the hook,
 *     or on a text in the DLL that shows which build it is
 *     (xwau_mission_settings.c).
 *   - `mission_settings` holds the defaults that the global settings of
 *     hook_mission_tie and hook_engine_sound give every mission
 *     (xwau_mission_settings.h).
 *   - `headlights` comes from the headlights_* keys of SSAO.cfg, when ddraw
 *     has SSAO on, since it only draws them then. The light is one-sided,
 *     along the craft's nose, with a colour and a power in watts as a Blender
 *     area light has them. `position_m` is from the craft's eye, with Z
 *     forward. A mission turns the lights on with `headlights: true`
 *     (xwau_mission_files.h).
 *   - `weapons` holds the impact colours of hook_weapon_color, the global
 *     laser rates and weapon profiles of hook_weapon_rate (xwau_craft.h and
 *     xwau_weapon_stats.h), and the hook's factors by rank and difficulty.
 *     The factors are float32 values, written with enough digits to get the
 *     exact value back. WeaponsCount goes into the object counts
 *     (xwau_object_limits.h).
 *   - `shield_recharge` comes with hook_shield.dll. `docking` comes with
 *     hook_dock.dll, which changes how craft dock without any data.
 *   - `ai_attack`, `mag_pulse` and `warhead_lock` come with the global
 *     sections of hook_weapon_lock.dll.
 *   - `camera` comes with Hook_CockpitLook.dll or hook_exterior.dll, and
 *     turns on the game's enhanced camera. Its values are converted from
 *     CockpitLook.cfg, with the hook's defaults for the keys it doesn't set.
 *     With hook_exterior.dll alone, it turns the HUD on and the lag, the tilt
 *     and the sway off.
 *   - `suns` comes with a ddraw build that draws procedural suns. ddraw.dll
 *     can be a loader of DDraw_Effects.dll or DDraw_Golden.dll, as
 *     DDrawType.cfg says. The flare is on when ddraw has its post effects on
 *     and a flare intensity above 0.01.
 */

#include <stdbool.h>

#include "xwau_game_tables.h"
#include "xwau_names.h"

/* Writes the settings of the installation `game` to the file `path`, with
 * the names in `names`. The weapon profiles only write the hardpoint kinds
 * that differ from `stock`. Returns false on an error. */
bool XwauFlightSettings_Write(const char* game, const XwauNames* names, const XwauStockValues* stock,
							  const char* path);

#endif /* AERON_XWAU_FLIGHT_SETTINGS_H */
