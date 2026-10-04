#ifndef AERON_ASSET_OPT_PAYLOAD_H
#define AERON_ASSET_OPT_PAYLOAD_H

/* In-memory forms of the OPT node payloads shared by the OPT simulation model
 * and the OPT image writer. Values are in OPT units and encodings; the writer
 * serializes them explicitly as little-endian, never by copying these
 * structs. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One triangle. `vertex_index` uses the layout of the first four ints of an
 * OPT face record: { a, b, c, -1 }. */
typedef struct AeronOptFace {
	int32_t vertex_index[4];
	float   normal[3]; /* unit, outward, OPT frame */
} AeronOptFace;

typedef struct AeronOptMeshDescriptor {
	int32_t mesh_type;
	int32_t explosion_type;
	float   span[3];
	float   center[3];
	float   box_min[3];
	float   box_max[3];
	int32_t target_id;
	float   target[3];
} AeronOptMeshDescriptor;

/* Axes are Q15-scaled floats (32767 ~= 1.0), as all OPT versions store them. */
typedef struct AeronOptRotScale {
	float pivot[3];
	float rotation_axis[3];
	float direction_axis[3];
	float up_axis[3];
} AeronOptRotScale;

typedef struct AeronOptHardpoint {
	int32_t type;
	float   position[3];
} AeronOptHardpoint;

typedef struct AeronOptEngineGlow {
	int32_t  is_disabled;
	uint32_t core_color;  /* 0xAARRGGBB */
	uint32_t outer_color; /* 0xAARRGGBB */
	float    dimensions[3];
	float    position[3];
	float    look[3]; /* unit */
	float    up[3];
	float    right[3];
} AeronOptEngineGlow;

#ifdef __cplusplus
}
#endif

#endif
