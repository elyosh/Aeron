#ifndef AERON_ASSET_BMP_H
#define AERON_ASSET_BMP_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 4/8-bit BMP and RLE8, in top-down output order. Coverage includes index zero;
 * the caller selects any color key. RLE gaps retain index zero. */
bool AeronBmp_Decode(const void* bytes, size_t size, AeronIndexedFrame* out, AeronDecodeError* error);
#ifdef __cplusplus
}
#endif
#endif
