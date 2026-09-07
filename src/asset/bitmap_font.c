#include "aeron/asset/bitmap_font.h"
#include "decode_internal.h"

bool AeronBitmapFont_Decode(const void* data, size_t size, uint8_t row_bytes, uint16_t first_char,
							AeronDecodedFont* out, AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!bytes || !out || (row_bytes != 1 && row_bytes != 4) || size < 2 ||
		size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 80, "invalid cockpit font input");
	const uint32_t glyph_height = bytes[1];
	if (!glyph_height || glyph_height > 64)
		return decode_error(error, 81, "invalid cockpit font height");
	const uint32_t glyph_stride = 2 + 2 * glyph_height * row_bytes;
	if (size % glyph_stride)
		return decode_error(error, 82, "cockpit font size does not match its variant");
	const uint32_t count = (uint32_t)(size / glyph_stride);
	if (!count || count > 256 || count > 65536u - first_char)
		return decode_error(error, 83, "invalid cockpit font glyph count");
	uint32_t max_width = 0;
	for (uint32_t index = 0; index < count; ++index) {
		const uint8_t width = bytes[index * glyph_stride];
		if (width > row_bytes * 8 || bytes[index * glyph_stride + 1] != glyph_height)
			return decode_error(error, 84, "cockpit font glyph %u is invalid", index);
		if (width > max_width)
			max_width = width;
	}
	if (!max_width)
		return decode_error(error, 85, "cockpit font is empty");

	/* Keep filtering footprints inside transparent texels. The sampled
	 * cell retains its previous dimensions, while an external guard
	 * surrounds every side for linear and mip-filtered sampling. */
	enum { atlas_columns = 16, guard = 2 };

	const uint32_t cell_width    = max_width + 1;
	const uint32_t stride_width  = cell_width + 2 * guard;
	const uint32_t stride_height = glyph_height + 2 * guard;
	const uint32_t atlas_rows    = (count + atlas_columns - 1) / atlas_columns;
	const uint32_t atlas_width   = atlas_columns * stride_width;
	const uint32_t atlas_height  = atlas_rows * stride_height;
	if (atlas_width > UINT16_MAX || atlas_height > UINT16_MAX ||
		(size_t)atlas_width * atlas_height > SIZE_MAX / 4)
		return decode_error(error, 86, "cockpit font atlas is too large");
	uint8_t*           foreground = calloc((size_t)atlas_width * atlas_height, 1);
	uint8_t*           shadow     = calloc((size_t)atlas_width * atlas_height, 1);
	AeronDecodedGlyph* glyphs     = calloc(count, sizeof *glyphs);
	if (!foreground || !shadow || !glyphs) {
		free(foreground);
		free(shadow);
		free(glyphs);
		return decode_error(error, 87, "cockpit font allocation failed");
	}
	for (uint32_t index = 0; index < count; ++index) {
		const uint8_t* slot     = bytes + index * glyph_stride;
		const uint32_t origin_x = (index % atlas_columns) * stride_width + guard;
		const uint32_t origin_y = (index / atlas_columns) * stride_height + guard;
		glyphs[index]           = (AeronDecodedGlyph) {
			.x             = (uint16_t)origin_x,
			.y             = (uint16_t)origin_y,
			.width         = (uint16_t)cell_width,
			.height        = (uint16_t)glyph_height,
			.advance       = slot[0],
			.source_offset = index * glyph_stride,
		};
		for (uint32_t y = 0; y < glyph_height; ++y) {
			const uint8_t* row = slot + 2 + y * row_bytes * 2;
			for (uint32_t x = 0; x < slot[0]; ++x) {
				const uint32_t byte_index = row_bytes - 1 - (x >> 3);
				const size_t   pixel      = (size_t)(origin_y + y) * atlas_width + origin_x + x;
				foreground[pixel]         = ((row[byte_index] >> (7 - (x & 7))) & 1) ? 255 : 0;
				shadow[pixel]             = ((row[row_bytes + byte_index] >> (7 - (x & 7))) & 1) ? 255 : 0;
			}
		}
	}
	out->foreground  = foreground;
	out->shadow      = shadow;
	out->glyphs      = glyphs;
	out->width       = (uint16_t)atlas_width;
	out->height      = (uint16_t)atlas_height;
	out->first_char  = first_char;
	out->glyph_count = (uint16_t)count;
	out->cell_width  = (uint16_t)cell_width;
	out->cell_height = (uint16_t)glyph_height;
	out->baseline    = (uint16_t)glyph_height;
	return true;
}
