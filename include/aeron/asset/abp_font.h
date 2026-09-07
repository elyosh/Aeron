#ifndef AERON_ASSET_ABP_FONT_H
#define AERON_ASSET_ABP_FONT_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* All 256 glyphs, with the original coverage transfer function and spacing.
 * ABP contains foreground coverage only; shadow is NULL. */
bool AeronAbpFont_Decode(const void* bytes, size_t size, AeronDecodedFont* out, AeronDecodeError* error);
#ifdef __cplusplus
}
#endif
#endif
