#ifndef AERON_OPT2GLTF_OPT_JSON_H
#define AERON_OPT2GLTF_OPT_JSON_H

/*
 * Helpers for the converter's JSON objects, built with cJSON and printed
 * unformatted. Floats are printed as cgltf_write prints the glTF's own
 * numbers, "%.9g": nine significant digits, which read back as the same
 * float (cJSON would print the double with up to 17).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cJSON.h"

/* A float, printed "%.9g"; NULL when out of memory. */
static inline cJSON* OptJson_Float(float value) {
	char text[32];
	snprintf(text, sizeof text, "%.9g", (double)value);
	return cJSON_CreateRaw(text);
}

/* Adds `item` to the object `parent` as `key`, or to the array `parent`
 * when `key` is NULL. False, with `item` deleted, when `parent` or `item`
 * is NULL (out of memory). */
static inline bool OptJson_Add(cJSON* parent, const char* key, cJSON* item) {
	const bool added = parent && item &&
					   (key ? cJSON_AddItemToObject(parent, key, item) : cJSON_AddItemToArray(parent, item));
	if (!added)
		cJSON_Delete(item);
	return added;
}

/* A new array or object added to `parent` as `key` (to the array `parent`
 * when `key` is NULL), owned by it; NULL when out of memory. Containers are
 * added as they are made, so that a failure leaves nothing unowned. */
static inline cJSON* OptJson_AddArray(cJSON* parent, const char* key) {
	cJSON* array = cJSON_CreateArray();
	return OptJson_Add(parent, key, array) ? array : NULL;
}

static inline cJSON* OptJson_AddObject(cJSON* parent, const char* key) {
	cJSON* object = cJSON_CreateObject();
	return OptJson_Add(parent, key, object) ? object : NULL;
}

/* An array of `count` floats; NULL when out of memory. */
static inline cJSON* OptJson_Floats(const float* values, size_t count) {
	cJSON* array = cJSON_CreateArray();
	bool   ok    = array != NULL;
	for (size_t i = 0; i < count; ++i)
		ok &= OptJson_Add(array, NULL, OptJson_Float(values[i]));
	if (!ok) {
		cJSON_Delete(array);
		return NULL;
	}
	return array;
}

/* Prints `object` unformatted and deletes it; NULL when out of memory. */
static inline char* OptJson_Print(cJSON* object) {
	char* text = object ? cJSON_PrintUnformatted(object) : NULL;
	cJSON_Delete(object);
	return text;
}

#endif /* AERON_OPT2GLTF_OPT_JSON_H */
