#ifndef AERON_ASSET_FLIGHT_TAGS_H
#define AERON_ASSET_FLIGHT_TAGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;
struct cgltf_data;

/* Game-defined tag: lower-case ASCII letters, digits, '.' and '_'. Components,
 * materials and lights carry tags in their AERON_flight_model `tags` array.
 * Aeron stores them without interpreting them. */
#define AERON_FLIGHT_TAG_MAX 64

typedef struct AeronFlightTag {
	char name[AERON_FLIGHT_TAG_MAX];
} AeronFlightTag;

/* The distinct tags of a model's materials and lights, in order: the
 * materials' tags in material order, then the lights' in node order. A
 * material or a light records its tags as a mask of these groups: bit g for
 * `names[g]`. */
#define AERON_FLIGHT_TAG_GROUP_MAX 32

typedef struct AeronFlightTagGroups {
	uint32_t       count;
	AeronFlightTag names[AERON_FLIGHT_TAG_GROUP_MAX];
} AeronFlightTagGroups;

/* True when `tag` is 1 to AERON_FLIGHT_TAG_MAX - 1 characters of the tag
 * syntax. */
bool Aeron_FlightTagValid(const char* tag);

/* Reads a `tags` array into a new array (caller frees it) and its count.
 * NULL `tags` gives no tags. Malformed tags make it fail; repeats count
 * once. */
bool Aeron_FlightTagsParse(const struct cJSON* tags, AeronFlightTag** out, uint32_t* count);

/* Collects the tag groups of `data`. Fails, with a log line, on a malformed
 * `tags` array or more than AERON_FLIGHT_TAG_GROUP_MAX distinct tags. */
bool Aeron_FlightTagGroupsBuild(const struct cgltf_data* data, const char* source_label,
								AeronFlightTagGroups* out);

/* The mask of the groups that the valid `tags` array names; 0 for NULL. */
uint32_t Aeron_FlightTagGroupsMask(const AeronFlightTagGroups* groups, const struct cJSON* tags);

#ifdef __cplusplus
}
#endif

#endif
