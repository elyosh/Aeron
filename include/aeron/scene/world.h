#ifndef AERON_SCENE_WORLD_H
#define AERON_SCENE_WORLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert absolute integer coordinates only after exact subtraction. */
void AeronWorld_LocalI32(const int32_t origin[3], const int32_t world[3], float out[3]);
void AeronWorld_DeltaI32(const int32_t a[3], const int32_t b[3], float out_a_minus_b[3]);

/* A precise point represented by an integer base plus a fractional offset. */
void AeronWorld_LocalPointI32F32(const int32_t origin[3], const int32_t base[3], const float offset[3],
								 float out[3]);

#ifdef __cplusplus
}
#endif

#endif
