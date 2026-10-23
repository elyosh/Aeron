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
 *       fire_needs_decharge_rate: true
 *       no_starship_fire_from_destroyed_racks: true
 *       starship_ambient_sounds: true
 *       no_player_warhead_slot_cap: true
 *       gunner_weapon_select_in_seat: true
 *       gunner_fires_warheads: true
 *       gunner_fires_selected_group: true
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
 *       rates: {decharge: 4, cooldown: 47}
 *       energy_transfer: {rate: 8}
 *       ai_torpedoes: {per_pass: 2, per_target: 6}
 *       impact_spin: {enabled: false}
 *       decharge_percent: 100
 *       recharge_percent: 100
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
 * speed and its attacker's, which their stats profiles can change.
 * hook_weapon_rate.dll gives fire_needs_decharge_rate if it has weapon
 * profiles (it contains the text "_EnergyLowHighSeparation"). With this
 * rule, a craft only fires a base laser when the rack's charge is at least
 * its decharge rate. The DLL gives no_starship_fire_from_destroyed_racks if
 * it reads the craft's mesh states (it contains "Craft_Offset_292"). With
 * this rule, a freighter, starship, platform or satellite doesn't fire from
 * a rack whose mesh is destroyed. The DLL gives no_player_warhead_slot_cap
 * if it has warhead pools (it contains "WarheadTypeCount_fg_"): the hook then
 * lets a player's warhead rack hold more than 9 warheads, with or without
 * pools. hook_mission_objects.dll gives gunner_weapon_select_in_seat and
 * gunner_fires_warheads if it has gunner seats (it contains "Turrets.txt"),
 * and gunner_fires_selected_group when its UnlimitedTurretLaser setting
 * isn't 0 (hook_mission_objects.cfg, else Hooks.ini; 1 by default). With
 * these rules, the weapon key works in a seat, a seat fires warheads when
 * its cockpit has a warhead hardpoint, and a seat fires one laser group.
 * hook_engine_sound.dll gives
 * starship_ambient_sounds if it has ambient sounds (it contains
 * "AreStarshipAmbientEnabled") and that setting isn't 0. With this rule,
 * starships play their ambient sounds from the StarshipAmbient bank.
 * `mission_settings` holds the defaults that the global settings of
 * hook_mission_tie and hook_engine_sound give every mission
 * (xwau_mission_settings.h).
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
 * The laser rates are written when the mod installs hook_weapon_rate.dll.
 * `weapons.rates` holds the rates for every craft. They come from
 * FlightModels/WeaponRate.txt, or else from the [WeaponRate] section of
 * FlightModels/default.ini (XwauCraft_ReadGlobalWeaponRates). If they set
 * no rate, `rates` is left out. The other keys of that section go into
 * `energy_transfer`, `ai_torpedoes`, `impact_spin` and `system_damage`
 * (xwau_craft.h). When the section doesn't set MaxTorpedoCountPerPass or
 * MaxTorpedoCountPerTarget, the hook uses its own settings, from
 * hook_weapon_rate.cfg, or else from the [hook_weapon_rate] section of
 * Hooks.ini; the importer writes those. `decharge_percent` and `recharge_percent`
 * scale the rates of every craft. They come from hook_weapon_rate.cfg, or
 * else from the [hook_weapon_rate] section of Hooks.ini, and default to 100.
 * They are only written if the DLL contains the text "DechargeRatePercent".
 * The hook can also set rates by flight group for each mission
 * ([WeaponRates]). These aren't imported; xwau_import missions reports the
 * missions that have them.
 * The global weapon profiles of hook_weapon_rate also go into `weapons`.
 * They come from the [WeaponStats], [WarheadStats], [EnergyStats],
 * [LinkingStats] and [WeaponHardpointTypes] sections of
 * FlightModels/default.ini. The hook's settings for them come from
 * hook_weapon_rate.cfg, or else from Hooks.ini:
 *   - fire_ratio_by_rank, from NoviceRankFireratioMultiplicator to
 *     SuperAceRankFireratioMultiplicator (4, 3, 2, 2, 1, 1 by default),
 *     when the DLL reads them;
 *   - damage_by_difficulty, from DamagesMultiplicatorForDifficulty0 to 2
 *     (0.5, 0.25, 1), when the DLL reads them;
 *   - limit_linked_warhead_fire_rate, when LimitLinkedWarheadFireRate isn't
 *     0;
 *   - linking_rules, for each of PreventAILasersIonWhenDisabling,
 *     PreventAIIonWhenNotDisabling and PreventPlayerLinkedLasersIon that
 *     isn't 0.
 * The factors are float32 values. They are written with enough digits to
 * get the exact value back. WeaponsCount and IsLaserLinksFixEnabled aren't
 * imported. They are reported when they would change something.
 * When hook_weapon_rate.dll has weapon profiles, it also changes some rules
 * with no data. The importer writes tie_fighter_energy_step_always_high,
 * player_decharge_lasers_only, warhead_collision_bonus_any_target and
 * gun_turret_hardpoints. It writes no_fire_from_destroyed_hardpoints when
 * the hook's EnableFireMeshFilter isn't 0 (1 by default),
 * turbolaser_hardpoint_types when the DLL contains "WeaponHardpointTypes",
 * and rearm_follows_both_statuses when it contains
 * "WarheadCapacity_00460904_Hook", which only TFTC's build does.
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
