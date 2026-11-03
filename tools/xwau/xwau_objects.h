#ifndef AERON_XWAU_OBJECTS_H
#define AERON_XWAU_OBJECTS_H

/*
 * Writes the object type and craft type files of a mod (the mod importer's
 * craft phase):
 *   - <output_dir>/object_types/<model>.yaml, one for each object type that
 *     the mod changes or fills with a new craft;
 *   - <output_dir>/craft_types/<name>.yaml, one for each craft type that the
 *     mod links to another object type or whose ship list line or tech room
 *     texts it changes.
 * The importer compares the mod's XWINGALLIANCE.EXE and lists with the stock
 * game's tables:
 *   - the object type table (0x5FB240, 557 entries of 24 bytes), the craft
 *     type table (0x5B0F70, 232 entries) and the craft records (0x5BB480,
 *     265 records of 987 bytes, packed, with 4-byte pointers);
 *   - FlightModels/Spacecraft0.LST and Equipment0.LST;
 *   - the four sound switches that pick an engine or launch sound case by
 *     object type, and the test that takes the beam away from six craft.
 *
 * The mod's tables first get the [ExeCraftValues] sections of its models, as
 * hook_mission_tie applies them at each mission load. For each object type
 * below 219 with a craft record below 218, the section of the model that
 * the type loads (FlightModels/<model>ExeCraftValues.txt, else the section
 * of FlightModels/<model>.ini) sets:
 *   - GameOptions_IsHardpointsMirroring and GameOptions_IsAxisAligned: the
 *     type's flags 0x800 and 0x1000;
 *   - LaserTypeId0-2, LaserSequence0-2, LaserRange0-2 and LaserFireRatio0-2:
 *     the type, kind, range and cooldown of the laser groups;
 *   - WarheadTypeId0-1 and WarheadCapacity0-1: the type and capacity of the
 *     warhead launchers;
 *   - CounterMeasuresCount and AIHitsTakenToEvade.
 * A missing key keeps the value of the table. Values are read with
 * std::stoi(value, 0, 0) and cut to the field's size, as the hook does. The
 * comparison below then writes what changes. XWAU changes the tables only
 * during missions, so the tech room now sees the new weapons too. The
 * importer reports two object types that share a record and both have the
 * section (the last one wins, as in XWAU), a weapon type that the model has
 * no hardpoint for, and a model with the section that no object type
 * loads. A mission that swaps to or from such a model would change the
 * values for that mission only; the mission import reports it
 * (xwau_mission_files.h).
 *
 * An object type's model, in each game, is the name of its list row, in
 * lower case and without the folder and the extension. When the mod's model
 * has another name, the file gets `model`. One exception: a row that the mod
 * renames for one of its new craft (XWAU's CorvetteExplode rows 144 and 145,
 * which its MC80s load) keeps the stock model for the stock object types that
 * load it.
 *
 *   # Made by the OpenXWA mod importer.
 *   version: 1
 *   object_type: victory_star_destroyer2
 *   model: victorystardestroyer
 *   genus: starship
 *   family: craft
 *   flags: [player_targetable, ai_targetable, float_hardpoints]
 *   craft_record:
 *     hull: 16590
 *     lasers:
 *       - {type: rebel_laser_turbo, count: 2, kind: turret, range: 65536, cooldown: 236}
 *   sounds_like: tie_fighter
 *   beam_allowed: true
 *   names: {short: 'QF/Cr', long: 'Quasar Fire Carrier', plural: '...', gender: m}
 *
 *   # Made by the OpenXWA mod importer.
 *   version: 1
 *   craft_type: 124
 *   object_type: victory_star_destroyer2  # victorystardestroyer
 *   ship_list: {name: '...', category: fighter, flyable: true, known: true,
 *               skirmish: true, hidden: false, icon: [82, 10, 89, 18]}
 *   tech_room: {name: '...', description: '...'}
 *
 * The files use the game's names. `object_type` is the slot's stock name,
 * and `name` is the new name from XwauObjects_Rename. Other object types are
 * written with their names in the mod.
 *
 * A file only has the keys that change, and `craft_record` only has the
 * fields that change. The importer compares the object type's record in the
 * mod with the type's own stock record, and the game writes the changes into
 * the type's own record. So when the mod moves an object type to another
 * record, or when several object types share a record, each of them gets its
 * own changes. When one laser group changes, `lasers` lists all three
 * groups. `warheads` works the same way for the two launchers, and `turrets`
 * for the two turret seats, with the gunner seat keys. A group that isn't
 * listed is empty. Fields that the game rebuilds from the model when it
 * loads it (bounds, hardpoints, the first and last weapon slots,
 * engine glows, dock, aux and jamming points, child mounts) and the point
 * value aren't written. Other changed fields that have no key are reported,
 * because the game never reads them.
 *
 * A file can set seven flags: player_targetable, ai_targetable, nav_buoy,
 * static_object, float_hardpoints, spins and collision_face_groups. If the
 * mod turns one of them on or off, the file gets a `flags` list with each of
 * these flags that is on, because the game turns off the ones that aren't
 * listed. The other flags come from the original image files. If the mod
 * changes them, the import fails. Bits 0x10 and 0x40 are left out and
 * reported, because the game never reads them.
 *
 * `names` holds the species names of the object type's record in the mod,
 * when they differ from the names of its stock record and the object type
 * has a file.
 *
 * The object type files are for the stock object types of both lists, the
 * free slots that the mod fills with a new craft, and the placeholders
 * 103 to 114 that it gives another model. When the type's row is in
 * Equipment0.LST, `model` comes with `list: equipment`. A slot or a
 * placeholder that the mod gives an empty row loads nothing, so it gets no
 * file, and neither do its craft types.
 *
 * A craft type file has the craft type's object type in the mod, with that
 * object type's model in a comment. It also has the ship list line and the
 * tech room texts that the mod changes. The ship list category is written as
 * a name. XWAU's "Planet/Asteroid" isn't a game category, and XWAU leaves
 * such a craft out of the ship list. The file would list it as
 * satellite_buoy, so the import fails. The file is only written when the
 * object type loads a model in the mod. Otherwise the text import keeps
 * skipping these texts. The file is named after the craft type's ship list
 * name in the mod, in lower case with '_' between words. When two craft
 * types have the same name, they get <name>_<craft type>.yaml. A craft type
 * without a name gets craft_type_<n>.yaml.
 *
 * The names and the craft type texts aren't in text/strings.yaml (the
 * `moved` IDs of XwauObjects_Write).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xwau_game_tables.h"
#include "xwau_names.h"
#include "xwau_text.h"

/* Whether model `model` (a model key) of the installation `game` has an
 * [ExeCraftValues] section. */
bool XwauObjects_HasExeCraftValues(const char* game, const char* model);

/* Finds the object types that the mod at `mod_root` renames, and stores the
 * new names in `names`. A slot is renamed when the mod's ship list shows it
 * with a different name than the stock game, or when the stock game doesn't
 * list it. The new name is the ship list name in lower case with '_' between
 * words, such as tie_advanced_x1. If another object type already has that
 * name, the slot number is added. A slot that only changes its model, stats
 * or flags keeps its stock name. So do the weapon and effect slots, which
 * aren't in the ship list. `mod` holds the mod's tables, or is NULL when they
 * can't be read; nothing is renamed then. Reports each rename. Returns false
 * only when out of memory. XwauObjects_FreeRenames frees the names. */
bool XwauObjects_Rename(const char* mod_root, const char* stock_game, const XwauGameTables* stock,
						const XwauGameTables* mod, XwauNames* names);
void XwauObjects_FreeRenames(XwauNames* names);

/* Writes the object type and craft type files of the mod at `mod_root` into
 * `output_dir`/object_types and `output_dir`/craft_types. Adds the IDs of the
 * texts that these files contain to `moved`. `stock` holds the stock game's
 * tables, and `stock_game` is the stock game's folder, which has its texts.
 * `mod` holds the mod's tables, and the [ExeCraftValues] sections are
 * applied to it. The files use the names in `names`. Returns false when
 * `mod` is NULL, or when a file can't be read or written. */
bool XwauObjects_Write(const char* mod_root, const char* stock_game, const XwauGameTables* stock,
					   XwauGameTables* mod, const XwauNames* names, const char* output_dir,
					   XwauTextIds* moved);

#endif /* AERON_XWAU_OBJECTS_H */
