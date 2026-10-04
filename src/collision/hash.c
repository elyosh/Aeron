#include "aeron/collision/hash.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

uint64_t Aeron_Xxh64(const void* data, size_t size, uint64_t seed) { return XXH64(data, size, seed); }
