#include "srgb8.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

/* The formulas that the tables reproduce. */
static float srgb8_to_linear_formula(uint8_t v) {
	const float c = v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t linear_to_srgb8_formula(float c) {
	c             = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

static float s_linear[256];
/* s_threshold[k] is the smallest c in [0, 1] whose sRGB value is k or more. The
   formula never goes down when c goes up, so the sRGB value of c is the largest
   k with s_threshold[k] <= c. */
static float          s_threshold[256];
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static float float_of_bits(uint32_t bits) {
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

static void fill_tables(void) {
	for (int v = 0; v < 256; ++v)
		s_linear[v] = srgb8_to_linear_formula((uint8_t)v);
	/* Positive floats sort in the same order as their bit patterns, so a binary
	   search over the bit patterns from 0 to 1.0f finds each threshold. */
	const uint32_t one = 0x3F800000u;
	s_threshold[0]     = 0.0f;
	for (int k = 1; k < 256; ++k) {
		uint32_t low = 0, high = one;
		while (low < high) {
			const uint32_t middle = low + (high - low) / 2;
			if (linear_to_srgb8_formula(float_of_bits(middle)) >= k)
				high = middle;
			else
				low = middle + 1;
		}
		s_threshold[k] = float_of_bits(low);
	}
}

float srgb8_to_linear(uint8_t v) {
	pthread_once(&s_once, fill_tables);
	return s_linear[v];
}

uint8_t linear_to_srgb8(float c) {
	pthread_once(&s_once, fill_tables);
	c         = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	int value = 0;
	for (int step = 128; step > 0; step /= 2)
		if (c >= s_threshold[value + step])
			value += step;
	return (uint8_t)value;
}
