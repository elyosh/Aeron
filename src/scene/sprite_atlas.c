/*
 * Sprite-atlas YAML parser (consumer side of `filmextract --atlas`).
 *
 * Uses the vendored libyaml's document API (DOM-style) — for the few
 * KB of layout files we ship, allocating a tree is fine and the
 * traversal code stays compact compared to event-driven parsing.
 */

#include "aeron/scene/sprite_atlas.h"

#include "aeron/log.h"
#include "aeron/numeric.h"
#include "aeron/config_file.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <yaml.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_int(const AeronConfigNode *map, const char *key, int fallback) {
    const AeronConfigNode *value = AeronConfigNode_MapGet(map, key);
    if (AeronConfigNode_Type(value) != AERON_CONFIG_INT) return fallback;
    int64_t parsed = AeronConfigNode_Int(value, fallback);
    return parsed < INT_MIN || parsed > INT_MAX ? fallback : (int)parsed;
}

bool Aeron_SpriteAtlasLoadVfs(AeronSpriteAtlas *out, AeronVfs *vfs,
                              AeronVfsRoot root, const char *yaml_path) {
    if (!out || !vfs || !yaml_path || !yaml_path[0]) return false;
    memset(out, 0, sizeof *out);
    AeronConfigFile *document = NULL;
    AeronConfigError error = {0};
    if (!AeronConfigFile_LoadYamlEx(vfs, root, yaml_path, &document, &error)) {
        Aeron_LogError("aeron.scene", "[atlas] %s:%d: %s", yaml_path,
                       error.line, error.message);
        return false;
    }
    const AeronConfigNode *document_root = AeronConfigFile_Root(document);
    const AeronConfigNode *atlas = AeronConfigNode_MapGet(document_root, "atlas");
    const AeronConfigNode *frames = AeronConfigNode_MapGet(document_root, "frames");
    const size_t frame_count = AeronConfigNode_SequenceCount(frames);
    out->atlas_w = config_int(atlas, "w", 0);
    out->atlas_h = config_int(atlas, "h", 0);
    out->classic_atlas_w = config_int(atlas, "classic_w", 0);
    out->classic_atlas_h = config_int(atlas, "classic_h", 0);
    if (AeronConfigNode_Type(document_root) != AERON_CONFIG_MAP ||
        AeronConfigNode_Type(atlas) != AERON_CONFIG_MAP ||
        AeronConfigNode_Type(frames) != AERON_CONFIG_SEQUENCE ||
        frame_count == 0 || frame_count > INT_MAX ||
        out->atlas_w <= 0 || out->atlas_h <= 0) {
        Aeron_LogError("aeron.scene", "[atlas] %s: invalid atlas document",
                       yaml_path);
        AeronConfigFile_Destroy(document);
        return false;
    }
    out->frames = calloc(frame_count, sizeof *out->frames);
    out->origin_x = calloc(frame_count, sizeof *out->origin_x);
    out->origin_y = calloc(frame_count, sizeof *out->origin_y);
    out->ids = calloc(frame_count, sizeof *out->ids);
    out->pages = calloc(frame_count, sizeof *out->pages);
    out->classic_w = calloc(frame_count, sizeof *out->classic_w);
    out->classic_h = calloc(frame_count, sizeof *out->classic_h);
    if (!out->frames || !out->origin_x || !out->origin_y || !out->ids ||
        !out->pages || !out->classic_w || !out->classic_h) {
        AeronConfigFile_Destroy(document);
        Aeron_SpriteAtlasFree(out);
        return false;
    }
    bool have_ids = false, have_pages = false, have_classic = false;
    int max_page = 0;
    for (size_t index = 0; index < frame_count; ++index) {
        const AeronConfigNode *frame = AeronConfigNode_SequenceGet(frames, index);
        int x = config_int(frame, "x", -1), y = config_int(frame, "y", -1);
        int w = config_int(frame, "w", -1), h = config_int(frame, "h", -1);
        int page = config_int(frame, "page", 0);
        if (AeronConfigNode_Type(frame) != AERON_CONFIG_MAP || x < 0 || y < 0 ||
            w <= 0 || h <= 0 || page < 0 || page > INT16_MAX ||
            x > out->atlas_w - w || y > out->atlas_h - h) {
            Aeron_LogError("aeron.scene", "[atlas] %s: invalid frame %zu",
                           yaml_path, index);
            AeronConfigFile_Destroy(document);
            Aeron_SpriteAtlasFree(out);
            return false;
        }
        out->frames[index] = (AeronSpriteRect){x, y, w, h};
        out->origin_x[index] = (int16_t)config_int(frame, "origin_x", 0);
        out->origin_y[index] = (int16_t)config_int(frame, "origin_y", 0);
        out->ids[index] = config_int(frame, "id", -1);
        out->pages[index] = (int16_t)page;
        out->classic_w[index] = (int16_t)config_int(frame, "classic_w", 0);
        out->classic_h[index] = (int16_t)config_int(frame, "classic_h", 0);
        have_ids |= out->ids[index] >= 0;
        have_pages |= page > 0;
        have_classic |= out->classic_w[index] > 0;
        if (page > max_page) max_page = page;
    }
    out->frame_count = (int)frame_count;
    out->page_count = max_page + 1;
    if (!have_ids) { free(out->ids); out->ids = NULL; }
    if (!have_pages) { free(out->pages); out->pages = NULL; }
    if (!have_classic) {
        free(out->classic_w); out->classic_w = NULL;
        free(out->classic_h); out->classic_h = NULL;
    }
    AeronConfigFile_Destroy(document);
    return true;
}

/* Pull a scalar node's string content; NULL when not a scalar. */
static const char *node_scalar(const yaml_document_t *doc, int node_id) {
    yaml_node_t *n = yaml_document_get_node((yaml_document_t *)doc, node_id);
    if (!n || n->type != YAML_SCALAR_NODE)
        return NULL;
    return (const char *)n->data.scalar.value;
}

static int node_int(const yaml_document_t *doc, int node_id) {
    const char *s = node_scalar(doc, node_id);
    return s ? atoi(s) : 0;
}

static float node_float(const yaml_document_t *doc, int node_id) {
    const char *s = node_scalar(doc, node_id);
    double value;
    return s && Aeron_ParseAsciiDouble(s, strlen(s), &value) ? (float)value : 0.0f;
}

/* Walk an `atlas:` mapping, populating out->atlas_w / out->atlas_h.
 * Other writer-side fields (cols/rows/max_w/max_h/pad) are ignored —
 * the consumer only needs the rect array, which the writer derives
 * once and bakes into `frames`. */
static void parse_atlas_mapping(const yaml_document_t *doc,
                                const yaml_node_t *m,
                                AeronSpriteAtlas *out) {
    for (yaml_node_pair_t *kv = m->data.mapping.pairs.start;
         kv < m->data.mapping.pairs.top; kv++) {
        const char *k = node_scalar(doc, kv->key);
        if (!k)
            continue;
        if      (strcmp(k, "w") == 0) out->atlas_w = node_int(doc, kv->value);
        else if (strcmp(k, "h") == 0) out->atlas_h = node_int(doc, kv->value);
        else if (strcmp(k, "classic_w") == 0) out->classic_atlas_w = node_int(doc, kv->value);
        else if (strcmp(k, "classic_h") == 0) out->classic_atlas_h = node_int(doc, kv->value);
    }
}

/* Walk a single frame mapping (`{ x: .., y: .., w: .., h: .. }` or
 * the block-style equivalent) into one AeronSpriteRect slot plus a pair
 * of int16 origin slots. The origin slots are kept purely so save can
 * round-trip them; the compose path doesn't consult them. */
static void parse_frame_mapping(const yaml_document_t *doc,
                                const yaml_node_t *m,
                                AeronSpriteRect *out,
                                int16_t *out_origin_x,
                                int16_t *out_origin_y,
                                int32_t *out_id,
                                int16_t *out_page,
                                int16_t *out_classic_w,
                                int16_t *out_classic_h) {
    for (yaml_node_pair_t *kv = m->data.mapping.pairs.start;
         kv < m->data.mapping.pairs.top; kv++) {
        const char *k = node_scalar(doc, kv->key);
        if (!k)
            continue;
        float v = node_float(doc, kv->value);
        if      (strcmp(k, "x") == 0) out->x = v;
        else if (strcmp(k, "y") == 0) out->y = v;
        else if (strcmp(k, "w") == 0) out->w = v;
        else if (strcmp(k, "h") == 0) out->h = v;
        else if (strcmp(k, "origin_x") == 0)
            *out_origin_x = (int16_t)node_int(doc, kv->value);
        else if (strcmp(k, "origin_y") == 0)
            *out_origin_y = (int16_t)node_int(doc, kv->value);
        else if (strcmp(k, "id") == 0)
            *out_id = (int32_t)node_int(doc, kv->value);
        else if (strcmp(k, "page") == 0)
            *out_page = (int16_t)node_int(doc, kv->value);
        else if (strcmp(k, "classic_w") == 0)
            *out_classic_w = (int16_t)node_int(doc, kv->value);
        else if (strcmp(k, "classic_h") == 0)
            *out_classic_h = (int16_t)node_int(doc, kv->value);
    }
}

bool Aeron_SpriteAtlasLoad(AeronSpriteAtlas *out, const char *yaml_path) {
    if (!out || !yaml_path)
        return false;
    memset(out, 0, sizeof *out);

    FILE *fp = fopen(yaml_path, "rb");
    if (!fp) {
        Aeron_LogError("aeron.scene", "[atlas] cannot open %s", yaml_path);
        return false;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return false;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        Aeron_LogError("aeron.scene", "[atlas] %s: parse error at line %lu col %lu: %s",
                yaml_path,
                (unsigned long)parser.problem_mark.line + 1,
                (unsigned long)parser.problem_mark.column + 1,
                parser.problem ? parser.problem : "(no detail)");
        yaml_parser_delete(&parser);
        fclose(fp);
        return false;
    }

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        Aeron_LogError("aeron.scene", "[atlas] %s: top-level node is not a mapping", yaml_path);
        goto fail;
    }

    /* First pass over the root mapping pulls the `atlas:` block (for
     * dims) and the `frames:` sequence. Order in the file is
     * unimportant — libyaml resolves cross-refs through the document. */
    for (yaml_node_pair_t *kv = root->data.mapping.pairs.start;
         kv < root->data.mapping.pairs.top; kv++) {
        const char *k = node_scalar(&doc, kv->key);
        yaml_node_t *v = yaml_document_get_node(&doc, kv->value);
        if (!k || !v)
            continue;
        if (strcmp(k, "atlas") == 0 && v->type == YAML_MAPPING_NODE) {
            parse_atlas_mapping(&doc, v, out);
        } else if (strcmp(k, "frames") == 0 &&
                   v->type == YAML_SEQUENCE_NODE) {
            int n = (int)(v->data.sequence.items.top -
                          v->data.sequence.items.start);
            if (n <= 0)
                continue;
            out->frames    = (AeronSpriteRect *)calloc((size_t)n,
                                                     sizeof(AeronSpriteRect));
            out->origin_x  = (int16_t *)calloc((size_t)n, sizeof(int16_t));
            out->origin_y  = (int16_t *)calloc((size_t)n, sizeof(int16_t));
            out->ids       = (int32_t *)calloc((size_t)n, sizeof(int32_t));
            out->pages     = (int16_t *)calloc((size_t)n, sizeof(int16_t));
            out->classic_w = (int16_t *)calloc((size_t)n, sizeof(int16_t));
            out->classic_h = (int16_t *)calloc((size_t)n, sizeof(int16_t));
            if (!out->frames || !out->origin_x || !out->origin_y ||
                !out->ids || !out->pages || !out->classic_w || !out->classic_h) {
                Aeron_LogError("aeron.scene", "[atlas] %s: out of memory (%d frames)",
                        yaml_path, n);
                goto fail;
            }
            int idx = 0;
            int have_ids = 0;
            int max_page = 0;
            int have_classic = 0;
            for (yaml_node_item_t *it = v->data.sequence.items.start;
                 it < v->data.sequence.items.top && idx < n; it++, idx++) {
                yaml_node_t *fr = yaml_document_get_node(&doc, *it);
                out->ids[idx] = -1;
                if (fr && fr->type == YAML_MAPPING_NODE)
                    parse_frame_mapping(&doc, fr, &out->frames[idx],
                                        &out->origin_x[idx],
                                        &out->origin_y[idx],
                                        &out->ids[idx],
                                        &out->pages[idx],
                                        &out->classic_w[idx],
                                        &out->classic_h[idx]);
                if (out->ids[idx] >= 0)
                    have_ids = 1;
                if (out->pages[idx] > max_page)
                    max_page = out->pages[idx];
                if (out->classic_w[idx] > 0)
                    have_classic = 1;
            }
            /* Key-less corpora keep NULL classic arrays (Save
             * round-trip parity; consumers fall back to the
             * atlas-level ratio). */
            if (!have_classic) {
                free(out->classic_w);
                free(out->classic_h);
                out->classic_w = NULL;
                out->classic_h = NULL;
            }
            /* Id-less / single-page atlases keep NULL arrays so Save
             * round-trips the file without introducing keys. */
            if (!have_ids) {
                free(out->ids);
                out->ids = NULL;
            }
            if (max_page == 0) {
                free(out->pages);
                out->pages = NULL;
            }
            out->page_count  = max_page + 1;
            out->frame_count = n;
        }
    }

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);

    if (out->frame_count == 0) {
        Aeron_LogError("aeron.scene", "[atlas] %s: no `frames:` sequence found",
                yaml_path);
        Aeron_SpriteAtlasFree(out);
        return false;
    }
    return true;

fail:
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    Aeron_SpriteAtlasFree(out);
    return false;
}

void Aeron_SpriteAtlasFree(AeronSpriteAtlas *a) {
    if (!a)
        return;
    free(a->frames);
    free(a->origin_x);
    free(a->origin_y);
    free(a->ids);
    free(a->pages);
    free(a->classic_w);
    free(a->classic_h);
    memset(a, 0, sizeof *a);
}

int Aeron_SpriteAtlasFindById(const AeronSpriteAtlas *a, int32_t id) {
    if (!a || !a->ids || id < 0)
        return -1;
    for (int i = 0; i < a->frame_count; i++) {
        if (a->ids[i] == id)
            return i;
    }
    return -1;
}

/* Round-to-nearest float→int with negatives clamped to 0. The runtime
 * stores rect coords as float for sub-pixel-precise dst rects, but the
 * on-disk YAML uses integer pixel coords (filmextract writes them as
 * %d). The editor only mutates frames via sprite_atlas writers that
 * land integer values, so this rounder is a no-op in practice; it
 * exists to keep the save path defensive. */
static int round_pos(float f) {
    if (f <= 0.0f) return 0;
    return (int)(f + 0.5f);
}

bool Aeron_SpriteAtlasSave(const AeronSpriteAtlas *a, const char *yaml_path,
                       char *err, size_t errsz) {
    if (!a || !yaml_path || !yaml_path[0]) {
        if (err && errsz) snprintf(err, errsz, "invalid args");
        return false;
    }

    /* tmp path = "<yaml_path>.tmp". Atomic rename keeps the source-of-
     * truth intact when fprintf fails partway through. */
    char tmp_path[1100];
    int n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", yaml_path);
    if (n < 0 || n >= (int)sizeof tmp_path) {
        if (err && errsz) snprintf(err, errsz, "path too long");
        return false;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        if (err && errsz) snprintf(err, errsz, "open %s: %s",
                                   tmp_path, strerror(errno));
        return false;
    }

    /* Mirror filmextract's writer (src/tools/film/filmextract.c). The
     * inline-flow shape per frame is what the on-disk corpus uses;
     * keeping byte-for-byte parity makes diffs minimal. */
    if (fprintf(f,
                "atlas:\n"
                "  w: %d\n"
                "  h: %d\n",
                a->atlas_w, a->atlas_h) < 0)
        goto io_err;
    if (a->classic_atlas_w > 0 && a->classic_atlas_h > 0) {
        if (fprintf(f,
                    "  classic_w: %d\n"
                    "  classic_h: %d\n",
                    a->classic_atlas_w, a->classic_atlas_h) < 0)
            goto io_err;
    }
    if (fprintf(f, "frames:\n") < 0)
        goto io_err;
    for (int i = 0; i < a->frame_count; i++) {
        const AeronSpriteRect *r = &a->frames[i];
        int ox = a->origin_x ? a->origin_x[i] : 0;
        int oy = a->origin_y ? a->origin_y[i] : 0;
        /* Optional keys mirror what Load kept: ids for sparse
         * identities, pages for multi-image atlases, classic dims for
         * frame-geometry consumers. */
        char extra[48];
        char tail[48];
        int  en = 0;
        extra[0] = '\0';
        tail[0]  = '\0';
        if (a->ids)
            en += snprintf(extra + en, sizeof extra - (size_t)en,
                           "id: %d, ", (int)a->ids[i]);
        if (a->pages)
            en += snprintf(extra + en, sizeof extra - (size_t)en,
                           "page: %d, ", (int)a->pages[i]);
        if (a->classic_w && a->classic_h)
            snprintf(tail, sizeof tail, ", classic_w: %d, classic_h: %d",
                     (int)a->classic_w[i], (int)a->classic_h[i]);
        if (fprintf(f,
                    "  - { %sx: %d, y: %d, w: %d, h: %d, "
                    "origin_x: %d, origin_y: %d%s }\n",
                    extra,
                    round_pos(r->x), round_pos(r->y),
                    round_pos(r->w), round_pos(r->h),
                    ox, oy, tail) < 0)
            goto io_err;
    }

    if (fflush(f) != 0) goto io_err;
    if (fclose(f) != 0) {
        f = NULL;
        if (err && errsz) snprintf(err, errsz, "close %s: %s",
                                   tmp_path, strerror(errno));
        SDL_RemovePath(tmp_path);
        return false;
    }
    f = NULL;

    if (!SDL_RenamePath(tmp_path, yaml_path)) {
        if (err && errsz) snprintf(err, errsz, "rename %s -> %s: %s",
                                   tmp_path, yaml_path, SDL_GetError());
        SDL_RemovePath(tmp_path);
        return false;
    }
    return true;

io_err:
    if (err && errsz) snprintf(err, errsz, "write %s: %s",
                               tmp_path, strerror(errno));
    if (f) fclose(f);
    SDL_RemovePath(tmp_path);
    return false;
}
