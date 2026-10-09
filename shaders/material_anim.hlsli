#ifndef AERON_SCENE_MATERIAL_ANIM_HLSLI
#define AERON_SCENE_MATERIAL_ANIM_HLSLI

/*
 * Material animation evaluation (docs: xwau_animated_materials.md,
 * "Shaders"), over the per-mesh animation records of
 * material_anim_layout.hlsli. Written in the subset that also compiles as
 * C, so CPU references run this code: the includer defines
 * AERON_ANIM_RECORD(i) as an expression with .x .y .z .w members (a
 * StructuredBuffer<uint4> element, or a C struct).
 */

#include "material_anim_layout.hlsli"

#ifdef __HLSL_VERSION
#define AERON_ANIM_FN
#define material_anim_asfloat asfloat
#define material_anim_floor floor
#else
#include <math.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t uint;
#define AERON_ANIM_FN static inline
static inline float material_anim_asfloat(uint u) {
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
#define material_anim_floor floorf
#endif

/* The record of the first of a material's `count` flipbooks from record
 * `first` that plays on `layer` (AERON_ANIM_LAYER_*) under `state_mask`;
 * 0 (the header) when none does. */
AERON_ANIM_FN uint material_anim_select(uint first, uint count, uint layer, uint state_mask)
{
	uint active = state_mask | (1u << AERON_ANIM_STATE_ALWAYS);
	uint record = first;
	for (uint i = 0u; i < count; i++) {
		uint x = AERON_ANIM_RECORD(record).x;
		if (((x >> AERON_ANIM_FLIPBOOK_LAYER_SHIFT) & 1u) == layer && ((active >> (x & 0x1Fu)) & 1u) != 0u)
			return record;
		record += (x & AERON_ANIM_FLIPBOOK_HAS_UV) != 0u ? 2u : 1u;
	}
	return 0u;
}

/* Time `t` seconds into a flipbook of `total` seconds: wrapped for a loop,
 * held at the end for `once`. */
AERON_ANIM_FN float material_anim_local_time(uint mode, float total, float t)
{
	if (!(total > 0.0f))
		return 0.0f;
	if (mode == AERON_ANIM_MODE_ONCE)
		return t < 0.0f ? 0.0f : (t < total ? t : total);
	return t - total * material_anim_floor(t / total);
}

/* Index of the frame showing at local time `t`: the first whose end time
 * exceeds it, the last one past the end. */
AERON_ANIM_FN uint material_anim_frame_index(uint first, uint count, uint stride, float t)
{
	uint lo = 0u;
	uint hi = count - 1u;
	while (lo < hi) {
		uint mid = (lo + hi) >> 1u;
		if (t < material_anim_asfloat(AERON_ANIM_RECORD(first + mid * stride).w))
			hi = mid;
		else
			lo = mid + 1u;
	}
	return lo;
}

/* The frame record of flipbook record `flipbook` at `t` seconds since it
 * started; 0 for a flipbook without frames. */
AERON_ANIM_FN uint material_anim_frame(uint flipbook, float t)
{
	uint x     = AERON_ANIM_RECORD(flipbook).x;
	uint first = AERON_ANIM_RECORD(flipbook).y;
	uint count = AERON_ANIM_RECORD(flipbook).z;
	if (count == 0u)
		return 0u;
	uint  stride = AERON_ANIM_FRAME_RECORDS((x >> AERON_ANIM_FLIPBOOK_LAYER_SHIFT) & 1u);
	float local  = material_anim_local_time((x >> AERON_ANIM_FLIPBOOK_MODE_SHIFT) & 1u,
											material_anim_asfloat(AERON_ANIM_RECORD(flipbook).w), t);
	return first + material_anim_frame_index(first, count, stride, local) * stride;
}

#endif /* AERON_SCENE_MATERIAL_ANIM_HLSLI */
