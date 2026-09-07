#include "aeron/asset/act.h"
#include "decode_internal.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t AeronAct_RunMask[16] = {
	0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};
static const uint8_t AeronAct_RunShift[16] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3, 4, 5, 6,
};

static bool AeronAct_RangeValid(size_t size, uint32_t offset, size_t bytes) {
	return offset <= size && bytes <= size - offset;
}

static bool AeronAct_DecodeFrame(const uint8_t* blob, size_t size, uint32_t sub_offset, uint16_t frame_index,
								 AeronIndexedFrame* out, AeronDecodeError* error) {
	if (!AeronAct_RangeValid(size, sub_offset, 44))
		return decode_error(error, 60, "ACT frame %u header is truncated", frame_index);
	const uint32_t rgb_relative  = decode_u32(blob + sub_offset + 4);
	const uint32_t rle_relative  = decode_u32(blob + sub_offset + 8);
	const uint32_t width         = decode_u32(blob + sub_offset + 16);
	const uint32_t height        = decode_u32(blob + sub_offset + 20);
	const uint32_t split         = decode_u32(blob + sub_offset + 32);
	const uint32_t bit_depth     = decode_u32(blob + sub_offset + 36);
	const uint32_t palette_count = decode_u32(blob + sub_offset + 40);
	if (bit_depth != 24)
		return decode_error(error, 72, "ACT frame %u requires a 24-bit palette", frame_index);
	if (!width || !height || width > 4096 || height > 4096 || !palette_count || palette_count > 256)
		return decode_error(error, 61, "ACT frame %u dimensions or palette are invalid", frame_index);
	if (rgb_relative > UINT32_MAX - sub_offset || rle_relative > UINT32_MAX - sub_offset)
		return decode_error(error, 62, "ACT frame %u offset overflows", frame_index);
	const uint32_t rgb_offset = sub_offset + rgb_relative;
	const uint32_t rle_offset = sub_offset + rle_relative;
	if (!AeronAct_RangeValid(size, rgb_offset, (size_t)palette_count * 4) ||
		!AeronAct_RangeValid(size, rle_offset, 16))
		return decode_error(error, 63, "ACT frame %u data is truncated", frame_index);
	if ((size_t)width > SIZE_MAX / height || (size_t)width * height > SIZE_MAX / 4)
		return decode_error(error, 64, "ACT frame %u allocation overflows", frame_index);
	uint8_t* indices  = calloc((size_t)width * height, 1);
	uint8_t* coverage = calloc((size_t)width * height, 1);
	if (!indices || !coverage) {
		free(indices);
		free(coverage);
		return decode_error(error, 65, "ACT frame allocation failed");
	}
	out->indices       = indices;
	out->coverage      = coverage;
	out->palette_count = (uint16_t)palette_count;
	for (uint32_t i = 0; i < palette_count; ++i) {
		memcpy(out->palette[i], blob + rgb_offset + i * 4, 3);
		out->palette[i][3] = 255;
	}
	out->width           = (uint16_t)width;
	out->height          = (uint16_t)height;
	out->anchor_x        = decode_i16(blob + rle_offset);
	out->anchor_y        = (int16_t)-decode_i16(blob + rle_offset + 4);
	out->frame_index     = frame_index;
	const uint8_t mask   = AeronAct_RunMask[split & 15];
	const uint8_t shift  = AeronAct_RunShift[split & 15];
	size_t        cursor = (size_t)rle_offset + 16;
	for (uint32_t y = 0; y < height; ++y) {
		if (cursor >= size)
			goto truncated;
		if (blob[cursor] == 0xff)
			return true;
		uint16_t base       = 0;
		uint32_t x          = 0;
		bool     terminated = false;
		while (cursor < size) {
			const uint8_t op = blob[cursor++];
			if (op == 0xff)
				return true;
			if (op == 0xfe) {
				terminated = true;
				break;
			}
			if (op == 0xfb) {
				if (size - cursor < 2)
					goto truncated;
				base = decode_u16(blob + cursor);
				cursor += 2;
				continue;
			}
			uint32_t count;
			uint8_t  color;
			bool     transparent = false;
			if (op == 0xfc) {
				if (cursor == size)
					goto truncated;
				count       = (uint32_t)blob[cursor++] + 1;
				color       = 0;
				transparent = true;
			} else if (op == 0xfd) {
				if (size - cursor < 2)
					goto truncated;
				count = (uint32_t)blob[cursor++] + 1;
				color = blob[cursor++];
			} else {
				count = (uint32_t)(op & mask) + 1;
				color = (uint8_t)(base + (op >> shift));
			}
			if (count > width - x)
				goto invalid_run;
			if (!transparent) {
				if (color >= palette_count)
					goto invalid_color;
				memset(indices + (size_t)y * width + x, color, count);
				memset(coverage + (size_t)y * width + x, 255, count);
			}
			x += count;
		}
		if (!terminated)
			goto truncated;
	}
	return true;

truncated:
	AeronIndexedFrame_Free(out);
	return decode_error(error, 66, "ACT frame %u RLE is truncated", frame_index);
invalid_run:
	AeronIndexedFrame_Free(out);
	return decode_error(error, 67, "ACT frame %u run exceeds its row", frame_index);
invalid_color:
	AeronIndexedFrame_Free(out);
	return decode_error(error, 68, "ACT frame %u palette index is invalid", frame_index);
}

bool AeronAct_Decode(const void* data, size_t size, AeronIndexedFrames* out, AeronDecodeError* error) {
	const uint8_t* blob = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!blob || !out || size < 52 || size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 69, "invalid ACT payload size");
	const uint32_t table = decode_u32(blob + 16);
	const uint32_t count = decode_u32(blob + 24);
	if (!count || count > 256 || !AeronAct_RangeValid(size, table, (size_t)count * 4))
		return decode_error(error, 70, "invalid ACT frame table");
	AeronIndexedFrame* frames = calloc(count, sizeof *frames);
	if (!frames)
		return decode_error(error, 71, "ACT frame allocation failed");
	out->frames = frames;
	out->count  = (uint16_t)count;
	for (uint32_t index = 0; index < count; ++index) {
		const uint32_t sub_offset = decode_u32(blob + table + index * 4);
		if (!AeronAct_DecodeFrame(blob, size, sub_offset, (uint16_t)index, &frames[index], error)) {
			AeronIndexedFrames_Free(out);
			return false;
		}
	}
	return true;
}
