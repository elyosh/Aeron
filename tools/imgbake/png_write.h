/*
 * png_write — minimal RGBA8 PNG writer.
 *
 * Uses zlib's stored-block format (no compression) so we don't need to
 * link against libz. Output is valid PNG, ~0% compressed.
 */
#ifndef FILM_PNG_WRITE_H
#define FILM_PNG_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

bool write_png_rgba(const char *path, int width, int height,
                    const uint8_t *rgba);

/* Atomic variant: writes to "<path>.tmp" and renames over `path` on
 * success so a crash mid-write can't truncate an existing file.
 * Reason on failure (open/write/close/rename) goes into `err` if
 * non-NULL. */
#include <stddef.h>
bool write_png_rgba_atomic(const char *path, int width, int height,
                           const uint8_t *rgba,
                           char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
