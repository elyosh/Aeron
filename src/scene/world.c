#include "aeron/scene/world.h"

void AeronWorld_LocalI32(const int32_t origin[3], const int32_t world[3], float out[3]) {
	for (int axis = 0; axis < 3; ++axis)
		out[axis] = (float)((int64_t)world[axis] - (int64_t)origin[axis]);
}

void AeronWorld_DeltaI32(const int32_t a[3], const int32_t b[3], float out_a_minus_b[3]) {
	for (int axis = 0; axis < 3; ++axis)
		out_a_minus_b[axis] = (float)((int64_t)a[axis] - (int64_t)b[axis]);
}

void AeronWorld_LocalPointI32F32(const int32_t origin[3], const int32_t base[3], const float offset[3],
								 float out[3]) {
	for (int axis = 0; axis < 3; ++axis)
		out[axis] = (float)((double)((int64_t)base[axis] - (int64_t)origin[axis]) + (double)offset[axis]);
}
