#ifndef AERON_XWAU_HANGARS_H
#define AERON_XWAU_HANGARS_H

/*
 * Writes XWAU's hangar settings, which hook_hangars reads at runtime, to
 * flight/hangars.yaml. This is the hangars phase of the mod importer. The
 * game reads the file in hangars.h, which describes its form.
 *
 * The layers come from these sources:
 *   - `default`: FlightModels/<Section>.txt, else [<Section>] of
 *     FlightModels/default.ini;
 *   - `ships.<model>`: FlightModels/<Model><Section>.txt, else [<Section>]
 *     of FlightModels/<Model>.ini. It gets `hangar_model` when
 *     <Model>Hangar.opt was converted;
 *   - `ships.<model>.by_iff.<n>`: the same sections with the IFF after their
 *     name, such as [HangarObjects1];
 *   - `crafts.<model>`: the [Camera] and [Size] sections of each model.
 * A layer holds only the settings that its source sets, because the hook
 * skips empty sections.
 *
 * Each [HangarObjects] key becomes a setting with the same meaning, in
 * metres and degrees. For example, ShuttlePositionX/Y become
 * `shuttle.position_m`, HangarIff -1 becomes `hangar.markings: command_ship`
 * and ShuttleModelIndex 0 becomes `shuttle.type: default`. Some keys need
 * more care:
 *   - The hook looks up each key in the layers, and the game looks up each
 *     setting the same way. Some keys become one setting, such as the three
 *     keys of a position. A layer must set all of them, or the import fails.
 *   - LoadDroids, DroidsPositionZ and IsDroidsFloorInverted apply to both
 *     droids, together with each droid's own key. When a layer sets either
 *     key, the import writes the droid's combined `enabled`, `height_m` or
 *     `floor`, using the layers below. If the result depends on the command
 *     ship, which can happen in a mission's layer, the import fails.
 *   - PlayerModelIndices and PlayerOffsetsX/Y/Z become
 *     `player.craft_offsets_m`, and PlayerFloorInvertedModelIndices becomes
 *     `player.inverted_floor_crafts`.
 *   - A key without a value hides the value of the lower layers, and the
 *     key's default applies. The import writes that default. When the default
 *     comes from another key, as with PlayerInvertedOffsetX, the import
 *     fails.
 *
 * [HangarMap] lines become `crane_arm` and `parked`, and [FamHangarMap]
 * lines become the same keys of `family_base`. The hook uses the second line
 * of [HangarMap] and the third line of [FamHangarMap] as the crane arm. The
 * roles of the other first lines have no effect. A line has 6 to 9 fields:
 * the model, the markings, x, y, z, the heading in XY and in Z, the profile
 * ("Default" when missing) and whether the object hangs from the inverted
 * floor. A z of 2147483647 puts the object on the floor. A line with another
 * number of fields places nothing.
 *
 * [HangarCamera] and [FamHangarCamera] give every key that the hook reads,
 * with 0 for a missing key: Key1/2/3/6/9 and FamKey1/2/3/6/7/8/9.
 *
 * The files are read the way the hook reads them. Sections and keys can be
 * in any case, the first line of a key wins, numbers are read with
 * std::stoi(value, 0, 0), and lists are split on ',' and ';'. XWAU stops on
 * a value that isn't a number, so the import leaves such a key out. The
 * import also converts these:
 *   - XWAU's model indices. The installation's XWINGALLIANCE.EXE maps each
 *     index to a row of its object list. Each index becomes the OpenXWA
 *     object type that loads the same model. This is the type with the same
 *     index when its model matches, else the only type that loads that model.
 *     The model of an OpenXWA type is the `model` of its object type file,
 *     else the model of its row in the stock game. When no OpenXWA type loads
 *     the model, for example for a row that XWAU added, the import fails. A
 *     row without a model file is dropped: a map line places nothing, and a
 *     key is left out;
 *   - a swap "FlightModels\<A>.opt = FlightModels\<B>.opt" becomes "a: b"
 *     when B is in the installation and was converted. The hook ignores
 *     swaps to <A>Exterior.opt;
 *   - [Camera] keys fall back the way the hook does it. When a triple is all
 *     zero, the next one in its list is used: X/Y/Z; InvertedX/Y/Z, then
 *     X/Y/Z; FamX/Y/Z, then X/Y/Z; FamInverted, Fam, Inverted, then X/Y/Z;
 *   - [Size] gives ClosedSFoilsElevation (0 when missing) and
 *     ClosedSFoilsElevationInverted (ClosedSFoilsElevation when 0).
 * Unknown HangarObjects keys, lines without '=' and repeated keys are left
 * out, because the hook ignores them. The import fails on a swap to a model
 * that wasn't converted, and on a hangar model that wasn't converted. The
 * import doesn't convert these, and reports them as unsupported:
 * LightColorIntensity, LightColorRgb and DrawShadows, because the remaster
 * lights the hangar itself; swaps to a file that isn't an OPT; the per-ship
 * mission sections [<Section>_<ship>]; and a mission's own hangar model,
 * <mission>_Hangar.opt.
 *
 * With the stock game, each mission's own layer becomes the `hangar` of its
 * mission file, flight/missions/<dir>/<mission>.yaml. The layer comes from
 * [<Section>] of <mission>.ini or from <mission>_<Section>.txt. The mission
 * file is created when needed. When the mission's [Objects] lines swap the
 * mothership, `use_ship_settings` names the ship whose settings and hangar
 * model apply instead:
 *
 *     hangar:
 *       droids:
 *         - {markings: 2}
 *       use_ship_settings: { frigate2standard: frigate2carrier }
 */

#include <stdbool.h>

#include "xwau_game_tables.h"
#include "xwau_names.h"

/* Writes <mod>/flight/hangars.yaml for the installation `xwau`, and also the
 * missions' layers when `stock_game` is set. `stock` holds the stock game's
 * tables. The import uses the converted models in <mod>/flight/models and
 * the object type files in <mod>/flight/object_types. `game_names` gives the
 * object type names, with the mod's renames. Returns false when a file can't
 * be read or written. */
bool XwauHangars_Write(const char* xwau, const XwauGameTables* stock, const XwauNames* game_names,
					   const char* stock_game, const char* mod);

#endif /* AERON_XWAU_HANGARS_H */
