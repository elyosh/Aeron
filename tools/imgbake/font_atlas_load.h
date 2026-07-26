/*
 * font_atlas_load — read an existing .png + .fnt v2 pair into a
 * FontAtlasResult so the GUI can render sample text from the bitmap
 * atlas using the same code path it uses for live TTF rasterization.
 *
 * The loaded result populates the same fields as font_atlas_build
 * except diagnostic counters (missing_glyphs, oversize_*, ascender_clip_px,
 * etc.) which stay zero — those describe build-time issues, not the
 * loaded data.
 */
#ifndef FONT_ATLAS_LOAD_H
#define FONT_ATLAS_LOAD_H

#include "font_atlas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read <basename>.png + <basename>.fnt v2. Returns false + err on
 * failure. On success the result owns the decoded PNG buffer + glyph
 * records; release with font_atlas_free(). */
bool font_atlas_load(const char *basename, FontAtlasResult *r,
                     char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
