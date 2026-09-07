#ifndef AERON_ASSET_PNL_H
#define AERON_ASSET_PNL_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef AeronIndexedFrame AeronPnlBitmap;

typedef struct AeronPnlList {
	AeronByteSpan* bitmaps;
	uint32_t       count;
} AeronPnlList;

/* List views borrow the input. Clean EOF can shorten declared_count. */
bool AeronPnl_Parse(const void* bytes, size_t size, uint32_t declared_count, AeronPnlList* out,
					AeronDecodeError* error);
void AeronPnl_Free(AeronPnlList* list);
bool AeronPnl_Measure(const void* bytes, size_t size, int* width, int* height, size_t* consumed,
					  AeronDecodeError* error);
/* Palette and color-key policy are supplied by the game. No embedded palette. */
bool AeronPnl_Decode(const void* bytes, size_t size, AeronPnlBitmap* out, AeronDecodeError* error);
#ifdef __cplusplus
}
#endif
#endif
