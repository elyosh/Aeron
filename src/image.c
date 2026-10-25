#include "aeron/image.h"

#include <limits.h>
#include <math.h>
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
	const size_t row_bytes = (size_t)width * 4u;
	/* Repeating a singleton dimension preserves the two-pixel or one-pixel average. */
	const size_t right_offset  = width > 1 ? 4u : 0u;
	const size_t bottom_offset = height > 1 ? row_bytes : 0u;
	for (int y = 0; y < next_height; ++y) {
		const uint8_t* top    = source + (size_t)y * 2u * row_bytes;
		const uint8_t* bottom = top + bottom_offset;
		uint8_t*       output = result + (size_t)y * next_width * 4u;
		for (int x = 0; x < next_width; ++x) {
			for (int channel = 0; channel < 4; ++channel)
				output[channel] = (uint8_t)(((unsigned)top[channel] + top[right_offset + channel] +
											 bottom[channel] + bottom[right_offset + channel] + 2u) >>
											2);
			top += 2u * right_offset;
			bottom += 2u * right_offset;
			output += 4;
		}
	}
	*out_width  = next_width;
	*out_height = next_height;
	return result;
}

uint8_t* Aeron_ImageDownsampleStraightAlphaRgba8(const uint8_t* source, int width, int height, int* out_width,
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
			unsigned alpha_sum       = 0;
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
				output[channel] =
					alpha_sum ? (uint8_t)((weighted_rgb[channel] + alpha_sum / 2u) / alpha_sum) : 0;
			}
			output[3] = (uint8_t)((alpha_sum + samples / 2u) / samples);
		}
	}
	*out_width  = next_width;
	*out_height = next_height;
	return result;
}

/* The Mitchell-Netravali filter with B = C = 1/3, which is 0 past 2. */
static float image_mitchell(float x) {
	const float B = 1.0f / 3.0f, C = 1.0f / 3.0f;
	x         = fabsf(x);
	if (x < 1.0f)
		return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x + (-18.0f + 12.0f * B + 6.0f * C) * x * x +
				(6.0f - 2.0f * B)) /
			   6.0f;
	if (x < 2.0f)
		return ((-B - 6.0f * C) * x * x * x + (6.0f * B + 30.0f * C) * x * x + (-12.0f * B - 48.0f * C) * x +
				(8.0f * B + 24.0f * C)) /
			   6.0f;
	return 0.0f;
}

/* The source pixels and weights of each output pixel along one axis. */
typedef struct ImageTaps {
	int*   first; /* the first source pixel of each output pixel */
	int*   count;
	float* weights; /* `taps` weights for each output pixel */
	int    taps;
} ImageTaps;

static void image_free_taps(ImageTaps* t) {
	free(t->first);
	free(t->count);
	free(t->weights);
	memset(t, 0, sizeof *t);
}

static int image_build_taps(int in_size, int out_size, ImageTaps* t) {
	const float scale   = (float)in_size / (float)out_size;
	/* When the image shrinks, the filter widens so that every source pixel
	   counts. */
	const float width   = scale > 1.0f ? scale : 1.0f;
	const float support = 2.0f * width;
	t->taps             = (int)ceilf(support * 2.0f) + 1;
	t->first            = (int*)malloc((size_t)out_size * sizeof *t->first);
	t->count            = (int*)malloc((size_t)out_size * sizeof *t->count);
	t->weights          = (float*)malloc((size_t)out_size * (size_t)t->taps * sizeof *t->weights);
	if (!t->first || !t->count || !t->weights) {
		image_free_taps(t);
		return 0;
	}
	for (int o = 0; o < out_size; o++) {
		const float center = ((float)o + 0.5f) * scale - 0.5f;
		int         first  = (int)ceilf(center - support);
		int         last   = (int)floorf(center + support);
		if (last - first + 1 > t->taps)
			last = first + t->taps - 1;
		float* w   = t->weights + (size_t)o * t->taps;
		float  sum = 0.0f;
		for (int i = first; i <= last; i++) {
			w[i - first] = image_mitchell(((float)i - center) / width);
			sum += w[i - first];
		}
		for (int i = first; i <= last; i++)
			w[i - first] /= sum;
		t->first[o] = first;
		t->count[o] = last - first + 1;
	}
	return 1;
}

static float image_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t image_linear_to_srgb(float c) {
	c       = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
	float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

uint8_t* Aeron_ImageResizeSrgbRgba8(const uint8_t* source, int width, int height, int out_width,
									int out_height) {
	if (!source || width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0)
		return NULL;
	float linear[256];
	for (int i = 0; i < 256; i++)
		linear[i] = image_srgb_to_linear((float)i / 255.0f);
	ImageTaps across = { 0 }, down = { 0 };
	float*    rows   = NULL;
	uint8_t*  result = NULL;
	if (!image_build_taps(width, out_width, &across) || !image_build_taps(height, out_height, &down))
		goto done;
	/* The first pass resizes the rows, in linear light with associated alpha;
	   the second resizes the columns. */
	rows   = (float*)malloc((size_t)out_width * height * 4u * sizeof *rows);
	result = (uint8_t*)malloc((size_t)out_width * out_height * 4u);
	if (!rows || !result) {
		free(result);
		result = NULL;
		goto done;
	}
	for (int y = 0; y < height; y++) {
		const uint8_t* row = source + (size_t)y * width * 4u;
		for (int x = 0; x < out_width; x++) {
			const float* w      = across.weights + (size_t)x * across.taps;
			float        sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int k = 0; k < across.count[x]; k++) {
				int i = across.first[x] + k;
				i     = i < 0 ? 0 : i >= width ? width - 1 : i;
				const uint8_t* p     = row + (size_t)i * 4u;
				const float    alpha = (float)p[3] / 255.0f;
				sum[0] += w[k] * linear[p[0]] * alpha;
				sum[1] += w[k] * linear[p[1]] * alpha;
				sum[2] += w[k] * linear[p[2]] * alpha;
				sum[3] += w[k] * alpha;
			}
			memcpy(rows + ((size_t)y * out_width + x) * 4u, sum, sizeof sum);
		}
	}
	for (int y = 0; y < out_height; y++) {
		const float* w = down.weights + (size_t)y * down.taps;
		for (int x = 0; x < out_width; x++) {
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int k = 0; k < down.count[y]; k++) {
				int i = down.first[y] + k;
				i     = i < 0 ? 0 : i >= height ? height - 1 : i;
				const float* p = rows + ((size_t)i * out_width + x) * 4u;
				for (int c = 0; c < 4; c++)
					sum[c] += w[k] * p[c];
			}
			uint8_t*    out   = result + ((size_t)y * out_width + x) * 4u;
			const float alpha = sum[3] < 0.0f ? 0.0f : sum[3] > 1.0f ? 1.0f : sum[3];
			for (int c = 0; c < 3; c++)
				out[c] = alpha > 0.0f ? image_linear_to_srgb(sum[c] / alpha) : 0;
			out[3] = (uint8_t)(alpha * 255.0f + 0.5f);
		}
	}

done:
	free(rows);
	image_free_taps(&across);
	image_free_taps(&down);
	return result;
}
