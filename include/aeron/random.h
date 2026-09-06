#ifndef AERON_RANDOM_H
#define AERON_RANDOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills data with OS cryptographic randomness. Thread-safe; requires no Aeron
 * initialization. Returns 1 on success, 0 on invalid arguments or OS failure.
 * Zero size succeeds, including with NULL data. On failure, discard the buffer.
 * On Linux, an unready entropy source fails immediately. */
int Aeron_RandomBytes(void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
