#include "aeron/image.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void Aeron_ImagePremultiplyRgba8(uint8_t* rgba, size_t pixel_count) {
	for (size_t i = 0; i < pixel_count; i++) {
		uint8_t* pixel = rgba + i * 4u;
		pixel[0]       = (uint8_t)(((unsigned)pixel[0] * pixel[3] + 127u) / 255u);
		pixel[1]       = (uint8_t)(((unsigned)pixel[1] * pixel[3] + 127u) / 255u);
		pixel[2]       = (uint8_t)(((unsigned)pixel[2] * pixel[3] + 127u) / 255u);
	}
}

uint8_t* Aeron_ImageUpscaleNearestRgba8(const uint8_t* source, int width, int height, int scale,
										int* out_width, int* out_height) {
	if (!source || width <= 0 || height <= 0 || scale <= 0 || !out_width || !out_height ||
		width > INT_MAX / scale || height > INT_MAX / scale)
		return NULL;
	const int scaled_width  = width * scale;
	const int scaled_height = height * scale;
	if ((size_t)scaled_width > SIZE_MAX / (size_t)scaled_height / 4u)
		return NULL;
	uint8_t* result = (uint8_t*)malloc((size_t)scaled_width * scaled_height * 4u);
	if (!result)
		return NULL;
	const size_t row_bytes = (size_t)scaled_width * 4u;
	for (int y = 0; y < height; y++) {
		uint8_t* first_row = result + (size_t)y * scale * row_bytes;
		for (int x = 0; x < width; x++) {
			const uint8_t* pixel = source + ((size_t)y * width + x) * 4u;
			for (int repeat = 0; repeat < scale; repeat++)
				memcpy(first_row + ((size_t)x * scale + repeat) * 4u, pixel, 4u);
		}
		for (int repeat = 1; repeat < scale; repeat++)
			memcpy(first_row + (size_t)repeat * row_bytes, first_row, row_bytes);
	}
	*out_width  = scaled_width;
	*out_height = scaled_height;
	return result;
}

uint8_t* Aeron_ImageDownsampleRgba8(const uint8_t* source, int width, int height, int* out_width,
									int* out_height) {
	if (!source || width <= 0 || height <= 0 || !out_width || !out_height)
		return NULL;
	const int next_width  = width > 1 ? width / 2 : 1;
	const int next_height = height > 1 ? height / 2 : 1;
	uint8_t*  result      = (uint8_t*)malloc((size_t)next_width * next_height * 4u);
	if (!result)
		return NULL;
	for (int y = 0; y < next_height; y++) {
		for (int x = 0; x < next_width; x++) {
			unsigned sum[4]  = { 0, 0, 0, 0 };
			unsigned samples = 0;
			for (int dy = 0; dy < 2; dy++) {
				const int source_y = y * 2 + dy;
				if (source_y >= height)
					continue;
				for (int dx = 0; dx < 2; dx++) {
					const int source_x = x * 2 + dx;
					if (source_x >= width)
						continue;
					const uint8_t* pixel = source + ((size_t)source_y * width + source_x) * 4u;
					for (int channel = 0; channel < 4; channel++)
						sum[channel] += pixel[channel];
					samples++;
				}
			}
			uint8_t* output = result + ((size_t)y * next_width + x) * 4u;
			for (int channel = 0; channel < 4; channel++)
				output[channel] = (uint8_t)((sum[channel] + samples / 2u) / samples);
		}
	}
	*out_width  = next_width;
	*out_height = next_height;
	return result;
}

uint8_t* Aeron_ImageDownsampleStraightAlphaRgba8(const uint8_t* source, int width, int height,
												 int* out_width, int* out_height) {
	if (!source || width <= 0 || height <= 0 || !out_width || !out_height)
		return NULL;
	const int next_width  = width > 1 ? width / 2 : 1;
	const int next_height = height > 1 ? height / 2 : 1;
	uint8_t*  result      = (uint8_t*)malloc((size_t)next_width * next_height * 4u);
	if (!result)
		return NULL;
	for (int y = 0; y < next_height; y++) {
		for (int x = 0; x < next_width; x++) {
			unsigned alpha_sum      = 0;
			unsigned weighted_rgb[3] = { 0, 0, 0 };
			unsigned samples         = 0;
			for (int dy = 0; dy < 2; dy++) {
				const int source_y = y * 2 + dy;
				if (source_y >= height)
					continue;
				for (int dx = 0; dx < 2; dx++) {
					const int source_x = x * 2 + dx;
					if (source_x >= width)
						continue;
					const uint8_t* pixel = source + ((size_t)source_y * width + source_x) * 4u;
					alpha_sum += pixel[3];
					for (int channel = 0; channel < 3; channel++)
						weighted_rgb[channel] += (unsigned)pixel[channel] * pixel[3];
					samples++;
				}
			}
			uint8_t* output = result + ((size_t)y * next_width + x) * 4u;
			for (int channel = 0; channel < 3; channel++) {
				output[channel] = alpha_sum
					? (uint8_t)((weighted_rgb[channel] + alpha_sum / 2u) / alpha_sum)
					: 0;
			}
			output[3] = (uint8_t)((alpha_sum + samples / 2u) / samples);
		}
	}
	*out_width  = next_width;
	*out_height = next_height;
	return result;
}
