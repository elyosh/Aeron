#include "aeron/asset/bmp.h"
#include "decode_internal.h"

static int bmp_decode_rle8(const uint8_t* data, size_t size, uint8_t* indices, int width, int height,
						   int top_down) {
	const uint8_t* p           = data;
	const uint8_t* end         = data + size;
	const int64_t  pixels      = (int64_t)width * height;
	const int64_t  row_step    = top_down ? width : -width;
	int64_t        row_start   = top_down ? 0 : pixels - width;
	int64_t        destination = row_start;
	/* Original XvT assets use deltas and runs beyond the nominal row width.
	 * Keep the row anchor independent of the linear write cursor: only EOL
	 * resets the column, and every write must stay within the image allocation. */
	while (p < end) {
		if ((size_t)(end - p) < 2)
			return 0;
		const int count = *p++;
		const int value = *p++;
		if (count) {
			if (destination < 0 || destination > pixels || count > pixels - destination)
				return 0;
			memset(indices + (size_t)destination, value, (size_t)count);
			destination += count;
		} else if (value == 0) {
			row_start += row_step;
			destination = row_start;
			if (row_start < 0 || row_start >= pixels)
				return 1;
		} else if (value == 1) {
			return 1;
		} else if (value == 2) {
			if ((size_t)(end - p) < 2)
				return 0;
			const int dx = *p++;
			const int dy = *p++;
			row_start += row_step * dy;
			destination += dx + row_step * dy;
			if (destination < 0 || destination > pixels)
				return 0;
		} else {
			const size_t literal = (size_t)value;
			const size_t encoded = literal + (literal & 1u);
			if (encoded > (size_t)(end - p) || destination < 0 || destination > pixels ||
				value > pixels - destination)
				return 0;
			memcpy(indices + (size_t)destination, p, literal);
			p += encoded;
			destination += value;
		}
	}
	return 0;
}

bool AeronBmp_Decode(const void* data, size_t size, AeronIndexedFrame* out, AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!bytes || !out || size < 54 || size > AERON_DECODE_MAX_ENTRY_SIZE || bytes[0] != 'B' ||
		bytes[1] != 'M')
		return decode_error(error, 110, "invalid BMP input");
	uint32_t pixel_offset = decode_u32(bytes + 10);
	uint32_t dib_size     = decode_u32(bytes + 14);
	if (dib_size < 40 || dib_size > size - 14u)
		return decode_error(error, 110, "unsupported BMP header");
	int      width         = decode_i32(bytes + 18);
	int      signed_height = decode_i32(bytes + 22);
	uint16_t planes        = decode_u16(bytes + 26);
	uint16_t bpp           = decode_u16(bytes + 28);
	uint32_t compression   = decode_u32(bytes + 30);
	if (width <= 0 || width > 4096 || signed_height == 0 || signed_height < -4096 || signed_height > 4096 ||
		planes != 1 || (compression != 0 && !(compression == 1 && bpp == 8)) || (bpp != 4 && bpp != 8))
		return decode_error(error, 110, "unsupported BMP layout");
	int      height      = signed_height < 0 ? -signed_height : signed_height;
	uint32_t color_count = decode_u32(bytes + 46);
	if (!color_count)
		color_count = 1u << bpp;
	if (color_count > 256 || color_count * 4u > size - 14u - dib_size || pixel_offset > size ||
		pixel_offset < (size_t)14 + dib_size + color_count * 4u)
		return decode_error(error, 110, "malformed BMP palette");
	uint8_t        palette[1024] = { 0 };
	const uint8_t* disk_palette  = bytes + 14u + dib_size;
	for (uint32_t i = 0; i < color_count; i++) {
		palette[4 * i]     = disk_palette[4 * i + 2];
		palette[4 * i + 1] = disk_palette[4 * i + 1];
		palette[4 * i + 2] = disk_palette[4 * i];
		palette[4 * i + 3] = 255;
	}
	uint8_t* indices = (uint8_t*)malloc((size_t)width * height);
	if (!indices)
		return decode_error(error, 110, "BMP allocation failed");
	memset(indices, 0, (size_t)width * height);
	if (compression == 1) {
		if (!bmp_decode_rle8(bytes + pixel_offset, size - pixel_offset, indices, width, height,
							 signed_height < 0)) {
			free(indices);
			return decode_error(error, 110, "malformed BMP RLE8 pixels");
		}
	} else {
		const size_t row_stride = ((size_t)width * bpp + 31u) / 32u * 4u;
		if (row_stride > SIZE_MAX / (size_t)height || row_stride * (size_t)height > size - pixel_offset) {
			free(indices);
			return decode_error(error, 110, "truncated BMP pixels");
		}
		for (int y = 0; y < height; y++) {
			int            source_y = signed_height > 0 ? height - 1 - y : y;
			const uint8_t* row      = bytes + pixel_offset + (size_t)source_y * row_stride;
			for (int x = 0; x < width; x++)
				indices[(size_t)y * width + x] =
					bpp == 8 ? row[x] : (uint8_t)((row[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0f);
		}
	}
	for (size_t i = 0; i < (size_t)width * height; ++i) {
		if (indices[i] >= color_count) {
			free(indices);
			return decode_error(error, 111, "BMP palette index is invalid");
		}
	}
	uint8_t* coverage = malloc((size_t)width * height);
	if (!coverage) {
		free(indices);
		return decode_error(error, 112, "BMP allocation failed");
	}
	memset(coverage, 255, (size_t)width * height);
	out->indices       = indices;
	out->coverage      = coverage;
	out->width         = (uint16_t)width;
	out->height        = (uint16_t)height;
	out->palette_count = (uint16_t)color_count;
	memcpy(out->palette, palette, sizeof palette);
	return true;
}
