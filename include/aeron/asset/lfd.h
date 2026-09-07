#ifndef AERON_ASSET_LFD_H
#define AERON_ASSET_LFD_H
#include "aeron/asset/decode_types.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AERON_LFD_FOURCC(a, b, c, d)                                                                         \
	((uint32_t)(uint8_t)(a) << 24 | (uint32_t)(uint8_t)(b) << 16 | (uint32_t)(uint8_t)(c) << 8 |             \
	 (uint32_t)(uint8_t)(d))

typedef struct AeronLfdEntry {
	uint32_t       type;
	char           name[9];
	const uint8_t* data;
	size_t         size;
} AeronLfdEntry;

typedef struct AeronLfd {
	AeronLfdEntry* entries;
	uint32_t       count;
} AeronLfd;

/* Sequential 16-byte resource headers; entries borrow the input blob. */
bool AeronLfd_Parse(const void* bytes, size_t size, AeronLfd* out, AeronDecodeError* error);
/* Returns NULL for a missing or ambiguous (duplicate) type. */
const AeronLfdEntry* AeronLfd_Find(const AeronLfd* lfd, uint32_t type);
void                 AeronLfd_Free(AeronLfd* lfd);
#ifdef __cplusplus
}
#endif
#endif
