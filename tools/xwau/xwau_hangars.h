#ifndef AERON_XWAU_HANGARS_H
#define AERON_XWAU_HANGARS_H

/*
 * Writes XWAU's hangar settings, which hook_hangars reads at runtime, once
 * and already parsed (the mod importer's hangars phase):
 *
 *     version: 1
 *     default:                  # FlightModels/<Section>.txt, else default.ini [<Section>]
 *       objects: { LoadShuttle: 1, ShuttleModelIndex: 50, ... }
 *       swaps: { hangardroid: "hangardroidr2q5" }
 *       map: [[314, 0, -1759, 813, 2147483647, 25064, 0, "Default", -1], null, ...]
 *       camera: { 1: [1130, -2320, -300], ... }
 *       fam_map: [...]
 *       fam_camera: { 1: [...], ... }
 *     ships:                    # FlightModels/<Ship>.ini, by lower-case name
 *       imperialstardestroyer:
 *         model: imperialstardestroyerhangar
 *         objects: {...}
 *         iff:
 *           1: { objects: {...}, map: [...] }   # [<Section>1], for the command ship's IFF 1
 *     crafts:                   # the player craft's [Camera] and [Size]
 *       xwing: { camera: [[x, y, z], [inverted], [family], [family inverted]], elevation: [e, inverted e] }
 *
 * A layer holds the sections that one source has, and only the ones that
 * aren't empty, as the hook skips empty sections. The game picks, for the
 * command ship and its IFF, the layers mission, then ship IFF, then ship,
 * then default:
 *   - objects and swaps: for each key, the first layer that sets it;
 *   - map, camera, fam_map and fam_camera: the whole section of the first
 *     layer that has it.
 *
 * The files are read as the hook reads them: sections and keys in any case,
 * the first key wins, numbers as std::stoi(value, 0, 0), and lists split on
 * ',' and ';'. A value that isn't a number would stop XWAU, so it is
 * reported and left out. These are resolved here:
 *   - XWAU's model indices, which the installation's XWINGALLIANCE.EXE binds
 *     to list rows. Each one becomes the OpenXWA object type that loads the
 *     same model: the same index when OpenXWA's model there is XWAU's, else
 *     the one type that loads it. OpenXWA's model of a type is the `model`
 *     of its object type file, else the model of its row in the stock game.
 *     An index whose model has no OpenXWA type, such as one of XWAU's added
 *     rows, makes the import fail. An index that loads no model in XWAU
 *     either is reported: a map line becomes null, so that the lines keep
 *     their places, and a key is left out;
 *   - a swap "FlightModels\<A>.opt = FlightModels\<B>.opt" becomes "a: b"
 *     when B is in the installation and was converted. The hook ignores
 *     swaps to <A>Exterior.opt;
 *   - a map line has 6 to 9 fields: the model, the markings, x, y, z, the
 *     heading in XY and in Z, the profile ("Default" when missing) and
 *     whether it is upside down (-1 when missing). Other lines become null;
 *   - a camera section gets every key that the hook reads, 0 when missing:
 *     Key1/2/3/6/9 for camera, FamKey1/2/3/6/7/8/9 for fam_camera;
 *   - [Camera] falls back as the hook does, from a triple that is all zero:
 *     X/Y/Z; InvertedX/Y/Z, then X/Y/Z; FamX/Y/Z, then X/Y/Z; FamInverted,
 *     Fam, Inverted, then X/Y/Z;
 *   - [Size] gives ClosedSFoilsElevation (0 when missing) and
 *     ClosedSFoilsElevationInverted (ClosedSFoilsElevation when 0).
 * Unknown HangarObjects keys, lines without '=' and repeated keys are
 * reported and left out. LightColorIntensity and LightColorRgb make the
 * import fail, because the remaster lights the hangar itself. So do a swap
 * to a model that wasn't converted, a hangar model that wasn't converted,
 * the per-ship mission sections [<Section>_<ship>] and a mission's own
 * hangar model, <mission>_Hangar.opt.
 *
 * With the stock game, each mission's own layer, from [<Section>] of
 * <mission>.ini or <mission>_<Section>.txt, becomes the `hangar` of its
 * mission file, flight/missions/<dir>/<mission>.yaml, which is created when
 * needed. Its `ships` names the ship whose settings and hangar model apply
 * instead of a mothership that the mission's [Objects] lines swap:
 *
 *     hangar:
 *       objects: { Droid1Markings: 2 }
 *       map: [...]
 *       ships: { frigate2standard: frigate2carrier }
 */

#include <stdbool.h>

#include "xwau_game_tables.h"
#include "xwau_names.h"

/* Writes <mod>/flight/hangars.yaml for the installation `xwau`, and the
 * missions' layers when `stock_game` is set. `stock` holds the stock game's
 * tables. Uses the converted models in <mod>/flight/models and the object
 * type files in <mod>/flight/object_types. `game_names` converts the object
 * type names in those files. Returns false when a file can't be read or
 * written. */
bool XwauHangars_Write(const char* xwau, const XwauGameTables* stock, const XwauNames* game_names,
					   const char* stock_game, const char* mod);

#endif /* AERON_XWAU_HANGARS_H */
