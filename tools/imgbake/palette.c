#include "palette.h"

#include <string.h>

void palette_black(Palette *pal) {
	memset(pal, 0, sizeof(*pal));
	pal->start = 0;
	pal->len = 256;
	for (int i = 0; i < 256; i++) {
		pal->rgba[i][0] = 0;
		pal->rgba[i][1] = 0;
		pal->rgba[i][2] = 0;
		pal->rgba[i][3] = (i == 0) ? 0 : 255;
	}
}

bool palette_overlay(Palette *pal, const uint8_t *data, uint32_t size) {
	if (size < 2)
		return false;
	uint8_t pal_start = data[0];
	uint8_t pal_end = data[1];
	if (pal_end < pal_start)
		return false;
	int n = pal_end - pal_start + 1;
	if ((uint32_t)(2 + n * 3) > size)
		return false;

	const uint8_t *p = data + 2;
	for (int i = 0; i < n; i++) {
		int slot = pal_start + i;
		/* PLTT stores 8-bit RGB. The engine downshifts (`>> 2`) to feed a
		   6-bit VGA DAC, then a modern shell upscales back. We skip the
		   lossy round-trip and write the 8-bit value directly. */
		pal->rgba[slot][0] = p[0];
		pal->rgba[slot][1] = p[1];
		pal->rgba[slot][2] = p[2];
		pal->rgba[slot][3] = (slot == 0) ? 0 : 255;
		p += 3;
	}
	return true;
}
