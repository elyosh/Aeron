#ifndef AERON_ASSET_ACT_H
#define AERON_ASSET_ACT_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Packed ACT images with RGB-plus-unused palettes (24-bit source variant).
 * Transparent runs leave coverage zero independently of their color index. */
bool AeronAct_Decode(const void* bytes, size_t size, AeronIndexedFrames* out, AeronDecodeError* error);
#ifdef __cplusplus
}
#endif
#endif
