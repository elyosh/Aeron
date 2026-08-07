/*
 * delt — DELT (and RAW) sprite decoder.
 *
 * Layout:
 *   int16  left
 *   int16  top
 *   int16  right_minus_one    (width  = right_minus_one  - left + 1)
 *   int16  bottom_minus_one   (height = bottom_minus_one - top  + 1)
 *   stream of records: { u16 ctrl, i16 x, i16 y, [data] }, ctrl == 0 ends.
 *     ctrl >> 1 = byte count; ctrl & 1 = 1 → RLE-compressed line.
 *     RLE control byte: bit 0 = run/literal, bits 1..7 = length.
 *
 * (x, y) are absolute coords relative to the actor origin, which equals
 * the bbox top-left.
 */
#ifndef IMGBAKE_DELT_H
#define IMGBAKE_DELT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int left, top;       /* origin in classic coords */
	int width, height;
	uint8_t *pixels;     /* width*height bytes (palette indices) */
} Image8;

void image_free(Image8 *img);

bool decode_delt(Image8 *out, const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
