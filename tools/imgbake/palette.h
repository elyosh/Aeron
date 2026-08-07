/*
 * palette — XPALETTE accumulator.
 *
 * The engine's screen palette is composed of every PLTT that has been
 * applied: each PLTT writes its slot range, leaving other slots
 * untouched. palette_black initializes a fresh buffer (all black, slot
 * 0 transparent) — that's the engine's "blank" state. palette_overlay
 * writes one PLTT's range on top.
 *
 * PLTT format:
 *   u8 start_slot
 *   u8 end_slot     (inclusive)
 *   u8[3] rgb       per slot
 */
#ifndef IMGBAKE_PALETTE_H
#define IMGBAKE_PALETTE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t rgba[256][4]; /* slot 0 has alpha 0 (transparent) */
	int start;
	int len;
} Palette;

void palette_black(Palette *pal);
bool palette_overlay(Palette *pal, const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
