/*
 * png_write — the atomic variant of the PNG writer: a temporary file
 * renamed over the target. Kept apart from png_write.c, which needs no SDL.
 */

#include "png_write.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool write_png_rgba_atomic(const char* path, int width, int height, const uint8_t* rgba, char* err,
						   size_t errsz) {
	if (!path || !rgba || width <= 0 || height <= 0) {
		if (err && errsz)
			snprintf(err, errsz, "invalid args");
		return false;
	}
	char tmp[1100];
	int  n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (n < 0 || n >= (int)sizeof tmp) {
		if (err && errsz)
			snprintf(err, errsz, "path too long");
		return false;
	}
	if (!write_png_rgba(tmp, width, height, rgba)) {
		if (err && errsz)
			snprintf(err, errsz, "write %s failed (errno=%d: %s)", tmp, errno, strerror(errno));
		SDL_RemovePath(tmp);
		return false;
	}
	if (!SDL_RenamePath(tmp, path)) {
		if (err && errsz)
			snprintf(err, errsz, "rename %s -> %s: %s", tmp, path, SDL_GetError());
		SDL_RemovePath(tmp);
		return false;
	}
	return true;
}
