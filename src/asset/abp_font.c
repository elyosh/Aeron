#include "aeron/asset/abp_font.h"
#include "decode_internal.h"
#include <math.h>

#define ABP_HEADER_SIZE 0x60bu
#define FONT_COLUMNS 16
#define FONT_GUTTER 1

static uint8_t font_alpha(uint8_t value) {
	float encoded = (float)((value & 0x1fu) + 1u) / 32.0f;
	float linear  = encoded <= 0.04045f ? encoded / 12.92f : powf((encoded + 0.055f) / 1.055f, 2.4f);
	return (uint8_t)(linear * 255.0f + 0.5f);
}

static int decode_glyph(const uint8_t* data, const uint8_t* end, int width, int height, uint8_t* alpha) {
	memset(alpha, 0, (size_t)width * height);
	const uint8_t* row = data;
	for (int y = 0; y < height; y++) {
		if ((size_t)(end - row) < 4)
			return 0;
		uint32_t row_size = decode_u32(row);
		if (row_size <= 4 || row_size > (uint32_t)(end - row))
			return 0;
		const uint8_t* p       = row + 4;
		const uint8_t* row_end = row + row_size;
		size_t         x       = 0;
		while (p < row_end) {
			uint8_t token = *p++;
			if (token == 0x80)
				break;
			if (token & 0x80u) {
				int length = token & 0x7f;
				if (length > row_end - p)
					return 0;
				for (int i = 0; i < length && x + i < width; i++)
					alpha[(size_t)y * width + x + i] = font_alpha(p[i]);
				p += length;
				x += length;
			} else if (token & 0x40u) {
				x += token & 0x3f;
			} else {
				if (!token || p >= row_end)
					return 0;
				uint8_t value = font_alpha(*p++);
				for (int i = 0; i < token && x + i < width; i++)
					alpha[(size_t)y * width + x + i] = value;
				x += token;
			}
		}
		row = row_end;
	}
	return 1;
}

bool AeronAbpFont_Decode(const void* data, size_t size, AeronDecodedFont* out, AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!bytes || !out || size < ABP_HEADER_SIZE || size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 100, "invalid ABP input");
	uint32_t blob_size = decode_u32(bytes);
	if (blob_size > size - ABP_HEADER_SIZE)
		return decode_error(error, 100, "truncated ABP glyph data");
	int cell_width  = 1;
	int cell_height = 1;
	for (int i = 0; i < 256; i++) {
		if (bytes[0x504 + i] > cell_width)
			cell_width = bytes[0x504 + i];
		if (bytes[0x404 + i] > cell_height)
			cell_height = bytes[0x404 + i];
	}
	int stride_width  = cell_width + 2 * FONT_GUTTER;
	int stride_height = cell_height + 2 * FONT_GUTTER;
	out->width        = FONT_COLUMNS * stride_width;
	out->height       = 16 * stride_height;
	out->cell_width   = cell_width;
	out->cell_height  = cell_height;
	out->baseline     = cell_height;
	out->first_char   = 0;
	out->glyph_count  = 256;
	out->foreground   = (uint8_t*)calloc((size_t)out->width * out->height, 1);
	out->glyphs       = (AeronDecodedGlyph*)calloc(256, sizeof *out->glyphs);
	uint8_t* alpha    = (uint8_t*)malloc((size_t)cell_width * cell_height);
	if (!out->foreground || !out->glyphs || !alpha)
		goto oom;
	const uint8_t* blob     = bytes + ABP_HEADER_SIZE;
	const uint8_t* blob_end = blob + blob_size;
	uint8_t        spacing  = bytes[0x609];
	for (int glyph = 0; glyph < 256; glyph++) {
		int                width  = bytes[0x504 + glyph];
		int                height = bytes[0x404 + glyph];
		uint32_t           offset = decode_u32(bytes + 4 + 4 * glyph);
		AeronDecodedGlyph* metric = &out->glyphs[glyph];
		metric->x                 = (uint16_t)((glyph % 16) * stride_width + FONT_GUTTER);
		metric->y                 = (uint16_t)((glyph / 16) * stride_height + FONT_GUTTER);
		metric->width             = (uint16_t)width;
		metric->height            = (uint16_t)height;
		metric->advance           = (uint16_t)(width + spacing);
		metric->source_offset     = offset;
		if (!width || !height)
			continue;
		if (offset >= blob_size || !decode_glyph(blob + offset, blob_end, width, height, alpha))
			goto malformed;
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				uint8_t coverage = alpha[(size_t)y * width + x];
				out->foreground[((size_t)metric->y + y) * out->width + metric->x + x] = coverage;
			}
		}
	}
	free(alpha);
	return 1;

oom:
	free(alpha);
	AeronDecodedFont_Free(out);
	return decode_error(error, 100, "ABP allocation failed");
malformed:
	free(alpha);
	AeronDecodedFont_Free(out);
	return decode_error(error, 100, "malformed ABP glyph data");
}
