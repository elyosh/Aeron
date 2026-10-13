#ifndef AERON_SCENE_MATERIAL_ANIM_HLSLI
#define AERON_SCENE_MATERIAL_ANIM_HLSLI

/*
 * Material animation evaluation, over the per-mesh animation records of
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

/* A damage rectangle in the texture's UV: s0 to s1. */
struct MaterialAnimRect
{
	float x0, y0, x1, y1;
};
#ifndef __HLSL_VERSION
typedef struct MaterialAnimRect MaterialAnimRect;
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
		record++;
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

/* Integer hash (lowbias32) of two values. */
AERON_ANIM_FN uint material_anim_hash(uint a, uint b)
{
	uint x = a * 0x9E3779B9u ^ b;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/* A hash as a value in [0, 1). */
AERON_ANIM_FN float material_anim_unorm(uint h)
{
	return (float)(h >> 8) * (1.0f / 16777216.0f);
}

/* The first record of the damage alternative an object (`seed`) shows: one
 * per (object, section `choice`), the same at every stage. */
AERON_ANIM_FN uint material_anim_damage_alternative(uint first, uint count, uint seed, uint choice)
{
	return first + (material_anim_hash(seed, choice * 4u) % count) * AERON_ANIM_DAMAGE_RECORDS;
}

/* The damage rectangle of alternative record `alternative` for an object
 * (`seed`), with random values per (object, section `placement`): r_s for
 * the size, r per axis for the offset. Without a scale it is the whole
 * texture; an "inside" offset keeps it within the texture, a range offset
 * moves it by min + (max - min)·r per axis. */
AERON_ANIM_FN MaterialAnimRect material_anim_damage_rect(uint alternative, uint seed, uint placement)
{
	uint  scale_min_bits = AERON_ANIM_RECORD(alternative + 3u).x;
	uint  scale_max_bits = AERON_ANIM_RECORD(alternative + 3u).y;
	uint  has_scale      = AERON_ANIM_RECORD(alternative + 3u).z;
	uint  mode           = AERON_ANIM_RECORD(alternative + 3u).w;
	float r_s            = material_anim_unorm(material_anim_hash(seed, placement * 4u + 1u));
	float r_x            = material_anim_unorm(material_anim_hash(seed, placement * 4u + 2u));
	float r_y            = material_anim_unorm(material_anim_hash(seed, placement * 4u + 3u));
	MaterialAnimRect rect;
	rect.x0 = 0.0f;
	rect.y0 = 0.0f;
	rect.x1 = 1.0f;
	rect.y1 = 1.0f;
	if (has_scale != 0u) {
		/* RAND_SCALE: a centred square of side lerp(min, max, r_s). */
		float smin = material_anim_asfloat(scale_min_bits);
		float side = smin + (material_anim_asfloat(scale_max_bits) - smin) * r_s;
		float half_rest = (1.0f - side) * 0.5f;
		rect.x0 = half_rest;
		rect.y0 = half_rest;
		rect.x1 = 1.0f - half_rest;
		rect.y1 = 1.0f - half_rest;
	}
	float ofs_x = 0.0f;
	float ofs_y = 0.0f;
	if (mode == AERON_ANIM_OFFSET_INSIDE) {
		ofs_x = (rect.x0 + (1.0f - rect.x1)) * r_x - rect.x0;
		ofs_y = (rect.y0 + (1.0f - rect.y1)) * r_y - rect.y0;
	} else if (mode == AERON_ANIM_OFFSET_RANGE) {
		float min_x = material_anim_asfloat(AERON_ANIM_RECORD(alternative + 4u).x);
		float min_y = material_anim_asfloat(AERON_ANIM_RECORD(alternative + 4u).y);
		ofs_x       = min_x + (material_anim_asfloat(AERON_ANIM_RECORD(alternative + 4u).z) - min_x) * r_x;
		ofs_y       = min_y + (material_anim_asfloat(AERON_ANIM_RECORD(alternative + 4u).w) - min_y) * r_y;
	}
	rect.x0 += ofs_x;
	rect.y0 += ofs_y;
	rect.x1 += ofs_x;
	rect.y1 += ofs_y;
	return rect;
}

#endif /* AERON_SCENE_MATERIAL_ANIM_HLSLI */
