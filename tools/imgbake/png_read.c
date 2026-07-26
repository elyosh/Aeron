#include "png_read.h"

/* STB_IMAGE_STATIC keeps the implementation symbols TU-local so they
 * don't collide with the copy already living in tie_cutscene
 * (cutscene_subtitle_gpu.c) when filmview links both static libs. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <stdio.h>

bool read_png_rgba(const char *path, uint8_t **out_rgba,
                   int *out_w, int *out_h,
                   char *err, size_t errsz) {
    if (!path || !out_rgba || !out_w || !out_h) {
        if (err && errsz) snprintf(err, errsz, "invalid args");
        return false;
    }
    int n = 0;
    uint8_t *p = stbi_load(path, out_w, out_h, &n, /*req_comp=*/4);
    if (!p) {
        const char *r = stbi_failure_reason();
        if (err && errsz) snprintf(err, errsz, "%s: %s", path,
                                   r ? r : "(no detail)");
        return false;
    }
    *out_rgba = p;
    return true;
}
