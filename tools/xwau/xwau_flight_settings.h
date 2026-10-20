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
 *     rules:
 *       reset_combat_music_at_mission_load: true
 *       hangar_red_alert_any_mothership: true
 *     mission_settings:
 *       warhead_collision_damage: false
 *       damage_through_shields_on_hard: true
 *       interdiction_regions: [1]
 *     headlights:
 *       position_m: [0, 0, 20]
 *       color: [0.944, 0.944, 1]
 *       power_w: 17671
 *       angle_degrees: 25
 *       auto_turn_on: true
 *
 * `extra_planets` is written when the mod installs XWAU's backdrop hook,
 * hook_backdrops.dll. With it, a mission's backdrop id N from 104 to 255
 * draws the planet of image group 6304 + (N - 104), and the mod's missions
 * use these ids (mission_backdrops.h in the game). XWAU's hook loader loads
 * every hook_*.dll of the game folder, and this hook has no setting that
 * turns this off.
 *
 * `rules` is written when the mod installs hook_mission_tie.dll, which
 * changes these two things in every mission. `mission_settings` holds the
 * defaults that the global settings of hook_mission_tie and
 * hook_engine_sound give every mission (xwau_mission_settings.h).
 *
 * `headlights` is written when the mod has XWAU's ddraw.dll and its SSAO.cfg
 * sets `ssao_enabled = 1`: ddraw draws the headlights only then. It comes
 * from the `headlights_*` keys of SSAO.cfg. The light is a one-sided light
 * along the craft's nose, with a colour and a power in watts as a Blender
 * area light has them (xwau_flight_settings.c says how ddraw's values are
 * converted). `position_m` is from the craft's eye, with Z forward.
 * `angle_degrees` is the width of ddraw's cone, for when the game has cone
 * lights. `auto_turn_on` turns them on in the missions without a global
 * light. A mission turns them on with `headlights: true` in its mission file
 * (xwau_mission_files.h).
 *
 * Without any of these hooks, the file only has its version, and the game
 * plays as stock.
 */

#include <stdbool.h>

/* Writes the settings of the installation `game` into the file `path`.
 * Returns false on an error. */
bool XwauFlightSettings_Write(const char* game, const char* path);

#endif /* AERON_XWAU_FLIGHT_SETTINGS_H */
