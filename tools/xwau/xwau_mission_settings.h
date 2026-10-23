#ifndef AERON_XWAU_MISSION_SETTINGS_H
#define AERON_XWAU_MISSION_SETTINGS_H

/*
 * The mission settings of XWAU's hook_mission_tie and the [Interdiction]
 * regions of hook_engine_sound, as the game's mission settings
 * (mission_settings.h in the game).
 *
 * hook_mission_tie reads `key = value` lines from a mission's [mission_tie]
 * lines (xwau_mission_tie.h). Each key has a default, and some defaults
 * depend on the mission's number in its MISSION.LST (20, 49 to 52, 1) or on
 * the hook's global settings: hook_mission_tie.cfg, else the
 * [hook_mission_tie] section of Hooks.ini. The importer works out the value
 * that applies in each mission, and writes it when it differs from what the
 * game does without it:
 *   - the stock rules that the game ties to mission numbers, for
 *     red_alert, skip_hyperspaced_messages, skip_arrival_messages_iff and
 *     force_player_in_turret;
 *   - the defaults of flight/flight.yaml for warhead_collision_damage,
 *     damage_through_shields_on_hard and interdiction_regions, which come
 *     from the global settings (XwauMissionSettings_WriteDefaults).
 *
 * As in the hook, a key is found in any case, the first line with it wins,
 * and numbers are read as std::stoi reads them. A value that isn't a number
 * stops XWAU (std::stoi throws); it is reported, and the default is used.
 *
 * Without hook_mission_tie.dll, there are no settings, and without
 * hook_engine_sound.dll, no interdiction regions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "xwau_config.h"

/* The global settings of the mod's hooks. */
typedef struct XwauSettingsConfig {
	bool mission_tie;  /* hook_mission_tie.dll */
	bool engine_sound; /* hook_engine_sound.dll */
	/* hook_opt_limit.dll, with its [EngineGlows] feature: the hook also
	 * hides the glows of destroyed meshes. */
	bool      opt_limit_engine_glows;
	/* hook_weapon_color.dll, with WeaponSwitchBasedOnIff, and its global
	 * value. */
	bool      weapon_switch;
	bool      weapon_switch_by_iff;
	/* hook_mission_tie.dll with stats profiles: a craft under attack compares
	 * the craft's own top speeds. */
	bool      mission_tie_stats;
	/* hook_weapon_rate.dll with weapon profiles: a base laser fires only
	 * with the decharge rate. */
	bool weapon_rate_fire_check;
	/* hook_weapon_rate.dll that reads mesh states: starships don't fire
	 * from destroyed racks. */
	bool      weapon_rate_rack_check;
	/* hook_weapon_rate.dll with warhead pools, which also lifts the cap of 9
	 * warheads in a player's rack. */
	bool weapon_rate_slot_cap;
	/* hook_weapon_rate.dll with its EnableFireMeshFilter setting: no craft
	 * fires from a destroyed mesh. */
	bool weapon_rate_mesh_filter;
	/* hook_weapon_rate.dll with hardpoint types: hardpoint types 33 to 38 are
	 * turbolasers. */
	bool weapon_rate_hardpoint_types;
	/* hook_weapon_rate.dll that reads both warhead statuses when a craft is
	 * rearmed. */
	bool weapon_rate_rearm_statuses;
	/* hook_mission_objects.dll with gunner seats, and its
	 * UnlimitedTurretLaser setting. */
	bool gunner_seats;
	bool gunner_selected_group;
	/* hook_engine_sound.dll with starship ambient sounds, and its
	 * AreStarshipAmbientEnabled setting. */
	bool      starship_ambient;
	bool      warhead_collision_damage;
	bool      damage_through_shields;
	long long target_method;
	bool      target_only_not_inspected;
	/* The regions of FlightModels/Interdiction.txt, else of the
	 * [Interdiction] section of FlightModels/default.ini. */
	int*   interdiction;
	size_t interdiction_count;
} XwauSettingsConfig;

/* Reads the global settings of the installation `game`. Returns false when
 * out of memory. */
bool XwauSettingsConfig_Read(const char* game, XwauSettingsConfig* out);
void XwauSettingsConfig_Free(XwauSettingsConfig* config);

/* Writes the `rules` and `mission_settings` sections of flight/flight.yaml.
 * The rules are hook_mission_tie's, hook_opt_limit's
 * classic_hide_destroyed_engine_glows, hook_weapon_color's
 * weapon_iff_swap, hook_mission_tie's ai_attack_uses_max_speed_cache and
 * hook_weapon_rate's fire_needs_decharge_rate,
 * no_starship_fire_from_destroyed_racks, no_player_warhead_slot_cap and
 * weapon rules,
 * hook_engine_sound's starship_ambient_sounds, and hook_mission_objects'
 * gunner_weapon_select_in_seat, gunner_fires_warheads and, with
 * UnlimitedTurretLaser = 1, gunner_fires_selected_group. Returns whether it
 * wrote anything.
 */
bool XwauMissionSettings_WriteDefaults(FILE* f, const XwauSettingsConfig* config);

/* The numbers of the missions of MISSION.LST in Missions/, Melee/ and
 * Combat/: the mod's lists, else the stock game's. */
typedef struct XwauMissionNumbers {
	struct XwauMissionNumber {
		char dir[16];
		char name[64]; /* lower case, without ".tie" */
		int  number;
	}* items;

	size_t count;
} XwauMissionNumbers;

bool XwauMissionNumbers_Read(const char* game, const char* stock_game, XwauMissionNumbers* out);
void XwauMissionNumbers_Free(XwauMissionNumbers* numbers);
/* The number of mission `name` (lower case) of `dir`, or -1. */
int XwauMissionNumbers_Find(const XwauMissionNumbers* numbers, const char* dir, const char* name);

/* A mission's settings, as the importer writes them. -1 means "not
 * written" for the numbers, and the lists are written when they have
 * anything. */
typedef struct XwauMissionSettings {
	int    red_alert, skip_hyperspaced_messages;
	int    skip_arrival_iff; /* an IFF from 0 to 254, 255 for all, 256 for none */
	int    force_turret;     /* 0: false; 1: at turret_time */
	char   turret_time[16];
	bool   disable_lasers, disable_warheads, no_warhead_collision_damage, skip_projectiles;
	int    damage_through_shields;
	bool   target_key, target_cycle, target_only_not_inspected;
	int*   target_fgs;
	size_t target_fg_count;
	bool   order_list; /* the mission lists flight groups */
	int*   order_fgs;  /* order_of_battle: flight_groups */
	size_t order_fg_count;

	struct XwauOrderName {
		int   fg;
		char* text;
	}* order_names;

	size_t    order_name_count;
	XwauLines order_lines;
	int       order_top;
	bool      interdiction; /* the mission has its own regions */
	int*      interdiction_regions;
	size_t    interdiction_count;
} XwauMissionSettings;

/* Reads the settings of mission `base` (such as "Missions/1b7m2w") of the
 * installation `game`, with number `number` (-1 when it has none), whose
 * [mission_tie] `key = value` lines are `lines`. Problems are reported with
 * `base` as their label. Returns false when out of memory. */
bool XwauMissionSettings_Read(const char* game, const char* base, int number, const XwauLines* lines,
							  const XwauSettingsConfig* config, XwauMissionSettings* out);
void XwauMissionSettings_Free(XwauMissionSettings* settings);

/* Whether the settings write anything. */
bool XwauMissionSettings_Any(const XwauMissionSettings* settings);

/* Drops, with a report, the flight groups past the end of the .TIE that
 * plays, which has `fg_count` of them. */
void XwauMissionSettings_CheckFlightGroups(XwauMissionSettings* settings, size_t fg_count, const char* base);

/* Writes the `settings` section, when there is anything to write. */
void XwauMissionSettings_Write(FILE* f, const XwauMissionSettings* settings);

#endif /* AERON_XWAU_MISSION_SETTINGS_H */
