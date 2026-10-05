/*
 * hdr_read — Radiance .hdr loader.
 *
 * Wraps stb_image's float loader with a fixed three-channel output and a
 * NUL-terminated reason string on failure.
 *
 * On success `*out_rgb` is heap-allocated (caller frees with free()),
 * `*out_w` / `*out_h` hold pixel dimensions, and the row stride is
 * `*out_w * 3` floats, top row first. Values are linear as stored in the
 * file.
 */
#ifndef IMGBAKE_HDR_READ_H
#define IMGBAKE_HDR_READ_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

bool read_hdr_rgb(const char* path, float** out_rgb, int* out_w, int* out_h, char* err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
