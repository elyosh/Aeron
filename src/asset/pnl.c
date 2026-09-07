#include "aeron/asset/pnl.h"
#include "decode_internal.h"

static bool AeronPnl_WalkRow(const uint8_t** cursor, const uint8_t* end, int* out_width, bool* truncated) {
	const uint8_t* p     = *cursor;
	int            width = 0;
	*truncated           = false;
	while (p < end) {
		const uint8_t op = *p++;
		if (op <= 0xfa) {
			width += (op & 3) + 1;
			if (width > 4096) {
				*truncated = true;
				return false;
			}
			continue;
		}
		if (op == 0xfb) {
			if (p == end) {
				*truncated = true;
				return false;
			}
			++p;
			continue;
		}
		if (op == 0xfc || op == 0xfd) {
			if ((size_t)(end - p) < 2) {
				*truncated = true;
				return false;
			}
			width += (op == 0xfc ? p[1] : p[0]) + 1;
			if (width > 4096) {
				*truncated = true;
				return false;
			}
			p += 2;
			continue;
		}
		*out_width = width;
		*cursor    = p;
		return op == 0xfe;
	}
	*truncated = true;
	return false;
}

static bool pnl_measure(const void* data, size_t size, int* out_width, int* out_height, size_t* out_consumed,
						bool allow_empty, AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (!bytes || !size || size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 40, "empty PNL bitmap stream");
	const uint8_t *cursor = bytes, *end = bytes + size;
	int            max_width = 0, rows = 0;
	for (;;) {
		const uint8_t* row_start = cursor;
		int            width     = 0;
		bool           truncated = false;
		const bool     more      = AeronPnl_WalkRow(&cursor, end, &width, &truncated);
		if (truncated)
			return decode_error(error, 41, "truncated PNL bitmap at byte %zu", (size_t)(cursor - bytes));
		if (width > max_width)
			max_width = width;
		if (more || cursor - row_start != 1 || width != 0)
			++rows;
		if (rows > 4096 || max_width > 4096)
			return decode_error(error, 42, "PNL bitmap dimensions exceed 4096");
		if (!more)
			break;
	}
	if (!allow_empty && (max_width <= 0 || rows <= 0))
		return decode_error(error, 43, "PNL bitmap has no pixels");
	if (out_width)
		*out_width = max_width;
	if (out_height)
		*out_height = rows;
	if (out_consumed)
		*out_consumed = (size_t)(cursor - bytes);
	return true;
}

static void AeronPnl_WritePixel(AeronPnlBitmap* bitmap, int x, int y, uint8_t color) {
	if (x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height)
		return;
	const size_t pixel      = (size_t)y * bitmap->width + x;
	bitmap->indices[pixel]  = color;
	bitmap->coverage[pixel] = 255;
}

static bool pnl_rasterize(const void* data, size_t size, AeronPnlBitmap* bitmap, AeronDecodeError* error) {
	const uint8_t *p = data, *end = p + size;
	int            x = 0, y = 0;
	uint8_t        base = 0;
	while (p < end) {
		const uint8_t op = *p++;
		if (op <= 0xfa) {
			const uint8_t color = (uint8_t)((op >> 2) + base);
			for (int count = (op & 3) + 1; count--; ++x)
				AeronPnl_WritePixel(bitmap, x, y, color);
			continue;
		}
		if (op == 0xfb) {
			if (p == end)
				return decode_error(error, 45, "truncated PNL bitmap 0xFB");
			base = *p++;
			continue;
		}
		if (op == 0xfc) {
			if ((size_t)(end - p) < 2)
				return decode_error(error, 46, "truncated PNL bitmap 0xFC");
			const uint8_t color     = *p++;
			int           remaining = *p++ + 1;
			while (remaining > 0) {
				AeronPnl_WritePixel(bitmap, x++, y, color);
				--remaining;
				if (remaining > 0) {
					AeronPnl_WritePixel(bitmap, x++, y, (uint8_t)(color + 1));
					--remaining;
				}
			}
			continue;
		}
		if (op == 0xfd) {
			if ((size_t)(end - p) < 2)
				return decode_error(error, 47, "truncated PNL bitmap 0xFD");
			int           count = *p++ + 1;
			const uint8_t color = *p++;
			while (count--)
				AeronPnl_WritePixel(bitmap, x++, y, color);
			continue;
		}
		if (op == 0xff)
			return true;
		if (++y > bitmap->height)
			return decode_error(error, 48, "PNL bitmap has too many rows");
		x = 0;
	}
	return decode_error(error, 49, "PNL bitmap stream has no terminator");
}

bool AeronPnl_Measure(const void* bytes, size_t size, int* width, int* height, size_t* consumed,
					  AeronDecodeError* error) {
	return pnl_measure(bytes, size, width, height, consumed, false, error);
}

bool AeronPnl_Decode(const void* bytes, size_t size, AeronPnlBitmap* out, AeronDecodeError* error) {
	if (!out)
		return decode_error(error, 44, "invalid PNL bitmap output");
	memset(out, 0, sizeof *out);
	int width, height;
	if (!AeronPnl_Measure(bytes, size, &width, &height, NULL, error))
		return false;
	out->indices  = calloc((size_t)width * height, 1);
	out->coverage = calloc((size_t)width * height, 1);
	if (!out->indices || !out->coverage) {
		AeronIndexedFrame_Free(out);
		return decode_error(error, 31, "PNL bitmap allocation failed");
	}
	out->width  = (uint16_t)width;
	out->height = (uint16_t)height;
	if (!pnl_rasterize(bytes, size, out, error)) {
		AeronIndexedFrame_Free(out);
		return false;
	}
	return true;
}

void AeronPnl_Free(AeronPnlList* list) {
	if (!list)
		return;
	free(list->bitmaps);
	memset(list, 0, sizeof *list);
}

bool AeronPnl_Parse(const void* data, size_t size, uint32_t declared_count, AeronPnlList* out,
					AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!bytes || !out || !declared_count || declared_count > 4096 || size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 30, "invalid PNL bitmap list");
	out->bitmaps = calloc(declared_count, sizeof *out->bitmaps);
	if (!out->bitmaps)
		return decode_error(error, 31, "PNL list allocation failed");
	size_t offset = 0;
	for (uint32_t i = 0; i < declared_count && offset < size; ++i) {
		size_t consumed;
		/* Walk opcodes: 0xFF inside a run operand is not a terminator. */
		if (!pnl_measure(bytes + offset, size - offset, NULL, NULL, &consumed, true, error)) {
			AeronPnl_Free(out);
			return false;
		}
		out->bitmaps[out->count++] = (AeronByteSpan) { bytes + offset, consumed };
		offset += consumed;
	}
	if (!out->count) {
		AeronPnl_Free(out);
		return decode_error(error, 32, "PNL contains no complete bitmaps");
	}
	return true;
}
