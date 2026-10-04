#ifndef AERON_COLLISION_HASH_H
#define AERON_COLLISION_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XXH64 of `size` bytes, the hash the cooked flight data uses. */
uint64_t Aeron_Xxh64(const void* data, size_t size, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif
