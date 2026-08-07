/*
 * font_atlas — shared rasterizer for the .png + .fnt v2 font-atlas
 * format. The builder produces renderer-independent glyph metrics;
 * consumers that add tracking externally must adapt the advances at
 * their integration boundary.
 *
 * font_atlas_load.[ch] populates the same FontAtlasResult from an
 * existing .png + .fnt pair for comparison and editing tools.
 */
#ifndef FONT_ATLAS_H
#define FONT_ATLAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rasterization tunables. Pass 0/-1 sentinels where documented to
 * request an auto-derived value. */
typedef struct {
    int first_char;     /* default 32 (' ') */
    int num_chars;      /* default 96 — covers printable ASCII + DEL */
    int cell_h;         /* required atlas-px cell height */
    int cap_height;     /* target 'H' ink height in atlas px; 0 = auto
                         * (use stbtt_ScaleForPixelHeight on cell_h) */
    int baseline;       /* baseline offset from cell top in atlas px;
                         * -1 = auto (round(ascent*scale)) */
    int cell_w;         /* uniform cell width; 0 = auto from max advance */
    int font_index;     /* TTC face index, default 0 */
    int tracking_atlas;       /* Per-glyph horizontal-stride offset in
                               * atlas px (signed). Applied uniformly
                               * to every glyph's baked advance. Negative
                               * tightens letter-spacing, positive loosens.
                               * 0 = no adjustment. The space character's
                               * override (below) is applied AFTER tracking,
                               * so an explicit space stride wins. */
    int space_advance_atlas;  /* 0 = use the TTF's natural ' ' advance.
                               * >0 = exact stored advance for the space
                               * glyph in atlas px. Applied after generic
                               * tracking, so the explicit value wins. */
    /* Per-class horizontal compression, expressed as a percentage of
     * the uniform (Y-driven) scale. 100 = no compression, 90 = glyphs
     * 10% narrower, 110 = 10% wider. 0 is treated as 100 for backward
     * compatibility. Compression is applied independently to three
     * codepoint classes:
     *   upper  — A..Z
     *   lower  — a..z
     *   other  — digits, punctuation, everything else
     * Cell height, baseline, ascent and cap-height are unaffected:
     * only X is scaled, so a "condensed" atlas keeps vertical metrics
     * matched to a reference atlas. */
    int compression_upper_pct;
    int compression_lower_pct;
    int compression_other_pct;
    /* Per-codepoint additive delta (in percentage points) layered on
     * top of the class compression. Indexed directly by codepoint
     * (0..255). Final per-glyph horizontal scale percentage is
     *   total_pct = class_pct + compression_glyph_delta_pct[cp]
     * clamped to [10, 200] before applying. 0 = no per-glyph override
     * (default). Codepoints >= 256 are not addressable here; the
     * builder treats them as zero. */
    int compression_glyph_delta_pct[256];
    /* Per-codepoint stored-advance override in atlas px. 0 = no
     * override (use the natural-scale
     * advance after class+delta compression and tracking). Same
     * semantics as space_advance_atlas but per-codepoint: applied
     * after the bake loop and bypasses class compression and tracking
     * for that glyph, so spacing can be set independently of glyph
     * shape. */
    int compression_glyph_advance_atlas[256];
    /* Per-codepoint left side bearing override in atlas px (column at
     * which the ink starts inside its cell). <0 = no override (use
     * the natural LSB from the rasterizer's bbox, scaled by class+
     * delta compression). >= 0 = render the glyph's ink at this column
     * inside its cell instead — INCLUDING 0, which is a valid override
     * meaning "ink flush to the cell's left edge".
     *
     * NOTE on initialization: zero-initialised entries (e.g. from
     * `FontAtlasParams p = { ... }` with designated initializers) are
     * interpreted as override-to-0, NOT as "no override". Callers
     * MUST explicitly fill this array with -1 if they want natural
     * LSBs.
     *
     * Why it matters: when only advance is overridden (above), the ink
     * still sits at the rasterizer's natural LSB, which for TTF tends
     * to be much wider than what bitmap references use. Two glyphs
     * with the same advance but different LSBs land their ink at
     * different visual offsets, producing uneven spacing. */
    int compression_glyph_lsb_atlas[256];
    /* Per-codepoint stem-thickness adjustment, in tenths of an
     * atlas-px iteration of a 3x3 morphological pass.
     *   0    = natural rasterization
     *  +N    = dilate stems (bolder); 10 = one full iteration,
     *          intermediate values blend between iters for sub-pixel
     *          control (e.g. 5 = halfway between natural and 1 iter)
     *  -N    = erode stems (thinner) with the same convention
     * Clamped internally to [-100, +100] (= -10.0..+10.0 logical
     * units). Default 0
     * (zero-init via designated initializer is the no-op value, so
     * no sentinel handling like the LSB array). */
    int compression_glyph_boldness_atlas[256];
    /* Per-output-slot codepoint remap for TTF glyph lookup. If
     * codepoint_remap[cp] > 0 for output slot `cp`, the rasterizer
     * fetches the TTF glyph for that remapped codepoint instead of
     * `cp` itself. 0 = no remap (default).
     *
     * This supports fixed-slot encodings where a slot's visual glyph
     * differs from its nominal codepoint. The output slot number is
     * unchanged; class compression and per-glyph deltas remain indexed
     * by the output codepoint. */
    int codepoint_remap[256];
    /* Per-codepoint vertical offset in atlas px. Positive value shifts
     * the glyph UP within its cell, negative shifts it DOWN; 0 = natural
     * placement. Useful when a glyph otherwise falls outside the cell's
     * allotted ascender or descender space. */
    int compression_glyph_y_offset_atlas[256];
} FontAtlasParams;

/* Per-glyph record, byte-identical to the .fnt v2 layout. */
typedef struct {
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t atlas_w;
    uint16_t atlas_h;
    uint16_t advance;
} FontAtlasGlyph;

/* Build result. `rgba` and `glyphs` are owned — release with
 * font_atlas_free(). All atlas-px metrics report the actual chosen
 * values after clamping and auto-derivation. */
typedef struct {
    int      atlas_w;
    int      atlas_h;
    int      cell_w;
    int      cell_h;
    int      first_char;
    int      num_chars;
    int      baseline;

    /* Visual metrics in atlas px — for display + clip warnings. */
    int      ascent_atlas;
    int      descent_atlas;
    int      cap_height_atlas;
    int      max_advance_atlas;

    /* Diagnostics. Non-zero values mean some glyph data was clipped
     * or the font was missing certain codepoints. */
    int      missing_glyphs;
    int      neg_lsb_clip_px;     /* worst negative LSB, atlas px */
    int      oversize_x;          /* worst right-edge overflow (cell_w too small) */
    int      oversize_y;          /* worst bottom-edge overflow */
    int      ascender_clip_px;    /* max(0, ascent_atlas - baseline) */
    int      descender_clip_px;   /* max(0, descent_atlas - (cell_h - baseline)) */

    uint8_t        *rgba;         /* atlas_w * atlas_h * 4, PMA */
    FontAtlasGlyph *glyphs;       /* num_chars entries */
    /* Per-glyph ink width in atlas px (= rightmost ink column -
     * leftmost ink column + 1, or 0 if the glyph has no ink). Useful
     * for matching a candidate's ink independently of its advance.
     * NOT serialized to .fnt — recomputed at
     * load time for bitmap atlases by scanning rgba alpha, or filled
     * in at build time for TTF atlases from the rasterizer's bbox. */
    int            *ink_widths;   /* num_chars entries */
    /* Per-glyph leftmost ink column (atlas-px LSB, relative to the
     * cell origin). 0 if the glyph has no ink. Same provenance as
     * ink_widths — bbox.x0 for TTF builds, alpha-scan for loaded
     * bitmap atlases. NOT serialized. */
    int            *ink_lsbs;     /* num_chars entries */
} FontAtlasResult;

/* Rasterize. Returns false and writes `err` on failure (caller-sized
 * buffer; pass NULL/0 to skip). On success, the caller owns `r->rgba`
 * and `r->glyphs` until font_atlas_free(). */
bool font_atlas_build(const uint8_t *ttf_data, size_t ttf_size,
                      const FontAtlasParams *params,
                      FontAtlasResult *r,
                      char *err, size_t err_size);

/* Write <basename>.png + <basename>.fnt v2. Returns false + err on I/O
 * failure. Does NOT free `r`. */
bool font_atlas_write(const FontAtlasResult *r, const char *basename,
                      char *err, size_t err_size);

/* Release rgba + glyphs, zero the struct. Safe on a zeroed result. */
void font_atlas_free(FontAtlasResult *r);

#ifdef __cplusplus
}
#endif

#endif
