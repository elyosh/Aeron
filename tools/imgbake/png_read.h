/*
 * png_read — minimal RGBA8 PNG loader.
 *
 * Wraps stb_image with a fixed force_channels=4 and a NUL-terminated
 * reason string on failure.
 *
 * On success `*out_rgba` is heap-allocated by stb (caller frees with
 * free()), `*out_w` / `*out_h` hold pixel dimensions, and the row
 * stride is `*out_w * 4` with no padding.
 */
#ifndef IMGBAKE_PNG_READ_H
#define IMGBAKE_PNG_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

bool read_png_rgba(const char *path, uint8_t **out_rgba,
                   int *out_w, int *out_h,
                   char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
