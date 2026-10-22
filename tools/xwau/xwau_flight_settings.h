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
 *       classic_hide_destroyed_engine_glows: true
 *       weapon_iff_swap: false
 *       ai_attack_uses_max_speed_cache: true
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
 *     weapons:
 *       impact: {280: 0xff0000, 281: 0xff0000, ...}
 *     shield_recharge:
 *       starships: true
 *       starships_on_easy: false
 *       double_starfighter_shields: false
 *       rate_percent: 100
 *
 * `extra_planets` is written when the mod installs XWAU's backdrop hook,
 * hook_backdrops.dll. With it, a mission's backdrop id N from 104 to 255
 * draws the planet of image group 6304 + (N - 104), and the mod's missions
 * use these ids (mission_backdrops.h in the game). XWAU's hook loader loads
 * every hook_*.dll of the game folder, and this hook has no setting that
 * turns this off.
 *
 * `rules` holds the rules that hooks change in every mission:
 * hook_mission_tie.dll's two, hook_opt_limit.dll's engine glow rule
 * (xwau_mission_settings.h), and hook_weapon_color.dll's weapon_iff_swap,
 * its global WeaponSwitchBasedOnIff. With the hook, a starship fires the
 * weapons of its craft record, whatever its IFF, unless the setting or its
 * model's switch_by_iff turns the swap to its side's weapons on
 * (xwau_craft.h). hook_mission_tie.dll with stats profiles gives
 * ai_attack_uses_max_speed_cache: a craft under attack compares its own top
 * speed and its attacker's, which their stats profiles can change. `mission_settings` holds the
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
 *
 * `weapons.impact` is written when the mod installs hook_weapon_color.dll,
 * which tints every impact mark: the colour of each weapon type 280 to 307,
 * from the hook's five group colours (XwauCraft_ReadGlobalImpactColors).
 *
 * `shield_recharge` is written when the mod installs hook_shield.dll. Its
 * values come from hook_shield.cfg, else the [hook_shield] section of
 * Hooks.ini: IsShieldRechargeForStarshipsEnabled (default 1),
 * IsShieldRechargeForStarshipsOnEasyDifficultyEnabled (0),
 * IsShieldStrengthForStarfighterDoubled (0) and RechargeRatePercent (100).
 * The rules are in shield_recharge.h in the game. The hook's per-mission
 * [Shield] lines (<mission>_Shield.txt), which no mod uses, aren't
 * imported.
 */

#include <stdbool.h>

/* Writes the settings of the installation `game` into the file `path`.
 * Returns false on an error. */
bool XwauFlightSettings_Write(const char* game, const char* path);

#endif /* AERON_XWAU_FLIGHT_SETTINGS_H */
