#include "aeron/random.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>
#else
#include <errno.h>
#include <sys/random.h>
#include <unistd.h>
#endif

int Aeron_RandomBytes(void* data, size_t size) {
	unsigned char* bytes = (unsigned char*)data;
	if (size && !data)
		return 0;
	while (size) {
		/* getentropy accepts at most 256 bytes per call. */
		size_t count = size < 256 ? size : 256;
#ifdef _WIN32
		if (BCryptGenRandom(NULL, bytes, (ULONG)count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
			return 0;
#elif defined(__linux__)
		ssize_t received = getrandom(bytes, count, GRND_NONBLOCK);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return 0;
		count = (size_t)received;
#else
		if (getentropy(bytes, count) != 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
#endif
		bytes += count;
		size -= count;
	}
	return 1;
}
