/*
 * png_write — RGBA8 PNG writer backed by stb_image_write.
 *
 * stb_image_write keeps the tool dependency-free and provides its own
 * deflate implementation. Files are typically compact for pixel-art
 * content. The atomic variant is png_write_atomic.c's, which needs SDL.
 */

#include "png_write.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool write_png_rgba(const char* path, int width, int height, const uint8_t* rgba) {
	if (!path || !rgba || width <= 0 || height <= 0)
		return false;
	/* stb's deflate "quality" caps the hash bucket depth used during
	 * LZ77 match search (it's not zlib's gzip-style 1–9 effort knob).
	 * The library clamps anything below 5 up to 5, so 5 is the floor
	 * that still trims work vs the default 8. On 4K-upscaled atlases
	 * level 5 produces files within ~0.2 % of level-8 size while
	 * cutting PNG encode time by ~20 %. */
	stbi_write_png_compression_level = 5;
	/* RGBA = 4 components, stride = width*4 (no row padding). */
	int rc = stbi_write_png(path, width, height, 4, rgba, width * 4);
	return rc != 0;
}
