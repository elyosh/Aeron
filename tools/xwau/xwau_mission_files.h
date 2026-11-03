#ifndef AERON_XWAU_MISSION_FILES_H
#define AERON_XWAU_MISSION_FILES_H

/*
 * Writes the selections of a mod's missions. Each mission with at least one
 * selection gets a file <out>/<dir>/<mission>.yaml (lower case). The game
 * reads it when it loads the mission. The file holds the mission's art,
 * settings, model swaps, craft names, concourse planet, headlights, stats
 * profiles, flight-group values, object profiles, skins and
 * articulations.
 *
 * A `craft` check is the mod's object type name for the flight group's
 * craft type.
 *
 * The missions come from XwauMissions_Names, in Missions/, Melee/ and
 * Combat/, and from their MISSION.LST files, since some settings have
 * defaults by mission number.
 *
 * Object profiles (hook_mission_objects): a line
 * `ObjectProfile_fg_<N> = <profile>` sets the profile of flight group N.
 * The lines are read from <mission>_Objects.txt, or else from the [Objects]
 * section of <mission>.ini, with the hook's C++ rules: when a flight group
 * has several lines, the first one is used. Other ObjectProfile_* keys
 * select a profile by model. The importer doesn't import them, and counts
 * them as unsupported. XWAU's own data has none.
 *
 * Skins (hook_32bpp): only missions that have their own skin lines
 * (XwauMissions_OwnSkinLines) get skins. For each model that has skins (the
 * OPTs in FlightModels), the skins are resolved the same way as during the
 * conversion (xwau_skins.h). A markings value is written only when its
 * variant is not the model's default one, which comes from the global
 * context. It goes under the model's name in lower case. The value is the
 * variant, or `~` for the model as modelled.
 *
 * Articulations (hook_sfoils): the start lines and variants of the flight
 * groups (xwau_missions.h).
 *
 * Flight-group values (hook_mission_tie): `flight_groups` holds the
 * markings, IFF and pilot voice that the mission's [mission_tie] lines set
 * (xwau_mission_tie.h), by position, as `markings`, `iff` and `pilot_voice`.
 * `craft_markings` holds the markings of single craft, keyed by the craft's
 * number in its wave, from 0. A value that doesn't fit in a byte is cut to
 * one, as XWAU does. A craft number past the flight group's craft count is
 * kept, since a skirmish can add craft. A flight group whose
 * name isn't ASCII gets no `name` check, since the game compares the .TIE's
 * bytes and the file is UTF-8.
 *
 * Warhead pools (hook_weapon_rate): `warheads` in a flight group's entry
 * holds the warheads that the flight group has in its hangar
 * (xwau_warhead_pools.h). The pools of a flight group that the .TIE doesn't
 * have are dropped.
 * Weapon profiles (hook_weapon_rate): `weapon_profile`, `warhead_profile`,
 * `energy_profile` and `linking_profile` in a flight group's entry pick the
 * flight group's profiles. When the model of the flight group's craft
 * doesn't have the profile, after the mission's swaps, it is reported and
 * kept.
 *
 * Art (hook_backdrops): `resdata` lists the DATs that the mission adds, in
 * its order: the lines of <mission>_Resdata.txt, or else of the [Resdata]
 * section of <mission>.ini. A DAT that doesn't exist is left out, as XWAU
 * skips it. A DAT whose name is too long for the game, and two
 * DATs that get the same art name, make the import fail. Each one is
 * written as the game names its mission art: its file
 * name without the extension, in lower case, with any character other than a
 * letter, a digit, '_' or '-' made '_'. The bake decides which of them get
 * their own mission art (resdata/mission_art/<name>); the game uses those
 * and ignores the others, which are in the shared atlases.
 *
 * Model swaps (hook_mission_objects): a line
 * `FlightModels\<A>.opt = FlightModels\<B>.opt` of the [Objects] lines
 * loads model B wherever the game loads A, in flight and in the briefing. As
 * in the hook, the first line for a path wins, in any case, and a swap whose
 * B doesn't exist is dropped. OpenXWA also needs B's GLB in the mod folder,
 * next to the output folder (<out>/../models/<b>.glb). A swap without it
 * makes the import fail. Both are written as model keys. A swap to or from a
 * model with [ExeCraftValues] is kept, and counted as unsupported, because
 * XWAU then gives the craft other values in that mission, and the object
 * type files can't (xwau_objects.h).
 *
 * Craft names (hook_mission_tie): the lines `craft, N, <key>, <value>` of
 * the [mission_tie] lines rename the craft of record N for the mission:
 * `name` (the ship list name), `specname`, `shortname`, `pluralname` and
 * `mapicon`, written as `ship_list`, `name`, `short_name`, `plural` and
 * `map_icon`. The last line for a value wins, as in the hook's flight. They
 * are written by model: the list row of the first object type that uses
 * record N in XWAU's tables. Names that aren't ASCII are counted as
 * unsupported, since the game may show them differently. Two used records with the same model, and a model
 * whose first and last name lines differ (XWAU shows the first in the briefing), make the import fail,
 * because the mission file holds one set of names for each model.
 *
 * Settings (hook_mission_tie, and hook_engine_sound's [Interdiction]):
 * `settings` holds the values that apply in the mission and that differ from
 * what the game does without them (xwau_mission_settings.h).
 *
 * Concourse planet (hook_concourse): `concourse` holds the planet image and
 * position that the mission sets (xwau_concourse.h). An image group that no
 * DAT of RESDATA.TXT has is kept: the concourse then shows no planet, as in
 * XWAU.
 *
 * Headlights (ddraw): `headlights: true` when `Enabled` of the mission's
 * [Headlights] section isn't 0. The light itself is in the `headlights`
 * section of flight/flight.yaml (xwau_flight_settings.h).
 *
 * Stats profiles (hook_mission_tie): the [StatsProfiles] lines, which pick
 * the profiles of the mission's craft by model (xwau_stats_profiles.h).
 *
 * The selections name flight groups by their position in the mission's .TIE,
 * and they are written with the same position. The .TIE is the one that
 * plays: the mod's own when it has one (xwau_game_files.h copies it into the
 * mod folder), else the game's. `name` and `craft` are that flight group's
 * values, and the game checks them again. A selection for a flight group that
 * the .TIE doesn't have is dropped, as XWAU ignores it.
 *
 * Text is written as UTF-8. Problems go to stderr. Laser rates by flight
 * group (hook_weapon_rate) aren't imported, and are counted as unsupported.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_missions.h"
#include "xwau_names.h"

/* Reads the [Objects] lines of mission `base` ("<dir>/<mission>"):
 * <mission>_Objects.txt, or else the [Objects] section of <mission>.ini.
 * Returns false when out of memory. */
bool XwauMissionFiles_ObjectLines(const char* game, const char* base, XwauLines* lines);

/* Writes the mission files under `out`. `stock_game` is the stock game's
 * folder, which has the .TIE files of the missions that the mod doesn't
 * change. The files use the names in `game_names`. `design_scale` is the
 * number of design pixels of the mod's frontend files in one 640x480 unit,
 * across and down. Returns false on an error. */
bool XwauMissionFiles_Write(const char* game, const XwauMissions* missions, const char* stock_game,
							const char* out, const XwauNames* game_names, const int design_scale[2]);

#endif /* AERON_XWAU_MISSION_FILES_H */
