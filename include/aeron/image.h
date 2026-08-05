#ifndef AERON_IMAGE_H
#define AERON_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronImageAlphaMode {
	AERON_IMAGE_ALPHA_STRAIGHT,
	AERON_IMAGE_ALPHA_PREMULTIPLIED,
} AeronImageAlphaMode;

void Aeron_ImagePremultiplyRgba8(uint8_t* rgba, size_t pixel_count);
/* Returns a malloc-owned nearest-neighbor enlargement of an RGBA8 image. */
uint8_t* Aeron_ImageUpscaleNearestRgba8(const uint8_t* source, int width, int height, int scale,
										int* out_width, int* out_height);
/* Returns a malloc-owned 2x box-filtered image. Each output dimension is
 * `max(1, input / 2)`. */
uint8_t* Aeron_ImageDownsampleRgba8(const uint8_t* source, int width, int height, int* out_width,
									int* out_height);

#ifdef __cplusplus
}
#endif

#endif
