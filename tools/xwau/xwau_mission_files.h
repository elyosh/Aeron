#ifndef AERON_XWAU_MISSION_FILES_H
#define AERON_XWAU_MISSION_FILES_H

/*
 * Writes the selections of a mod's missions. Each mission with at least one
 * selection gets a file <out>/<dir>/<mission>.yaml (lower case). The game
 * reads it when it loads the mission:
 *
 *     version: 1
 *     resdata: ["planet2", "planetcustom", "backdrop_special"]
 *     object_profiles:
 *       flight_groups:
 *         32: { profile: "Aeron", name: "Aeron", craft: 14 }
 *     skins:
 *       "awing":
 *         0: "0|default_2,ace"
 *     articulations:
 *       flight_groups:
 *         13: { name: "Red", craft: 1, variant: "1b0m7fw_fg13", start: [sfoils] }
 *
 * The missions come from XwauMissions_Names, in Missions/, Melee/ and
 * Combat/.
 *
 * Object profiles (hook_mission_objects): a line
 * `ObjectProfile_fg_<N> = <profile>` sets the profile of flight group N.
 * The lines are read from <mission>_Objects.txt, or else from the [Objects]
 * section of <mission>.ini, with the hook's C++ rules: when a flight group
 * has several lines, the first one is used. Other ObjectProfile_* keys
 * select a profile by model. They are reported but not imported, and
 * XWAU's data has none.
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
 * Art (hook_backdrops): `resdata` lists the DATs that the mission adds, in
 * its order: the lines of <mission>_Resdata.txt, or else of the [Resdata]
 * section of <mission>.ini. A DAT that doesn't exist is left out, as XWAU
 * skips it. Each one is written as the game names its set: its file name
 * without the extension, in lower case, with any character other than a
 * letter, a digit, '_' or '-' made '_'. The bake decides which of them are
 * sets (resdata/sets/<name>); the game uses those and ignores the others.
 *
 * The selections name flight groups by their position in the mission's .TIE,
 * and they are written with the same position. The .TIE is the one that
 * plays: the mod's own when it has one (xwau_game_files.h copies it into the
 * mod folder), else the game's. `name` and `craft` are that flight group's
 * values, and the game checks them again. A selection for a flight group that
 * the .TIE doesn't have is reported and dropped, as XWAU ignores it.
 *
 * Text is written as UTF-8. Problems go to stderr.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_missions.h"

/* Writes the mission files under `out`. `stock_game` is the stock game's
 * folder, which has the .TIE of the missions that the mod doesn't bring.
 * Returns false on an error. */
bool XwauMissionFiles_Write(const char* game, const XwauMissions* missions, const char* stock_game,
							const char* out);

#endif /* AERON_XWAU_MISSION_FILES_H */
