#ifndef AERON_ASSET_FLIGHT_NAMES_H
#define AERON_ASSET_FLIGHT_NAMES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/* Some AERON_flight_model values come from a fixed list, such as hardpoint
 * types. The extension writes them as names. These lists hold the names: the
 * loader and the cooker use them to read the extension, and the tools print
 * them. A value is the index of its name in the list. */
typedef enum AeronFlightNameList {
	/* A hardpoint's `type`, 0 to 39. */
	AERON_FLIGHT_HARDPOINT_TYPES,
	/* A part's `meshType`, 0 to 31. */
	AERON_FLIGHT_MESH_TYPES,
	/* The bits of a part's `explosion` list. Bit 0 gives the part its own hit
	   points. Bit 1 lets it be damaged and break off. */
	AERON_FLIGHT_EXPLOSION_FLAGS,
	/* The bits of a cockpit screen rectangle's `layers` list: graphics, text
	   and bar. */
	AERON_FLIGHT_SCREEN_LAYERS,
	AERON_FLIGHT_NAME_LISTS
} AeronFlightNameList;

enum {
	AERON_FLIGHT_HARDPOINT_TYPE_COUNT = 40,
	AERON_FLIGHT_MESH_TYPE_COUNT      = 32,
	AERON_FLIGHT_EXPLOSION_FLAG_COUNT = 2,
	AERON_FLIGHT_SCREEN_LAYER_COUNT   = 3,
};

/* Returns the number of names in `list`. */
uint32_t Aeron_FlightNameCount(AeronFlightNameList list);
/* Returns the name of `value` in `list`, or NULL when `value` is out of
   range. */
const char* Aeron_FlightName(AeronFlightNameList list, uint32_t value);
/* Returns the value named `text` in `list`, or -1. */
int Aeron_FlightNameFind(AeronFlightNameList list, const char* text);
/* Reads `names`, a JSON array of names from `list`, as a bit mask: the name
   of value n sets bit n. Returns false when `names` isn't an array, or when
   one of its names isn't in `list`. */
bool Aeron_FlightNameMask(AeronFlightNameList list, const struct cJSON* names, uint32_t* out);

#ifdef __cplusplus
}
#endif

#endif
