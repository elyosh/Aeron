#include "xwau_names.h"

#include <stddef.h>
#include <string.h>

const char* XwauNames_Name(const XwauNameList* list, int value) {
	if (!list->names || value < list->first || value - list->first >= list->count)
		return NULL;
	return list->names[value - list->first];
}

int XwauNames_Find(const XwauNameList* list, const char* name) {
	for (int i = 0; name && list->names && i < list->count; ++i)
		if (list->names[i] && strcmp(list->names[i], name) == 0)
			return list->first + i;
	return -1;
}

const char* XwauNames_KeyAction(const XwauNames* names, const char* key) {
	for (int i = 0; names && key && i < names->key_action_count; ++i)
		if (strcmp(names->key_actions[i].key, key) == 0)
			return names->key_actions[i].action;
	return NULL;
}

bool XwauNames_IsRenamed(const XwauNames* names, int object_type) {
	return object_type >= 0 && object_type < names->object_types.count && names->object_type_renames &&
		   names->object_type_renames[object_type];
}

const char* XwauNames_ObjectType(const XwauNames* names, int object_type) {
	if (XwauNames_IsRenamed(names, object_type))
		return names->object_type_renames[object_type];
	return XwauNames_Name(&names->object_types, object_type);
}

const char* XwauNames_WeaponType(const XwauNames* names, int weapon_type) {
	return XwauNames_Name(&names->weapon_types, weapon_type) ? XwauNames_ObjectType(names, weapon_type)
															 : NULL;
}
