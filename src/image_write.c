/*
 * Image writers on the VFS. stb_image_write is compiled static here so the
 * host tools that define its implementation do not see duplicate symbols.
 */

#include "aeron/image.h"

/* stb_image_write 1.16 defines its HDR encoder inside the stdio section, so
 * STBI_WRITE_NO_STDIO cannot be used; the file writers stay unused. */
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
/* C4505 is reported at the end of the file, so it stays disabled here. */
#pragma warning(disable : 4505) /* unreferenced function with internal linkage */
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <stdlib.h>
#include <string.h>

typedef struct HdrBuffer {
	uint8_t* data;
	size_t   size;
	size_t   capacity;
	bool     ok;
} HdrBuffer;

static void hdr_append(void* context, void* data, int size) {
	HdrBuffer* b = (HdrBuffer*)context;
	if (!b->ok || size <= 0)
		return;
	if (b->size + (size_t)size > b->capacity) {
		size_t capacity = b->capacity ? b->capacity : 4096u;
		while (capacity < b->size + (size_t)size)
			capacity *= 2u;
		uint8_t* grown = (uint8_t*)realloc(b->data, capacity);
		if (!grown) {
			b->ok = false;
			return;
		}
		b->data     = grown;
		b->capacity = capacity;
	}
	memcpy(b->data + b->size, data, (size_t)size);
	b->size += (size_t)size;
}

bool Aeron_ImageWriteHdr(AeronVfs* vfs, AeronVfsRoot root, const char* path, int width, int height,
						 const float* rgb) {
	if (!vfs || !path || !rgb || width <= 0 || height <= 0)
		return false;
	/* RGBE rows hold about 4 bytes per pixel; reserve that up front. */
	HdrBuffer buffer = { NULL, 0, 0, true };
	buffer.capacity  = (size_t)width * (size_t)height * 4u + 256u;
	buffer.data      = (uint8_t*)malloc(buffer.capacity);
	if (!buffer.data)
		return false;
	const bool ok = stbi_write_hdr_to_func(hdr_append, &buffer, width, height, 3, rgb) && buffer.ok &&
					AeronVfs_WriteAllAtomic(vfs, root, path, buffer.data, buffer.size);
	free(buffer.data);
	return ok;
}
