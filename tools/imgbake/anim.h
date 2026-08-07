/*
 * anim — ANIM (multi-frame DELT) decoder.
 *
 * Layout:
 *   u16 frame_count
 *   for each frame: u32 length, then `length` bytes of DELT data.
 *   length == 0 means an empty frame (compositor preserves prior state).
 */
#ifndef IMGBAKE_ANIM_H
#define IMGBAKE_ANIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "delt.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int count;
	Image8 *frames;      /* count entries; frames[i].pixels == NULL for empty */
} AnimImage;

void anim_free(AnimImage *a);

bool decode_anim(AnimImage *out, const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
