#ifndef AERON_OPT2GLTF_OPT_JSON_H
#define AERON_OPT2GLTF_OPT_JSON_H

/*
 * Helpers for the JSON objects of the converter. The objects are built with
 * cJSON and printed unformatted. Floats are printed with "%.9g", the same
 * way cgltf_write prints the numbers of the glTF. Nine significant digits
 * read back as the same float. cJSON would print the double with up to 17
 * digits.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cJSON.h"

/* A float printed with "%.9g", or NULL when out of memory. */
static inline cJSON* OptJson_Float(float value) {
	char text[32];
	snprintf(text, sizeof text, "%.9g", (double)value);
	return cJSON_CreateRaw(text);
}

/* Adds `item` to the object `parent` as `key`, or to the array `parent`
 * when `key` is NULL. Returns false and deletes `item` when `parent` or
 * `item` is NULL (out of memory). */
static inline bool OptJson_Add(cJSON* parent, const char* key, cJSON* item) {
	const bool added = parent && item &&
					   (key ? cJSON_AddItemToObject(parent, key, item) : cJSON_AddItemToArray(parent, item));
	if (!added)
		cJSON_Delete(item);
	return added;
}

/* Creates an array or object and adds it to `parent` as `key`, or to the
 * array `parent` when `key` is NULL. `parent` owns it. Returns NULL when out
 * of memory. Containers are added as soon as they are created, so a failure
 * leaves nothing without an owner. */
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

/* Prints `object` unformatted and deletes it. Returns NULL when out of
 * memory. */
static inline char* OptJson_Print(cJSON* object) {
	char* text = object ? cJSON_PrintUnformatted(object) : NULL;
	cJSON_Delete(object);
	return text;
}

#endif /* AERON_OPT2GLTF_OPT_JSON_H */
