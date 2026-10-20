#ifndef AERON_XWAU_OBJECTS_H
#define AERON_XWAU_OBJECTS_H

/*
 * Writes the object type and craft type files of a mod (xwau_import craft
 * --stock-game DIR):
 *   - <output_dir>/object_types/<model>.yaml, one for each object type that
 *     the mod changes or fills with a new craft;
 *   - <output_dir>/craft_types/<name>.yaml, one for each craft type that the
 *     mod links to another object type or whose ship list line or tech room
 *     texts it changes.
 * The importer compares the mod's XWINGALLIANCE.EXE and lists with the stock
 * game's:
 *   - the object type table (0x5FB240, 557 entries of 24 bytes), the craft
 *     type table (0x5B0F70, 232 entries) and the craft records (0x5BB480,
 *     265 records of 987 bytes, packed, with 4-byte pointers);
 *   - FlightModels/Spacecraft0.LST and Equipment0.LST;
 *   - the four sound switches that pick an engine or launch sound case by
 *     object type, and the test that takes the beam away from six craft.
 *
 * An object type's model, in each game, is the name of its list row, in
 * lower case and without the folder and the extension. When the mod's model
 * has another name, the file gets `model`. One exception: a row that the mod
 * renames for one of its new craft (XWAU's CorvetteExplode rows 144 and 145,
 * which its MC80s load) keeps the stock model for the stock object types that
 * load it.
 *
 *   # Written by xwau_import craft ...
 *   version: 1
 *   object_type: 138
 *   model: victorystardestroyer
 *   model_type: {genus: 4, family: 0, flags: 0x843, craft_record_index: 140}
 *   craft_record:
 *     hull: 16590
 *     lasers:
 *       - {type: 281, count: 2, kind: 4, range: 65536, cooldown: 236}
 *   sounds_like: 5
 *   beam_allowed: true
 *   names: {short: 'QF/Cr', long: 'Quasar Fire Carrier', plural: '...', gender: m}
 *
 *   # Written by xwau_import craft ...
 *   version: 1
 *   craft_type: 124
 *   object_type: 138  # victorystardestroyer
 *   ship_list: {name: '...', category: 'Fighter', flyable: true, known: true,
 *               skirmish: true, hidden: false, icon: [82, 10, 89, 18]}
 *   tech_room: {name: '...', description: '...'}
 *
 * `model_type` and `craft_record` hold only the keys that change. The
 * craft record is compared with the stock record that the object type uses
 * after its `craft_record_index`. `lasers` lists the three laser groups when
 * one of them changes, `warheads` the two launchers and `turrets` the two
 * turret seats; a group that isn't listed is empty. Fields that the game
 * rebuilds from the model when it loads it (bounds, hardpoints, the first and
 * last weapon slots, engine glows, dock, aux and jamming points, child
 * mounts) and the point value aren't written. Other changed fields that have
 * no key are reported.
 *
 * `names` holds the species names of the object type's craft record, when
 * the mod changes them and the object type has a file.
 *
 * The object type files are for the stock object types of both lists, the
 * reserved slots that the mod fills with a new craft, and the placeholders
 * 103 to 114 that it gives another model. When the type's row is in
 * Equipment0.LST, `model` comes with `list: equipment`. A slot or a
 * placeholder that the mod gives an empty row loads nothing, so it gets no
 * file, and neither do its craft types.
 *
 * A craft type file has the craft type's object type in the mod, with the
 * object type's model as a comment, and the ship list line and tech room
 * texts that the mod changes. It is only written when the object type loads
 * a model in the mod; otherwise the text import keeps skipping the texts. A
 * file is named after the craft type's ship list name in the mod, in lower
 * case with '_' between words. Two craft types with the same name get
 * <name>_<craft type>.yaml, and one without a name craft_type_<n>.yaml.
 *
 * The names and the craft type texts aren't in text/strings.yaml (the
 * `moved` IDs of XwauObjects_Write). Files that an earlier run wrote (they
 * start with "# Written by xwau_import craft") are removed first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xwau_text.h"

/* Writes the object type and craft type files of the mod at `mod_root` into
 * `output_dir`/object_types and `output_dir`/craft_types, and adds to `moved`
 * the text IDs that they carry. Returns false when a file can't be read or
 * written. */
bool XwauObjects_Write(const char* mod_root, const char* stock_game, const char* output_dir,
					   XwauTextIds* moved);

#endif /* AERON_XWAU_OBJECTS_H */
