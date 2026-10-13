#ifndef AERON_OPT2GLTF_XWAU_MISSION_FILES_H
#define AERON_OPT2GLTF_XWAU_MISSION_FILES_H

/*
 * Writes the selections of XWAU's missions for the original game's missions.
 * Each mission with at least one selection gets a file
 * <out>/<dir>/<mission>.yaml (lower case). The game reads it when it loads
 * the mission:
 *
 *     version: 1
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
 * The missions come from XwauMissions_Names, first Missions/ then Melee/.
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
 * variant, or `~` for the model as modelled. Skin lines do not need flight
 * group matching, but they assume that a flight group keeps its markings.
 * Flight groups whose markings differ between XWAU's mission and the
 * original one are reported.
 *
 * Articulations (hook_sfoils): the start lines and variants of the flight
 * groups (xwau_missions.h).
 *
 * XWAU's missions add, remove, rename flight groups and change their craft,
 * so XWAU's flight group indices can differ from the original mission's.
 * A selection is written for the original flight group with the same name
 * (ignoring trailing white space) and the same craft type. This is the
 * flight group at the same index if it matches. Otherwise it is the only
 * matching flight group, when XWAU's mission also has only one. Other
 * selections are reported and dropped. The key is the original flight
 * group's index. `name` and `craft` are the original flight group's
 * values, and the game checks them again.
 *
 * Text is written as UTF-8. Problems go to stderr.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_missions.h"

/* Writes the mission files under `out`. `stock_game` is the original game
 * installation, whose Missions/ and Melee/ .TIE files give the original
 * flight groups. Returns false on an error. */
bool XwauMissionFiles_Write(const char* game, const XwauMissions* missions, const char* stock_game,
							const char* out);

#endif /* AERON_OPT2GLTF_XWAU_MISSION_FILES_H */
