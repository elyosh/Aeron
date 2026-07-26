/*
 * Shared TTF→atlas rasterizer. See font_atlas.h for the API contract;
 * font_ttf.c (CLI) and font_tune.cpp (GUI) both call into this.
 *
 * Algorithm:
 *   1. Pick a scale (cap-height-driven if requested, else
 *      stbtt_ScaleForPixelHeight on the cell height)
 *   2. Pre-pass over codepoints: collect bbox + advance, find max
 *      advance / max ink right / max negative LSB — these drive the
 *      uniform cell width
 *   3. Allocate RGBA atlas (16-col grid, 1 px row gutter)
 *   4. Per glyph: rasterize via stbtt_MakeGlyphBitmap, copy alpha into
 *      atlas. RGB = alpha (premultiplied alpha).
 *   5. Fill in metric records + diagnostics.
 *
 * Errors are reported via the err buffer; the function returns false.
 * Diagnostics that aren't fatal (clipping, missing glyphs) land in the
 * result struct so the GUI can display them.
 */

#include "font_atlas.h"
#include "png_write.h"
#include "upscale.h"   /* scale_y_4k */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_COLS 16
#define ROW_GUTTER 1   /* prevents bilinear bleed across cell rows at runtime */

#define FNT_MAGIC   0x544E4654u   /* 'TFNT' little-endian */
#define FNT_VERSION 2

/* 3x3 morphological pass: each output pixel is the max (dilate=true)
 * or min (dilate=false) of its 3x3 neighborhood in src. Pixels outside
 * the (w, h) buffer are treated as 0 — correct for erosion (boundary
 * eats inward) and harmless for dilation (the in-bounds pixels supply
 * the actual stroke). Caller ping-pongs src/dst per iteration. */
static void morpho_pass(const uint8_t *src, uint8_t *dst,
                        int w, int h, bool dilate)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int result = dilate ? 0 : 255;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx;
                    int a;
                    if (xx < 0 || xx >= w || yy < 0 || yy >= h)
                        a = 0;
                    else
                        a = src[(size_t)yy * (size_t)w + (size_t)xx];
                    if (dilate) { if (a > result) result = a; }
                    else        { if (a < result) result = a; }
                }
            }
            dst[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)result;
        }
    }
}

/* Read + clamp the per-glyph boldness param for codepoint cp.
 * Storage unit is 1/10 of a morphological iteration; clamp range
 * (-100..+100) is therefore -10.0..+10.0 logical units. */
static int boldness_for_cp(const FontAtlasParams *p, int cp)
{
    if (cp < 0 || cp >= 256) return 0;
    int b = p->compression_glyph_boldness_atlas[cp];
    if (b < -100) b = -100;
    if (b >  100) b =  100;
    return b;
}

/* Atlas-px slack to reserve on each side of the bbox to fit the
 * dilated bitmap, given an int-tenths boldness. Returns 0 for
 * erosion or natural; ceil(bold/10) for dilation. */
static int dilation_pad_for_bold(int bold_tenths)
{
    if (bold_tenths <= 0) return 0;
    int N   = bold_tenths / 10;
    int rem = bold_tenths - N * 10;
    return N + (rem > 0 ? 1 : 0);
}

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

void font_atlas_free(FontAtlasResult *r)
{
    if (!r) return;
    free(r->rgba);
    free(r->glyphs);
    free(r->ink_widths);
    free(r->ink_lsbs);
    memset(r, 0, sizeof *r);
}

bool font_atlas_build(const uint8_t *ttf_data, size_t ttf_size,
                      const FontAtlasParams *p,
                      FontAtlasResult *r,
                      char *err, size_t err_size)
{
    if (!ttf_data || !p || !r) {
        set_err(err, err_size, "null argument");
        return false;
    }
    (void)ttf_size;
    memset(r, 0, sizeof *r);

    int first_char  = p->first_char  > 0 ? p->first_char  : 32;
    int num_chars   = p->num_chars   > 0 ? p->num_chars   : 96;
    int classic_h   = p->classic_h   > 0 ? p->classic_h   : 16;
    int font_index  = p->font_index  > 0 ? p->font_index  : 0;

    if (num_chars > 0xFFFF) {
        set_err(err, err_size, "num_chars > 65535");
        return false;
    }
    if (first_char + num_chars > 0xFFFF) {
        set_err(err, err_size, "codepoint range out of bounds");
        return false;
    }

    int cell_h = p->cell_h > 0 ? p->cell_h : scale_y_4k(classic_h);
    if (cell_h <= 0 || cell_h > 0xFFFF) {
        set_err(err, err_size, "cell_h out of range: %d", cell_h);
        return false;
    }

    /* Bind font face. */
    int offset = stbtt_GetFontOffsetForIndex(ttf_data, font_index);
    if (offset < 0) {
        set_err(err, err_size, "font index %d not present", font_index);
        return false;
    }
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf_data, offset)) {
        set_err(err, err_size, "stbtt_InitFont failed (not a TTF?)");
        return false;
    }

    int asc_fu, desc_fu, gap_fu;
    stbtt_GetFontVMetrics(&info, &asc_fu, &desc_fu, &gap_fu);

    /* Scale derivation. cap_height takes priority — measure 'H' once
     * to convert atlas-px target into a font-unit scale. */
    float scale;
    int cap_height_fu = 0;
    if (p->cap_height > 0) {
        int hx0, hy0, hx1, hy1;
        if (!stbtt_GetCodepointBox(&info, 'H', &hx0, &hy0, &hx1, &hy1)) {
            set_err(err, err_size, "cap_height: font has no 'H' glyph");
            return false;
        }
        cap_height_fu = hy1 - hy0;
        if (cap_height_fu <= 0) {
            set_err(err, err_size, "cap_height: 'H' bbox invalid");
            return false;
        }
        scale = (float)p->cap_height / (float)cap_height_fu;
    } else {
        scale = stbtt_ScaleForPixelHeight(&info, (float)cell_h);
        /* Need cap_height_fu for the diagnostic report below — measure
         * 'H' if available, otherwise fall back to ascent. */
        int hx0, hy0, hx1, hy1;
        if (stbtt_GetCodepointBox(&info, 'H', &hx0, &hy0, &hx1, &hy1))
            cap_height_fu = hy1 - hy0;
    }

    int baseline = p->baseline >= 0 ? p->baseline
                                    : (int)(asc_fu * scale + 0.5f);
    if (baseline < 0 || baseline > cell_h) {
        set_err(err, err_size, "baseline %d out of [0, %d]", baseline, cell_h);
        return false;
    }

    /* Per-class horizontal compression. 0 → 100% (no compression).
     * Resolve once so the per-glyph loop is just a class lookup. */
    int upper_pct = p->compression_upper_pct > 0 ? p->compression_upper_pct : 100;
    int lower_pct = p->compression_lower_pct > 0 ? p->compression_lower_pct : 100;
    int other_pct = p->compression_other_pct > 0 ? p->compression_other_pct : 100;

    /* Per-glyph pre-pass. Indexed via local struct so we don't expose
     * stb_truetype types in the public header. scale_x captures the
     * per-glyph horizontal scale so the bake loop below doesn't need
     * to redo the codepoint-class dispatch. */
    typedef struct {
        int   gidx;
        int   adv_fu;
        int   lsb_fu;
        int   x0, y0, x1, y1;
        float scale_x;
    } GlyphInfo;

    GlyphInfo *gi = (GlyphInfo *)calloc((size_t)num_chars, sizeof(GlyphInfo));
    if (!gi) {
        set_err(err, err_size, "out of memory (glyph table)");
        return false;
    }

    int max_advance_px  = 0;
    int max_glyph_right = 0;
    int max_glyph_left  = 0;
    int missing = 0;
    for (int g = 0; g < num_chars; g++) {
        int cp = first_char + g;
        /* Per-slot codepoint remap: fetch a different TTF glyph for
         * this output slot, e.g. U+2122 (™) into slot 126. Indexing
         * for class compression and per-glyph param arrays stays on
         * the OUTPUT cp; only the TTF lookup uses the remap. */
        int lookup_cp = cp;
        if (cp >= 0 && cp < 256 && p->codepoint_remap[cp] > 0)
            lookup_cp = p->codepoint_remap[cp];
        gi[g].gidx = stbtt_FindGlyphIndex(&info, lookup_cp);
        if (gi[g].gidx == 0) missing++;

        int pct;
        if      (cp >= 'A' && cp <= 'Z') pct = upper_pct;
        else if (cp >= 'a' && cp <= 'z') pct = lower_pct;
        else                              pct = other_pct;
        /* Per-codepoint delta layered on top of the class. Clamped
         * to [10, 200] so a runaway slider can't produce a 0-wide
         * scale (which would crash the bbox path). */
        if (cp >= 0 && cp < 256)
            pct += p->compression_glyph_delta_pct[cp];
        if (pct < 10)  pct = 10;
        if (pct > 200) pct = 200;
        gi[g].scale_x = scale * (float)pct / 100.0f;

        stbtt_GetGlyphHMetrics(&info, gi[g].gidx, &gi[g].adv_fu, &gi[g].lsb_fu);
        stbtt_GetGlyphBitmapBox(&info, gi[g].gidx, gi[g].scale_x, scale,
                                &gi[g].x0, &gi[g].y0, &gi[g].x1, &gi[g].y1);

        int adv_px = (int)(gi[g].adv_fu * gi[g].scale_x + 0.5f);
        if (adv_px > max_advance_px)        max_advance_px  = adv_px;
        /* Dilation grows the bbox by `pad_g` pixels on every side
         * (ceil of fractional iterations); cell sizing must reserve
         * that slack so the bolded ink doesn't clip. Erosion shrinks
         * the bbox so it never widens cell_w. */
        int pad_g = dilation_pad_for_bold(boldness_for_cp(p, cp));
        int eff_x1 = gi[g].x1 + pad_g;
        int eff_x0 = gi[g].x0 - pad_g;
        if (eff_x1 > max_glyph_right)       max_glyph_right = eff_x1;
        if (eff_x0 < max_glyph_left)        max_glyph_left  = eff_x0;
    }

    int cell_w_auto = max_advance_px;
    if (max_glyph_right > cell_w_auto) cell_w_auto = max_glyph_right;
    cell_w_auto += 1;   /* slack so runtime sampler doesn't leak */
    int cell_w = p->cell_w > 0 ? p->cell_w : cell_w_auto;
    if (cell_w <= 0 || cell_w > 0xFFFF) {
        set_err(err, err_size, "cell_w out of range: %d", cell_w);
        free(gi);
        return false;
    }

    int cols = ATLAS_COLS;
    int rows = (num_chars + cols - 1) / cols;
    int row_stride_y = cell_h + ROW_GUTTER;
    int atlas_w = cols * cell_w;
    int atlas_h = rows * row_stride_y;
    if (atlas_w > 0xFFFF || atlas_h > 0xFFFF) {
        set_err(err, err_size, "atlas exceeds 16-bit dim: %dx%d",
                atlas_w, atlas_h);
        free(gi);
        return false;
    }

    uint8_t *rgba = (uint8_t *)calloc((size_t)atlas_w * (size_t)atlas_h * 4u, 1);
    FontAtlasGlyph *glyphs = (FontAtlasGlyph *)calloc((size_t)num_chars,
                                                      sizeof(FontAtlasGlyph));
    int *ink_widths = (int *)calloc((size_t)num_chars, sizeof(int));
    int *ink_lsbs   = (int *)calloc((size_t)num_chars, sizeof(int));
    if (!rgba || !glyphs || !ink_widths || !ink_lsbs) {
        set_err(err, err_size, "out of memory (atlas/glyphs)");
        free(rgba); free(glyphs); free(ink_widths); free(ink_lsbs);
        free(gi);
        return false;
    }

    /* Two scratch buffers, ping-ponged for the morphological boldness
     * pass. Both sized to cell_w * cell_h — comfortably larger than
     * any single glyph's padded bitmap (gw + 2*pad, gh + 2*pad) since
     * cell_w/h was chosen to fit the widest glyph + dilation slack. */
    size_t scratch_size = (size_t)cell_w * (size_t)cell_h;
    uint8_t *scratch  = (uint8_t *)malloc(scratch_size);
    uint8_t *scratch2 = (uint8_t *)malloc(scratch_size);
    if (!scratch || !scratch2) {
        set_err(err, err_size, "out of memory (scratch)");
        free(scratch); free(scratch2);
        free(rgba); free(glyphs); free(ink_widths); free(ink_lsbs);
        free(gi);
        return false;
    }

    int oversize_x = 0, oversize_y = 0;

    for (int g = 0; g < num_chars; g++) {
        int cp = first_char + g;
        int gw = gi[g].x1 - gi[g].x0;
        int gh = gi[g].y1 - gi[g].y0;
        int bold_tenths = boldness_for_cp(p, cp);
        int pad   = dilation_pad_for_bold(bold_tenths);
        int eff_w = gw + 2 * pad;
        int eff_h = gh + 2 * pad;

        /* Ink width / LSB reflect what the bake will actually produce
         * in the atlas: boldness padding widens the dilated ink and
         * shifts its leftmost column outward; an LSB override pins
         * the leftmost column to the caller-chosen position. Erosion
         * (bold < 0, pad == 0) leaves the bbox alone — strokes shrink
         * inside the natural bounds. Whitespace glyphs report 0/0. */
        ink_widths[g] = gw > 0 ? eff_w : 0;
        if (gw > 0) {
            if (cp >= 0 && cp < 256
                && p->compression_glyph_lsb_atlas[cp] >= 0)
                ink_lsbs[g] = p->compression_glyph_lsb_atlas[cp];
            else
                ink_lsbs[g] = gi[g].x0 - pad;
        } else {
            ink_lsbs[g] = 0;
        }

        /* Always emit a metric record so consumers can index by codepoint.
         *
         * Advance is the natural TTF advance MINUS the runtime's
         * inter-glyph spacer, PLUS the caller's tracking offset (which
         * may be negative for tighter letter-spacing). Result is clamped
         * at zero. The runtime adds the spacer back unconditionally;
         * tracking flows through unchanged, so the on-screen stride
         * shifts by exactly `tracking_atlas` per glyph. */
        int advance_natural = (int)(gi[g].adv_fu * gi[g].scale_x + 0.5f);
        int advance_baked = advance_natural - FONT_ATLAS_SPACE_BETWEEN_PX
                            + p->tracking_atlas;
        if (advance_baked < 0) advance_baked = 0;
        if (advance_baked > 0xFFFF) advance_baked = 0xFFFF;
        glyphs[g].atlas_x = (uint16_t)((g % cols) * cell_w);
        glyphs[g].atlas_y = (uint16_t)((g / cols) * row_stride_y);
        glyphs[g].atlas_w = (uint16_t)cell_w;
        glyphs[g].atlas_h = (uint16_t)cell_h;
        glyphs[g].advance = (uint16_t)advance_baked;

        if (gw <= 0 || gh <= 0)
            continue;   /* whitespace / no ink — record stays as a blank cell */

        int cell_x = (g % cols) * cell_w;
        int cell_y = (g / cols) * row_stride_y;
        /* Natural origin of the (eff_w x eff_h) padded bitmap inside
         * the cell. The natural ink would land `pad` px inward; the
         * dilated ink fills the whole padded box, so the bitmap's top-
         * left sits `pad` px earlier on each axis than the natural
         * bbox would. */
        int dst_x  = cell_x + gi[g].x0 - pad;
        int dst_y  = cell_y + baseline + gi[g].y0 - pad;

        /* Per-glyph LSB override: render the ink at a caller-chosen
         * column inside the cell instead of the rasterizer's natural
         * LSB. >= 0 = override (0 means flush to cell's left edge),
         * < 0 = use natural LSB. The override is interpreted as the
         * leftmost column of the FINAL bitmap (so it already accounts
         * for any dilation) — auto-match copies the reference's
         * measured leftmost-ink column straight in. */
        if (cp >= 0 && cp < 256
            && p->compression_glyph_lsb_atlas[cp] >= 0)
            dst_x = cell_x + p->compression_glyph_lsb_atlas[cp];

        /* Descender-lift: shift codepoints with descenders UP within
         * the cell so the below-baseline part fits a tighter cell
         * budget. Bitmap font8's 'g'/'q' only descend ~10 atlas-px;
         * TTFs naturally do 18-22, which clips when cell_h is the
         * bitmap's. The shift makes lifted glyphs' bowls sit higher
         * than their unlifted neighbours — the trade-off the bitmap
         * font designer made too. */
        if (p->descender_lift_atlas > 0) {
            if (cp == 'g' || cp == 'j' || cp == 'p' || cp == 'q' ||
                cp == 'y' || cp == ',' || cp == ';')
                dst_y -= p->descender_lift_atlas;
        }

        /* Per-glyph vertical offset, applied after descender-lift.
         * Positive shifts up; useful for glyphs that sit far below
         * the cell's allotted descender area (like a font with a
         * very low underscore). Default 0 = no shift. */
        if (cp >= 0 && cp < 256)
            dst_y -= p->compression_glyph_y_offset_atlas[cp];

        /* Clip rect within cell; uses the padded eff_w/eff_h since the
         * dilated bitmap is what we're about to copy. */
        int rx = dst_x < cell_x ? cell_x : dst_x;
        int ry = dst_y < cell_y ? cell_y : dst_y;
        int rw = eff_w - (rx - dst_x);
        int rh = eff_h - (ry - dst_y);
        if (rx + rw > cell_x + cell_w) {
            int over = (rx + rw) - (cell_x + cell_w);
            if (over > oversize_x) oversize_x = over;
            rw -= over;
        }
        if (ry + rh > cell_y + cell_h) {
            int over = (ry + rh) - (cell_y + cell_h);
            if (over > oversize_y) oversize_y = over;
            rh -= over;
        }
        if (rw <= 0 || rh <= 0)
            continue;

        /* Rasterize the natural-size glyph at offset (pad, pad) inside
         * the eff_w x eff_h padded scratch — leaves a `pad`-wide ring
         * of zero alpha around the ink so dilation can grow into it. */
        memset(scratch, 0, (size_t)eff_w * (size_t)eff_h);
        stbtt_MakeGlyphBitmap(&info,
            scratch + (size_t)pad * (size_t)eff_w + (size_t)pad,
            gw, gh, eff_w,
            gi[g].scale_x, scale, gi[g].gidx);

        /* Apply morphological iterations for bold_tenths != 0. Stored
         * unit is 1/10 of an iteration; integer part = full passes,
         * remainder = fractional blend between the floor and ceil
         * iteration counts. Ping-pong scratch/scratch2 so we never
         * read and write the same buffer in one pass. */
        const uint8_t *cur_scratch = scratch;
        if (bold_tenths != 0) {
            bool dilate   = bold_tenths > 0;
            int  abs_b    = bold_tenths > 0 ? bold_tenths : -bold_tenths;
            int  N        = abs_b / 10;
            int  frac_q   = abs_b - N * 10;       /* 0..9 */
            bool has_frac = frac_q > 0;
            uint8_t *src = scratch;
            uint8_t *dst = scratch2;
            for (int i = 0; i < N; i++) {
                morpho_pass(src, dst, eff_w, eff_h, dilate);
                uint8_t *tmp = src; src = dst; dst = tmp;
            }
            /* After the loop, `src` holds the N-iter result (or the
             * natural raster if N == 0). */
            if (has_frac) {
                /* Run one extra iteration into dst, then blend
                 * src (N-iter) and dst (N+1-iter) with weight frac_q/10
                 * back into dst. Integer math + rounding to avoid
                 * float in the inner pixel loop. */
                morpho_pass(src, dst, eff_w, eff_h, dilate);
                size_t total = (size_t)eff_w * (size_t)eff_h;
                int w0 = 10 - frac_q;
                int w1 = frac_q;
                for (size_t i = 0; i < total; i++) {
                    int a = src[i];
                    int b = dst[i];
                    dst[i] = (uint8_t)((a * w0 + b * w1 + 5) / 10);
                }
                cur_scratch = dst;
            } else {
                cur_scratch = src;
            }
        }

        /* Boldness-induced descender clip: dilation extends `pad`
         * rows below the natural bbox. For glyphs that don't
         * naturally descend (y1 <= 0, ink ends at or above the
         * baseline), those extra rows would push ink into the
         * cell's descender area — visible as a thin tail under
         * 'M', 'a', etc. when bolded. Zero them. True descenders
         * (y1 > 0) keep their natural+dilation extension; erosion
         * and pad == 0 paths are no-ops here.
         *
         * Eff-coords baseline row = pad - y0 (since dst_y was set
         * to cell_y + baseline + y0 - pad). For y0 negative (typical),
         * this is positive. We cast cur_scratch to writable because
         * it always points at one of scratch / scratch2 — both of
         * which we own and allocated mutable. */
        if (pad > 0 && gi[g].y1 <= 0) {
            int baseline_eff_y = pad - gi[g].y0;
            if (baseline_eff_y < 0) baseline_eff_y = 0;
            if (baseline_eff_y < eff_h) {
                uint8_t *writable = (uint8_t *)cur_scratch;
                for (int y = baseline_eff_y; y < eff_h; y++)
                    memset(writable + (size_t)y * (size_t)eff_w,
                           0, (size_t)eff_w);
            }
        }

        /* Copy the final padded bitmap (stride eff_w) into the atlas. */
        for (int y = 0; y < rh; y++) {
            int sy = (ry - dst_y) + y;
            const uint8_t *srow = cur_scratch
                + (size_t)sy * (size_t)eff_w + (size_t)(rx - dst_x);
            uint8_t *drow = rgba + ((size_t)(ry + y) * (size_t)atlas_w
                                    + (size_t)rx) * 4u;
            for (int x = 0; x < rw; x++) {
                uint8_t a = srow[x];
                drow[0] = a; drow[1] = a; drow[2] = a; drow[3] = a;
                drow += 4;
            }
        }
    }

    free(scratch);
    free(scratch2);
    free(gi);

    /* Optional: override the space glyph's baked advance to hit a
     * caller-supplied runtime stride. The runtime always adds
     * FONT_ATLAS_SPACE_BETWEEN_PX between glyphs on top of the baked
     * advance, so to land at stride S we bake (S - 9). The bitmap
     * font's space stride is much wider than typical TTF — this is
     * how a TTF replacement matches it. */
    if (p->space_advance_atlas > 0) {
        int space_idx = ' ' - first_char;
        if (space_idx >= 0 && space_idx < num_chars) {
            int adv = p->space_advance_atlas - FONT_ATLAS_SPACE_BETWEEN_PX;
            if (adv < 0) adv = 0;
            if (adv > 0xFFFF) adv = 0xFFFF;
            glyphs[space_idx].advance = (uint16_t)adv;
        }
    }

    /* Per-glyph runtime-stride override. Same convention as
     * space_advance_atlas above (bake = override - 9 to land at the
     * given visible stride after the runtime spacer). Lets tools fix
     * spacing on a glyph-by-glyph basis without touching its
     * compression — used by ink-aware auto-match to trim bearings on
     * thin glyphs while leaving their stem at no-compress. */
    for (int cp = 0; cp < 256; cp++) {
        int target = p->compression_glyph_advance_atlas[cp];
        if (target <= 0) continue;
        int g = cp - first_char;
        if (g < 0 || g >= num_chars) continue;
        int adv = target - FONT_ATLAS_SPACE_BETWEEN_PX;
        if (adv < 0) adv = 0;
        if (adv > 0xFFFF) adv = 0xFFFF;
        glyphs[g].advance = (uint16_t)adv;
    }

    /* Populate result. */
    r->atlas_w           = atlas_w;
    r->atlas_h           = atlas_h;
    r->cell_w            = cell_w;
    r->cell_h            = cell_h;
    r->first_char        = first_char;
    r->num_chars         = num_chars;
    r->baseline          = baseline;
    r->ascent_atlas      = (int)(asc_fu * scale + 0.5f);
    r->descent_atlas     = (int)(-desc_fu * scale + 0.5f);
    r->cap_height_atlas  = cap_height_fu > 0
                           ? (int)(cap_height_fu * scale + 0.5f)
                           : r->ascent_atlas;
    r->max_advance_atlas = max_advance_px;
    r->missing_glyphs    = missing;
    r->neg_lsb_clip_px   = max_glyph_left < 0 ? -max_glyph_left : 0;
    r->oversize_x        = oversize_x;
    r->oversize_y        = oversize_y;
    r->ascender_clip_px  = r->ascent_atlas  > baseline
                           ? r->ascent_atlas  - baseline : 0;
    r->descender_clip_px = r->descent_atlas > (cell_h - baseline)
                           ? r->descent_atlas - (cell_h - baseline) : 0;
    r->rgba              = rgba;
    r->glyphs            = glyphs;
    r->ink_widths        = ink_widths;
    r->ink_lsbs          = ink_lsbs;
    return true;
}

bool font_atlas_write(const FontAtlasResult *r, const char *basename,
                      char *err, size_t err_size)
{
    if (!r || !basename || !r->rgba || !r->glyphs) {
        set_err(err, err_size, "invalid argument to write");
        return false;
    }
    char path[1024];

    snprintf(path, sizeof path, "%s.png", basename);
    if (!write_png_rgba(path, r->atlas_w, r->atlas_h, r->rgba)) {
        set_err(err, err_size, "PNG write failed: %s", path);
        return false;
    }

    snprintf(path, sizeof path, "%s.fnt", basename);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        set_err(err, err_size, "fopen failed: %s", path);
        return false;
    }

    uint8_t hdr[24];
    memset(hdr, 0, sizeof hdr);
    uint32_t magic = FNT_MAGIC;
    memcpy(hdr + 0, &magic, 4);
    uint16_t v;
    v = FNT_VERSION;            memcpy(hdr +  4, &v, 2);
    v = (uint16_t)r->first_char;memcpy(hdr +  6, &v, 2);
    v = (uint16_t)r->num_chars; memcpy(hdr +  8, &v, 2);
    v = (uint16_t)r->atlas_w;   memcpy(hdr + 10, &v, 2);
    v = (uint16_t)r->atlas_h;   memcpy(hdr + 12, &v, 2);
    v = (uint16_t)r->cell_w;    memcpy(hdr + 14, &v, 2);
    v = (uint16_t)r->cell_h;    memcpy(hdr + 16, &v, 2);
    v = (uint16_t)r->baseline;  memcpy(hdr + 18, &v, 2);
    if (fwrite(hdr, 1, 24, fp) != 24) {
        set_err(err, err_size, "header write failed");
        fclose(fp); return false;
    }

    for (int g = 0; g < r->num_chars; g++) {
        uint8_t rec[10];
        memcpy(rec + 0, &r->glyphs[g].atlas_x, 2);
        memcpy(rec + 2, &r->glyphs[g].atlas_y, 2);
        memcpy(rec + 4, &r->glyphs[g].atlas_w, 2);
        memcpy(rec + 6, &r->glyphs[g].atlas_h, 2);
        memcpy(rec + 8, &r->glyphs[g].advance, 2);
        if (fwrite(rec, 1, 10, fp) != 10) {
            set_err(err, err_size, "record %d write failed", g);
            fclose(fp); return false;
        }
    }
    fclose(fp);
    return true;
}
