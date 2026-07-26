/*
 * .png + .fnt v2 loader. Mirrors the parsing in
 * shells/sdl3/cutscene_subtitle_gpu.c::load_font_atlas so the GUI's
 * loaded view of the bitmap reference is identical to what the
 * runtime will display.
 *
 * PNG decoding goes through filmlib's read_png_rgba (which already
 * vendors stb_image with STB_IMAGE_STATIC) — that keeps stb_image's
 * implementation strictly TU-local in png_read.c and avoids symbol
 * clashes when this file links into a tool that also pulls filmlib.
 */

#include "font_atlas_load.h"
#include "font_atlas.h"
#include "png_read.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNT_MAGIC 0x544E4654u   /* 'TFNT' */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool font_atlas_load(const char *basename, FontAtlasResult *r,
                     char *err, size_t err_size)
{
    if (!basename || !r) {
        set_err(err, err_size, "null arg");
        return false;
    }
    memset(r, 0, sizeof *r);

    char path[1024];
    size_t fnt_size = 0;
    snprintf(path, sizeof path, "%s.fnt", basename);
    uint8_t *fnt = read_file(path, &fnt_size);
    if (!fnt) {
        set_err(err, err_size, "open failed: %s", path);
        return false;
    }
    if (fnt_size < 24 || rd_u32le(fnt) != FNT_MAGIC) {
        set_err(err, err_size, "%s: bad magic / truncated", path);
        free(fnt); return false;
    }
    uint16_t version = rd_u16le(fnt + 4);
    if (version != 2) {
        set_err(err, err_size, "%s: unsupported version %u", path, version);
        free(fnt); return false;
    }

    int first    = rd_u16le(fnt +  6);
    int count    = rd_u16le(fnt +  8);
    int atlas_w  = rd_u16le(fnt + 10);
    int atlas_h  = rd_u16le(fnt + 12);
    int cell_w   = rd_u16le(fnt + 14);
    int cell_h   = rd_u16le(fnt + 16);
    int baseline = rd_u16le(fnt + 18);

    if (fnt_size < (size_t)24 + (size_t)count * 10) {
        set_err(err, err_size, "%s: records truncated", path);
        free(fnt); return false;
    }
    FontAtlasGlyph *glyphs = (FontAtlasGlyph *)calloc((size_t)count,
                                                      sizeof(FontAtlasGlyph));
    if (!glyphs) {
        set_err(err, err_size, "out of memory");
        free(fnt); return false;
    }
    const uint8_t *recs = fnt + 24;
    int max_advance = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *rec = recs + (size_t)i * 10;
        glyphs[i].atlas_x = rd_u16le(rec + 0);
        glyphs[i].atlas_y = rd_u16le(rec + 2);
        glyphs[i].atlas_w = rd_u16le(rec + 4);
        glyphs[i].atlas_h = rd_u16le(rec + 6);
        glyphs[i].advance = rd_u16le(rec + 8);
        if (glyphs[i].advance > max_advance) max_advance = glyphs[i].advance;
    }
    free(fnt);

    snprintf(path, sizeof path, "%s.png", basename);
    int png_w = 0, png_h = 0;
    uint8_t *rgba = NULL;
    char png_err[256];
    if (!read_png_rgba(path, &rgba, &png_w, &png_h, png_err, sizeof png_err)) {
        set_err(err, err_size, "%s", png_err);
        free(glyphs); return false;
    }
    if (png_w != atlas_w || png_h != atlas_h) {
        set_err(err, err_size, "%s: PNG %dx%d != .fnt %dx%d",
                path, png_w, png_h, atlas_w, atlas_h);
        free(rgba); free(glyphs); return false;
    }
    /* read_png_rgba returns the stb_image buffer directly, which uses
     * the default malloc allocator (no custom STBI_MALLOC override in
     * the project), so plain free() through font_atlas_free is safe. */

    /* Per-glyph ink width and LSB from rgba scan. The atlas is white-
     * on-clear PMA (R=G=B=A), so alpha alone is ink. Bitmap atlases
     * have hard 0/255 alpha; TTF-rendered atlases re-loaded here have
     * AA fringes, so threshold at 32 to ignore the very faint tails.
     * Used by tools that match ink rather than advance, and that align
     * candidate ink LSB to reference LSB. */
    int *ink_widths = (int *)calloc((size_t)count, sizeof(int));
    int *ink_lsbs   = (int *)calloc((size_t)count, sizeof(int));
    if (!ink_widths || !ink_lsbs) {
        set_err(err, err_size, "out of memory (ink_widths/ink_lsbs)");
        free(rgba); free(glyphs); free(ink_widths); free(ink_lsbs);
        return false;
    }
    for (int g = 0; g < count; g++) {
        int gx = glyphs[g].atlas_x;
        int gy = glyphs[g].atlas_y;
        int gw = glyphs[g].atlas_w;
        int gh = glyphs[g].atlas_h;
        if (gw <= 0 || gh <= 0 ||
            gx + gw > atlas_w || gy + gh > atlas_h) continue;
        int min_x = gw, max_x = -1;
        for (int x = 0; x < gw; x++) {
            for (int y = 0; y < gh; y++) {
                uint8_t a = rgba[((size_t)(gy + y) * (size_t)atlas_w
                                  + (size_t)(gx + x)) * 4u + 3u];
                if (a >= 32) {
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    break;
                }
            }
        }
        if (max_x >= min_x) {
            ink_widths[g] = max_x - min_x + 1;
            ink_lsbs[g]   = min_x;
        }
    }

    r->atlas_w           = atlas_w;
    r->atlas_h           = atlas_h;
    r->cell_w            = cell_w;
    r->cell_h            = cell_h;
    r->first_char        = first;
    r->num_chars         = count;
    r->baseline          = baseline;
    r->ascent_atlas      = baseline;       /* rough — bitmap atlases pack tightly */
    r->descent_atlas     = cell_h - baseline;
    r->cap_height_atlas  = baseline;       /* same — bitmap caps fill above baseline */
    r->max_advance_atlas = max_advance;
    r->rgba              = rgba;
    r->glyphs            = glyphs;
    r->ink_widths        = ink_widths;
    r->ink_lsbs          = ink_lsbs;
    return true;
}
