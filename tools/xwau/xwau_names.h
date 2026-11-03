#ifndef AERON_XWAU_NAMES_H
#define AERON_XWAU_NAMES_H

/*
 * Names for the game's values, such as object types, weapon types and
 * warheads. Mod files use these names instead of numbers. The game owns the
 * tables. The program that calls the writers fills XwauNames from them, so
 * the writers use the same names as the game.
 *
 * Each list has a name for each value from `first` to `first + count - 1`.
 * A NULL entry is a value without a name, and the writers skip it.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct XwauNameList {
	const char* const* names;
	int                first;
	int                count;
} XwauNameList;

typedef struct XwauNames {
	/* The stock object type names, and the mod's new names.
	   `object_type_renames` has `object_types.count` entries. An entry is
	   NULL when the object type keeps its stock name. */
	XwauNameList object_types;
	const char** object_type_renames;
	XwauNameList weapon_types;
	XwauNameList warheads;
	XwauNameList genus;
	XwauNameList family;
	XwauNameList object_type_flags;          /* indexed by bit */
	uint16_t     settable_object_type_flags; /* the flags that a file can set */
	XwauNameList ship_list_categories;
	XwauNameList ship_list_category_texts; /* the text that the game shows */
	XwauNameList laser_kinds;
	XwauNameList hardpoint_kinds;
	XwauNameList ranks;
	XwauNameList difficulties;
	XwauNameList reticle_slots;
	XwauNameList sound_weapon_groups;
	XwauNameList screen_sources;
	XwauNameList hud_areas; /* indexed by bit */
} XwauNames;

/* Returns the name of `value` in `list`, or NULL when it has no name. */
const char* XwauNames_Name(const XwauNameList* list, int value);
/* Returns the value named `name` in `list`, or -1. */
int XwauNames_Find(const XwauNameList* list, const char* name);
/* Returns whether the mod renames `object_type`. */
bool XwauNames_IsRenamed(const XwauNames* names, int object_type);
/* Returns the name of `object_type` in the mod: the new name if the mod
   renames it, else the stock name. Returns NULL when it has no name. */
const char* XwauNames_ObjectType(const XwauNames* names, int object_type);
/* Returns the name of `weapon_type` in the mod, like
   XwauNames_ObjectType. */
const char* XwauNames_WeaponType(const XwauNames* names, int weapon_type);

#endif /* AERON_XWAU_NAMES_H */
