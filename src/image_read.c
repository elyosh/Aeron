/*
 * Reads images from the VFS. stb_image is compiled as static functions here,
 * like stb_image_write in image_write.c, so it doesn't clash with other
 * libraries and host tools that also include its implementation.
 */

#include "aeron/image.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
/* The compiler reports unused static functions at the end of the file, so
 * the warning has to stay off until then. */
#if defined(_MSC_VER)
#pragma warning(disable : 4505) /* unreferenced function with internal linkage */
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"

#include <limits.h>
#include <stdlib.h>

uint8_t* Aeron_ImageReadPngRgba8(AeronVfs* vfs, AeronVfsRoot root, const char* path, int* out_width,
								 int* out_height) {
	if (!vfs || !path || !out_width || !out_height)
		return NULL;
	uint8_t* bytes = NULL;
	size_t   size  = 0;
	if (!AeronVfs_ReadAll(vfs, root, path, INT_MAX, &bytes, &size))
		return NULL;
	int      width = 0, height = 0, channels = 0;
	uint8_t* pixels = stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4);
	free(bytes);
	if (!pixels)
		return NULL;
	*out_width  = width;
	*out_height = height;
	return pixels;
}
