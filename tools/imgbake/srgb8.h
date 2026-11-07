#ifndef IMGBAKE_SRGB8_H
#define IMGBAKE_SRGB8_H

/*
 * Converts between 8-bit sRGB values and linear floats with lookup tables.
 * The results are the same as the powf formulas, without a powf call for
 * each value.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the linear value of the 8-bit sRGB value v. */
float srgb8_to_linear(uint8_t v);

/* Returns the 8-bit sRGB value of the linear value c, rounded to the nearest
 * value. c is clamped to [0, 1] first. */
uint8_t linear_to_srgb8(float c);

#ifdef __cplusplus
}
#endif

#endif
