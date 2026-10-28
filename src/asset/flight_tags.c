#include "aeron/asset/flight_tags.h"

#include "aeron/asset/flight_model.h"

#include "cJSON.h"
#include "cgltf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void SDL_Log(const char* fmt, ...);

bool Aeron_FlightTagValid(const char* tag) {
	const size_t length = tag ? strlen(tag) : 0;
	if (length == 0 || length >= AERON_FLIGHT_TAG_MAX)
		return false;
	for (size_t index = 0; index < length; ++index) {
		const char c = tag[index];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'))
			return false;
	}
	return true;
}

/* True when `tags` is absent or an array of valid tags. */
static bool valid_tags(const cJSON* tags) {
	if (!tags)
		return true;
	if (!cJSON_IsArray(tags))
		return false;
	const cJSON* tag = NULL;
	cJSON_ArrayForEach(tag, tags) {
		if (!cJSON_IsString(tag) || !Aeron_FlightTagValid(tag->valuestring))
			return false;
	}
	return true;
}

bool Aeron_FlightTagsParse(const cJSON* tags, AeronFlightTag** out, uint32_t* count) {
	*out   = NULL;
	*count = 0;
	if (!valid_tags(tags))
		return false;
	const int size = tags ? cJSON_GetArraySize(tags) : 0;
	if (size == 0)
		return true;
	*out = calloc((size_t)size, sizeof **out);
	if (!*out)
		return false;
	const cJSON* tag = NULL;
	cJSON_ArrayForEach(tag, tags) {
		bool repeat = false;
		for (uint32_t existing = 0; existing < *count; ++existing)
			if (strcmp((*out)[existing].name, tag->valuestring) == 0)
				repeat = true;
		if (!repeat)
			strcpy((*out)[(*count)++].name, tag->valuestring);
	}
	return true;
}

static int group_index(const AeronFlightTagGroups* groups, const char* tag) {
	for (uint32_t index = 0; index < groups->count; ++index)
		if (strcmp(groups->names[index].name, tag) == 0)
			return (int)index;
	return -1;
}

/* Adds the tags of `tags` to the groups. `owner` names the material or light
 * in the log. */
static bool add_groups(const cJSON* tags, const char* source_label, const char* owner,
					   AeronFlightTagGroups* out) {
	if (!valid_tags(tags)) {
		SDL_Log("[flight_model] '%s' %s has malformed tags", source_label, owner);
		return false;
	}
	const cJSON* tag = NULL;
	cJSON_ArrayForEach(tag, tags) {
		if (group_index(out, tag->valuestring) >= 0)
			continue;
		if (out->count == AERON_FLIGHT_TAG_GROUP_MAX) {
			SDL_Log("[flight_model] '%s' has more than %d material and light tags", source_label,
					AERON_FLIGHT_TAG_GROUP_MAX);
			return false;
		}
		strcpy(out->names[out->count++].name, tag->valuestring);
	}
	return true;
}

static bool is_light(const cJSON* object) {
	const cJSON* role = cJSON_GetObjectItemCaseSensitive(object, "role");
	return cJSON_IsString(role) && strcmp(role->valuestring, "light") == 0;
}

static const char* flight_model_json(const cgltf_extension* extensions, cgltf_size count) {
	for (cgltf_size index = 0; index < count; ++index)
		if (extensions[index].name && extensions[index].data &&
			strcmp(extensions[index].name, AERON_FLIGHT_MODEL_EXTENSION) == 0)
			return extensions[index].data;
	return NULL;
}

bool Aeron_FlightTagGroupsBuild(const cgltf_data* data, const char* source_label, AeronFlightTagGroups* out) {
	memset(out, 0, sizeof *out);
	if (!source_label)
		source_label = "<memory glTF>";
	for (cgltf_size index = 0; index < data->materials_count; ++index) {
		const cgltf_material* material = &data->materials[index];
		const char*           json     = flight_model_json(material->extensions, material->extensions_count);
		if (!json)
			continue;
		char owner[32];
		snprintf(owner, sizeof owner, "material %u", (unsigned)index);
		cJSON*     object = cJSON_Parse(json);
		const bool added =
			add_groups(cJSON_GetObjectItemCaseSensitive(object, "tags"), source_label, owner, out);
		cJSON_Delete(object);
		if (!added)
			return false;
	}
	for (cgltf_size index = 0; index < data->nodes_count; ++index) {
		const cgltf_node* node = &data->nodes[index];
		const char*       json = flight_model_json(node->extensions, node->extensions_count);
		if (!json)
			continue;
		cJSON*     object = cJSON_Parse(json);
		const bool added  = !is_light(object) || add_groups(cJSON_GetObjectItemCaseSensitive(object, "tags"),
															source_label, "a light", out);
		cJSON_Delete(object);
		if (!added)
			return false;
	}
	return true;
}

uint32_t Aeron_FlightTagGroupsMask(const AeronFlightTagGroups* groups, const cJSON* tags) {
	uint32_t     mask = 0;
	const cJSON* tag  = NULL;
	cJSON_ArrayForEach(tag, tags) {
		const int group = cJSON_IsString(tag) ? group_index(groups, tag->valuestring) : -1;
		if (group >= 0)
			mask |= 1u << group;
	}
	return mask;
}
