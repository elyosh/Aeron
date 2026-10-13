#ifndef AERON_OPT2GLTF_XWAU_MISSION_FILES_H
#define AERON_OPT2GLTF_XWAU_MISSION_FILES_H

/*
 * What XWAU's missions select, for the original game's missions: each
 * mission with selections becomes <out>/<dir>/<mission>.yaml (lower case),
 * which the game reads at mission load:
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
 * The missions are those of XwauMissions_Names, Missions/ then Melee/.
 *
 * Object profiles (hook_mission_objects): `ObjectProfile_fg_<N> = <profile>`
 * lines (<mission>_Objects.txt, else [Objects] of <mission>.ini; C++ rules,
 * the first line of a flight group winning) select a flight group's
 * profile. Other ObjectProfile_* keys (by model) are reported and not
 * imported; XWAU's data has none.
 *
 * Skins (hook_32bpp): for a mission with skin lines of its own
 * (XwauMissions_OwnSkinLines), each model with skins (the OPTs of
 * FlightModels) is resolved as the conversion resolves it (xwau_skins.h), and the markings values whose
 * variant differs from the model's default (the global context's) are written under the model's key (its name
 * in lower case): the variant, or `~` for the model as modelled. Skin lines need no flight-group matching,
 * but they assume a flight group keeps its markings: flight groups whose markings differ between XWAU's
 * mission and the original one are reported.
 *
 * Articulations (hook_sfoils): the flight groups' start lines and variants
 * (xwau_missions.h).
 *
 * Flight group indices are XWAU's missions', which add, remove, rename and
 * re-craft flight groups. A selection is written for the original
 * mission's flight group with the same name (trailing white space aside)
 * and craft type: the one at the same index, else the only such flight
 * group when it is also the only one in XWAU's mission. The rest are
 * reported and dropped. The key is the original flight group's index;
 * `name` and `craft` are its own, which the game checks again.
 *
 * Text is written as UTF-8. Problems go to stderr.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_missions.h"

/* Writes the mission files under `out`, for the original game installed at
 * `stock_game` (its Missions/ and Melee/ .TIE files). False on an error. */
bool XwauMissionFiles_Write(const char* game, const XwauMissions* missions, const char* stock_game,
							const char* out);

#endif /* AERON_OPT2GLTF_XWAU_MISSION_FILES_H */
