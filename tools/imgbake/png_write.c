/*
 * png_write — RGBA8 PNG writer backed by stb_image_write.
 *
 * stb_image_write keeps the tool dependency-free and provides its own
 * deflate implementation. Files are typically compact for pixel-art
 * content.
 */

#include "png_write.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool write_png_rgba(const char *path, int width, int height,
                    const uint8_t *rgba)
{
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

bool write_png_rgba_atomic(const char *path, int width, int height,
                           const uint8_t *rgba,
                           char *err, size_t errsz)
{
	if (!path || !rgba || width <= 0 || height <= 0) {
		if (err && errsz) snprintf(err, errsz, "invalid args");
		return false;
	}
	char tmp[1100];
	int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (n < 0 || n >= (int)sizeof tmp) {
		if (err && errsz) snprintf(err, errsz, "path too long");
		return false;
	}
	if (!write_png_rgba(tmp, width, height, rgba)) {
		if (err && errsz) snprintf(err, errsz,
		                           "write %s failed (errno=%d: %s)",
		                           tmp, errno, strerror(errno));
		SDL_RemovePath(tmp);
		return false;
	}
	if (!SDL_RenamePath(tmp, path)) {
		if (err && errsz) snprintf(err, errsz,
		                           "rename %s -> %s: %s",
		                           tmp, path, SDL_GetError());
		SDL_RemovePath(tmp);
		return false;
	}
	return true;
}
