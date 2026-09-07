#ifndef AERON_ASSET_BITMAP_FONT_H
#define AERON_ASSET_BITMAP_FONT_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Width/height records followed by interleaved foreground/shadow mask rows.
 * row_bytes selects the one-byte or four-byte little-endian mask variant. */
bool AeronBitmapFont_Decode(const void* bytes, size_t size, uint8_t row_bytes, uint16_t first_char,
							AeronDecodedFont* out, AeronDecodeError* error);
#ifdef __cplusplus
}
#endif
#endif
