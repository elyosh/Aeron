#ifndef AERON_ASSET_DECODE_TYPES_H
#define AERON_ASSET_DECODE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronDecodeError {
	int  code;
	char message[256];
} AeronDecodeError;

/* Views borrow the input blob. Decoded images and atlases own their arrays.
 * Decode outputs must be empty; failure leaves them empty and safe to free. */
typedef struct AeronByteSpan {
	const uint8_t* data;
	size_t         size;
} AeronByteSpan;

typedef struct AeronIndexedFrame {
	uint8_t* indices;
	uint8_t* coverage;
	uint16_t width, height;
	int16_t  anchor_x, anchor_y;
	uint16_t frame_index;
	/* Explicit RGBA bytes, independent of host integer byte order. */
	uint8_t  palette[256][4];
	uint16_t palette_count;
} AeronIndexedFrame;

typedef struct AeronIndexedFrames {
	AeronIndexedFrame* frames;
	uint16_t           count;
} AeronIndexedFrames;

typedef struct AeronDecodedGlyph {
	/* Both coverage planes use the same atlas rectangle. */
	uint16_t x, y, width, height, advance;
	/* ABP: offset in the glyph-data blob; bitmap fonts: offset in the file. */
	uint32_t source_offset;
} AeronDecodedGlyph;

typedef struct AeronDecodedFont {
	uint8_t*           foreground;
	uint8_t*           shadow;
	AeronDecodedGlyph* glyphs;
	uint16_t           width, height;
	uint16_t           first_char, glyph_count;
	uint16_t           cell_width, cell_height, baseline;
} AeronDecodedFont;

void AeronIndexedFrame_Free(AeronIndexedFrame* frame);
void AeronIndexedFrames_Free(AeronIndexedFrames* frames);
void AeronDecodedFont_Free(AeronDecodedFont* font);

#ifdef __cplusplus
}
#endif
#endif
