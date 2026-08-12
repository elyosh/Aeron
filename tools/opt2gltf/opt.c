/*
 * opt.c - OPT mesh file parser. See opt.h for the public types.
 *
 * Header (8 bytes for version != 0, 4 bytes for version 0):
 *     int32 -version          (negative; absent => version = 0)
 *     int32 file_size - 8
 *
 * Container header (14 bytes, starts at file offset 8 / 4):
 *     int32 global_base_raw   (subtract OPT_HEADER_BYTES to get the bias
 *                              applied to every absolute pointer in the file)
 *     int16 _padding
 *     int32 root_node_count
 *     int32 root_table_ptr    (absolute pointer)
 *
 * Each Node is at least 24 bytes:
 *     int32 name_ptr          (0 or absolute pointer to ASCIIZ name)
 *     int32 node_type
 *     int32 child_count
 *     int32 child_table_ptr   (absolute pointer to int32[child_count])
 *     int32 p1                (per-node-type, often a count)
 *     int32 p2                (per-node-type, often a data pointer)
 *
 * After the header come (in this order, all optional): a name string, the
 * child-pointer table, the node's own data block, then the children's bodies.
 */

#include "opt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- Layout constants ----------------------------------------------------- */
#define OPT_VERSION_MARKER_RAW    ((int32_t)0xFFFFFFFE)  /* sentinel == -2 */

#define OPT_HEADER_BYTES                8   /* version int32 + size int32 */
#define OPT_HEADER_BYTES_LEGACY         4   /* version == 0: just size int32 */

#define OPT_CONTAINER_BYTES            14
#define OPT_CONTAINER_PAD_BYTES         2

#define OPT_NODE_HEADER_BYTES          24
#define OPT_NODE_OFF_NAME               0
#define OPT_NODE_OFF_TYPE               4
#define OPT_NODE_OFF_CHILD_COUNT        8
#define OPT_NODE_OFF_CHILD_TABLE       12
#define OPT_NODE_OFF_P1                16
#define OPT_NODE_OFF_P2                20

/* Per-node data block sizes / layouts */
#define OPT_VEC3_BYTES                 (3 * sizeof(float))
#define OPT_VEC2_BYTES                 (2 * sizeof(float))
#define OPT_FACE_INDEX_INTS            16  /* 4 each: verts, edges, uvs, normals */
#define OPT_FACE_INDEX_BYTES           (OPT_FACE_INDEX_INTS * sizeof(int32_t))
#define OPT_FACE_NORMAL_BYTES          OPT_VEC3_BYTES
#define OPT_FACE_TEX_BYTES             (2 * OPT_VEC3_BYTES) /* direction + magnitude */
#define OPT_FACE_BYTES                 (OPT_FACE_INDEX_BYTES + OPT_FACE_NORMAL_BYTES + OPT_FACE_TEX_BYTES)

/* MeshDescriptor data block: 2 int32 (mesh_type, explosion_type) +
 * 4 vec3 (span, center, bbox_min, bbox_max) + 1 int32 (target_id) +
 * 1 vec3 (target) = 72 bytes. The trailing target_id + target pair
 * was the source of the 56 vs 72 byte discrepancy with XWA's reader;
 * ~42% of 1998 meshes use it for component-level AI targeting. */
#define OPT_MESH_DESCRIPTOR_BYTES      (3 * sizeof(int32_t) + 5 * OPT_VEC3_BYTES)

/* Hardpoint data block: int32 type + vec3 pos */
#define OPT_HARDPOINT_BYTES            (sizeof(int32_t) + OPT_VEC3_BYTES)

/* EngineGlow data block: 3 int32 (disabled, core, outer) + 5 vec3
 * (dimensions, position, look/up/right axes) = 72 bytes. */
#define OPT_ENGINE_GLOW_BYTES          (3 * sizeof(int32_t) + 5 * OPT_VEC3_BYTES)

/* RotationScale data block: 4 vec3 (pivot + 3 axes) */
#define OPT_ROTATION_SCALE_BYTES       (4 * OPT_VEC3_BYTES)

/* Q15 axes remain in their on-disk scale. Consumers choose the conversion
 * required by their runtime representation. */

/* Texture data block (precedes pixels) */
#define OPT_TEX_OFF_PALETTE_PTR         0
#define OPT_TEX_OFF_PALETTE_TYPE        4
#define OPT_TEX_OFF_TEXTURE_SIZE        8
#define OPT_TEX_OFF_DATA_SIZE          12
#define OPT_TEX_OFF_WIDTH              16
#define OPT_TEX_OFF_HEIGHT             20
#define OPT_TEX_DATA_HEADER_BYTES      24

/* --- Read helpers --------------------------------------------------------- */
/*
 * Direct pointer arithmetic on the file buffer would work on little-endian
 * hosts (x86, ARM), but using memcpy keeps the parser correct under strict
 * aliasing rules and trivial to port to a big-endian host (swap inside ru32).
 */

static int32_t ru32(const uint8_t *buf, size_t off) {
    int32_t v;
    memcpy(&v, buf + off, sizeof v);
    return v;
}

static float rf32(const uint8_t *buf, size_t off) {
    float v;
    memcpy(&v, buf + off, sizeof v);
    return v;
}

static void rvec3(const uint8_t *buf, size_t off, opt_vec3_t *v) {
    v->x = rf32(buf, off);
    v->y = rf32(buf, off + sizeof(float));
    v->z = rf32(buf, off + 2 * sizeof(float));
}

static void rvec2(const uint8_t *buf, size_t off, opt_vec2_t *v) {
    v->u = rf32(buf, off);
    v->v = rf32(buf, off + sizeof(float));
}

/* --- Parser context ------------------------------------------------------- */
/*
 * Holds the file buffer, its length, the pointer bias (global_base), and
 * staging arrays for textures (which need deduplication by name).
 */
typedef struct {
    const uint8_t *buf;
    size_t         size;
    int32_t        global_base;

    /* Texture table being assembled during the parse. */
    opt_texture_t *textures;
    int32_t        texture_count;
    int32_t        texture_capacity;

    /* Texture index bound by the most recent top-level Texture root node,
     * -1 if none. The engine walks all roots with one shared draw state
     * (RenderScene_DrawObjectModel @ 0x481AD0), so a root-level Texture is
     * the default binding for every following mesh; some ships (e.g.
     * SYSTEMPATROLCRAFT) leave each mesh's first FaceData relying on it. */
    int32_t        root_texture;

    opt_error_t   *err;
    int            failed;
} opt_ctx_t;

static void set_err(opt_ctx_t *c, const char *fmt, ...) {
    if (c->err == NULL || c->failed) return;
    c->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err->msg, sizeof c->err->msg, fmt, ap);
    va_end(ap);
}

/* Convert an absolute on-disk pointer into a file offset (0 if pointer is 0). */
static size_t ptr_to_off(const opt_ctx_t *c, int32_t ptr) {
    return ptr == 0 ? 0 : (size_t)(ptr - c->global_base);
}

static int range_ok(const opt_ctx_t *c, size_t off, size_t len) {
    return off + len <= c->size;
}

static const char *cstring(const opt_ctx_t *c, size_t off) {
    if (off == 0 || off >= c->size) return "";
    /* Trust the file: scan up to the next NUL within bounds. */
    for (size_t i = off; i < c->size; i++) {
        if (c->buf[i] == 0) return (const char *)(c->buf + off);
    }
    return "";  /* not NUL-terminated, treat as empty */
}

/* --- Node header ---------------------------------------------------------- */
typedef struct {
    int32_t name_ptr;
    int32_t type;
    int32_t child_count;
    int32_t child_table_ptr;
    int32_t p1;
    int32_t p2;
    size_t  off;     /* file offset of this node */
} opt_node_t;

static int read_node(const opt_ctx_t *c, size_t off, opt_node_t *out) {
    if (!range_ok(c, off, OPT_NODE_HEADER_BYTES)) return 0;
    out->off              = off;
    out->name_ptr         = ru32(c->buf, off + OPT_NODE_OFF_NAME);
    out->type             = ru32(c->buf, off + OPT_NODE_OFF_TYPE);
    out->child_count      = ru32(c->buf, off + OPT_NODE_OFF_CHILD_COUNT);
    out->child_table_ptr  = ru32(c->buf, off + OPT_NODE_OFF_CHILD_TABLE);
    out->p1               = ru32(c->buf, off + OPT_NODE_OFF_P1);
    out->p2               = ru32(c->buf, off + OPT_NODE_OFF_P2);
    return 1;
}

/* Iterate node->children and call fn for each. fn returns 0 to keep going. */
typedef void (*child_fn_t)(opt_ctx_t *c, const opt_node_t *child, void *user);

static void for_each_child(opt_ctx_t *c, const opt_node_t *n,
                           child_fn_t fn, void *user) {
    if (n->child_count <= 0 || n->child_table_ptr == 0) return;
    size_t table = ptr_to_off(c, n->child_table_ptr);
    for (int32_t i = 0; i < n->child_count; i++) {
        if (!range_ok(c, table + (size_t)i * 4, 4)) return;
        int32_t cptr = ru32(c->buf, table + (size_t)i * 4);
        if (cptr == 0) continue;
        opt_node_t child;
        if (!read_node(c, ptr_to_off(c, cptr), &child)) continue;
        fn(c, &child, user);
        if (c->failed) return;
    }
}

/* --- Per-node-type data extraction ---------------------------------------- */

static void parse_mesh_vertices(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    int32_t count = n->p1;
    size_t  off   = ptr_to_off(c, n->p2);
    if (count <= 0 || off == 0) return;
    if (!range_ok(c, off, (size_t)count * OPT_VEC3_BYTES)) return;

    m->vertices = (opt_vec3_t *)calloc((size_t)count, sizeof *m->vertices);
    if (!m->vertices) { set_err(c, "out of memory (vertices)"); return; }
    m->vertex_count = count;
    for (int32_t i = 0; i < count; i++) {
        rvec3(c->buf, off + (size_t)i * OPT_VEC3_BYTES, &m->vertices[i]);
    }
}

static void parse_vertex_normals(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    int32_t count = n->p1;
    size_t  off   = ptr_to_off(c, n->p2);
    if (count <= 0 || off == 0) return;
    if (!range_ok(c, off, (size_t)count * OPT_VEC3_BYTES)) return;

    m->normals = (opt_vec3_t *)calloc((size_t)count, sizeof *m->normals);
    if (!m->normals) { set_err(c, "out of memory (normals)"); return; }
    m->normal_count = count;
    for (int32_t i = 0; i < count; i++) {
        rvec3(c->buf, off + (size_t)i * OPT_VEC3_BYTES, &m->normals[i]);
    }
}

static void parse_texcoords(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    int32_t count = n->p1;
    size_t  off   = ptr_to_off(c, n->p2);
    if (count <= 0 || off == 0) return;
    if (!range_ok(c, off, (size_t)count * OPT_VEC2_BYTES)) return;

    m->uvs = (opt_vec2_t *)calloc((size_t)count, sizeof *m->uvs);
    if (!m->uvs) { set_err(c, "out of memory (uvs)"); return; }
    m->uv_count = count;
    for (int32_t i = 0; i < count; i++) {
        rvec2(c->buf, off + (size_t)i * OPT_VEC2_BYTES, &m->uvs[i]);
    }
}

static void parse_mesh_descriptor(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    size_t off = ptr_to_off(c, n->p2);
    if (off == 0 || !range_ok(c, off, OPT_MESH_DESCRIPTOR_BYTES)) return;

    opt_mesh_descriptor_t *d = &m->descriptor;
    d->mesh_type      = (opt_mesh_type_t)ru32(c->buf, off + 0);
    d->explosion_type = ru32(c->buf, off + 4);
    rvec3(c->buf, off + 8 + 0 * OPT_VEC3_BYTES, &d->span);
    rvec3(c->buf, off + 8 + 1 * OPT_VEC3_BYTES, &d->center);
    rvec3(c->buf, off + 8 + 2 * OPT_VEC3_BYTES, &d->bbox_min);
    rvec3(c->buf, off + 8 + 3 * OPT_VEC3_BYTES, &d->bbox_max);
    /* AI / T-key target reference (trailing 16 bytes). XWA's reader
     * documents this layout; 1998 files author it ~42% of the time. */
    d->target_id      = ru32(c->buf, off + 8 + 4 * OPT_VEC3_BYTES);
    rvec3(c->buf, off + 8 + 4 * OPT_VEC3_BYTES + sizeof(int32_t),
          &d->target);
    m->has_descriptor = 1;
}

static void parse_hardpoint(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    size_t off = ptr_to_off(c, n->p2);
    if (off == 0 || !range_ok(c, off, OPT_HARDPOINT_BYTES)) return;

    opt_hardpoint_t *grown = (opt_hardpoint_t *)realloc(
        m->hardpoints, (size_t)(m->hardpoint_count + 1) * sizeof *grown);
    if (!grown) { set_err(c, "out of memory (hardpoints)"); return; }
    m->hardpoints = grown;
    opt_hardpoint_t *hp = &m->hardpoints[m->hardpoint_count++];
    hp->type = (opt_hardpoint_type_t)ru32(c->buf, off);
    rvec3(c->buf, off + sizeof(int32_t), &hp->pos);
}

static void parse_engine_glow(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    size_t off = ptr_to_off(c, n->p2);
    if (off == 0 || !range_ok(c, off, OPT_ENGINE_GLOW_BYTES)) return;

    opt_engine_glow_t *grown = (opt_engine_glow_t *)realloc(
        m->engine_glows, (size_t)(m->engine_glow_count + 1) * sizeof *grown);
    if (!grown) { set_err(c, "out of memory (engine glows)"); return; }
    m->engine_glows = grown;
    opt_engine_glow_t *g = &m->engine_glows[m->engine_glow_count++];
    g->is_disabled = ru32(c->buf, off + 0);
    g->core_color  = (uint32_t)ru32(c->buf, off + 4);
    g->outer_color = (uint32_t)ru32(c->buf, off + 8);
    rvec3(c->buf, off + 12 + 0 * OPT_VEC3_BYTES, &g->dimensions);
    rvec3(c->buf, off + 12 + 1 * OPT_VEC3_BYTES, &g->position);
    rvec3(c->buf, off + 12 + 2 * OPT_VEC3_BYTES, &g->look_axis);
    rvec3(c->buf, off + 12 + 3 * OPT_VEC3_BYTES, &g->up_axis);
    rvec3(c->buf, off + 12 + 4 * OPT_VEC3_BYTES, &g->right_axis);
}

static void parse_rotation_scale(opt_ctx_t *c, const opt_node_t *n, opt_mesh_t *m) {
    size_t off = ptr_to_off(c, n->p2);
    if (off == 0 || !range_ok(c, off, OPT_ROTATION_SCALE_BYTES)) return;

    rvec3(c->buf, off + 0 * OPT_VEC3_BYTES, &m->rotation_scale.pivot);
    rvec3(c->buf, off + 1 * OPT_VEC3_BYTES, &m->rotation_scale.rotation_axis);
    rvec3(c->buf, off + 2 * OPT_VEC3_BYTES, &m->rotation_scale.direction_axis);
    rvec3(c->buf, off + 3 * OPT_VEC3_BYTES, &m->rotation_scale.up_axis);

    m->has_rotation_scale = 1;
}

/* --- Texture deduplication -----------------------------------------------
 * Textures are stored as inline Texture nodes; other meshes may instead
 * carry a NodeReference whose data block is just the referenced name.
 * We collect every unique name into ctx->textures and return its index.
 */

static int32_t texture_lookup_or_add(opt_ctx_t *c, const char *name) {
    for (int32_t i = 0; i < c->texture_count; i++) {
        if (strncmp(c->textures[i].name, name, OPT_TEXTURE_NAME_MAX) == 0) {
            return i;
        }
    }
    if (c->texture_count == c->texture_capacity) {
        int32_t new_cap = c->texture_capacity == 0 ? 8 : c->texture_capacity * 2;
        opt_texture_t *grown = (opt_texture_t *)realloc(
            c->textures, (size_t)new_cap * sizeof *grown);
        if (!grown) { set_err(c, "out of memory (textures)"); return -1; }
        memset(grown + c->texture_capacity, 0,
               (size_t)(new_cap - c->texture_capacity) * sizeof *grown);
        c->textures = grown;
        c->texture_capacity = new_cap;
    }
    opt_texture_t *t = &c->textures[c->texture_count];
    strncpy(t->name, name, OPT_TEXTURE_NAME_MAX - 1);
    t->name[OPT_TEXTURE_NAME_MAX - 1] = 0;
    return c->texture_count++;
}

/* Count mip levels consistent with a given chain size. Mips halve each axis
 * (min 1) until both dimensions are 1; the file may store fewer levels than
 * the theoretical max if data_size stops earlier. */
static int32_t count_mip_levels(int32_t width, int32_t height, int32_t chain_bytes) {
    int32_t levels = 0;
    int32_t accumulated = 0;
    int32_t w = width, h = height;
    while (accumulated < chain_bytes) {
        int32_t level_bytes = w * h;
        if (accumulated + level_bytes > chain_bytes) break;
        accumulated += level_bytes;
        levels++;
        if (w == 1 && h == 1) break;
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }
    return levels < 1 ? 1 : levels;
}

static void parse_texture_data(opt_ctx_t *c, const opt_node_t *n, int32_t tex_index) {
    if (tex_index < 0) return;
    size_t data_off = ptr_to_off(c, n->p2);
    if (data_off == 0 || !range_ok(c, data_off, OPT_TEX_DATA_HEADER_BYTES)) return;

    int32_t palette_ptr  = ru32(c->buf, data_off + OPT_TEX_OFF_PALETTE_PTR);
    int32_t palette_type = ru32(c->buf, data_off + OPT_TEX_OFF_PALETTE_TYPE);
    int32_t texture_size = ru32(c->buf, data_off + OPT_TEX_OFF_TEXTURE_SIZE);
    int32_t data_size    = ru32(c->buf, data_off + OPT_TEX_OFF_DATA_SIZE);
    int32_t width        = ru32(c->buf, data_off + OPT_TEX_OFF_WIDTH);
    int32_t height       = ru32(c->buf, data_off + OPT_TEX_OFF_HEIGHT);

    opt_texture_t *t = &c->textures[tex_index];
    t->width  = width;
    t->height = height;

    /* If texture_size == 0, data_size == width*height and there are no mips.
     * Otherwise data_size covers the full halving mip chain starting at base. */
    size_t  pixels_off = data_off + OPT_TEX_DATA_HEADER_BYTES;
    int32_t chain_bytes = (texture_size == 0)
                          ? (int32_t)((size_t)width * (size_t)height)
                          : data_size;
    if (t->pixels == NULL && chain_bytes > 0
        && range_ok(c, pixels_off, (size_t)chain_bytes)) {
        t->pixels = (uint8_t *)malloc((size_t)chain_bytes);
        if (!t->pixels) { set_err(c, "out of memory (texture pixels)"); return; }
        memcpy(t->pixels, c->buf + pixels_off, (size_t)chain_bytes);
        t->mip_chain_bytes = chain_bytes;
        t->mip_count = (texture_size == 0) ? 1 : count_mip_levels(width, height, chain_bytes);
    }

    /* Palette: always 16 * 256 * 2 bytes. Two storage modes:
     *   palette_type == 0 → shared palette referenced via palette_ptr
     *                       (typical: dozens of textures share the same
     *                       palette to save space).
     *   palette_type != 0 → palette is appended right AFTER this
     *                       texture's pixel data. CORTN's Tex00008 +
     *                       Tex00009 use this — without honouring it
     *                       they sample a zero palette and render
     *                       solid black. Match the XWA reader's
     *                       handling: discriminate on palette_type and
     *                       pick the right source. */
    size_t pal_off = (palette_type == 0)
                     ? ptr_to_off(c, palette_ptr)
                     : pixels_off + (size_t)chain_bytes;
    if (pal_off != 0 && range_ok(c, pal_off, OPT_PALETTE_BYTES)) {
        memcpy(t->palette, c->buf + pal_off, OPT_PALETTE_BYTES);
    }

    /* Optional alpha: an XWA Texture node may carry a TextureAlpha child
     * (node type 26) whose data block is a 1-byte-per-pixel alpha map with
     * the same mip-chain length as the pixel data. p1 = byte count,
     * p2 = data pointer. Stored verbatim; opaque (no alpha) when absent. */
    if (t->alpha == NULL && n->child_count > 0 && n->child_table_ptr != 0) {
        size_t table = ptr_to_off(c, n->child_table_ptr);
        for (int32_t i = 0; i < n->child_count; i++) {
            if (!range_ok(c, table + (size_t)i * 4, 4)) break;
            int32_t cptr = ru32(c->buf, table + (size_t)i * 4);
            if (cptr == 0) continue;
            opt_node_t child;
            if (!read_node(c, ptr_to_off(c, cptr), &child)) continue;
            if (child.type != OPT_NODE_TEXTURE_ALPHA) continue;
            int32_t alpha_bytes = child.p1;
            size_t  alpha_off   = ptr_to_off(c, child.p2);
            if (alpha_bytes <= 0 || alpha_off == 0
                || !range_ok(c, alpha_off, (size_t)alpha_bytes)) break;
            t->alpha = (uint8_t *)malloc((size_t)alpha_bytes);
            if (!t->alpha) { set_err(c, "out of memory (texture alpha)"); return; }
            memcpy(t->alpha, c->buf + alpha_off, (size_t)alpha_bytes);
            break;
        }
    }
}

/* --- Face group walker ----------------------------------------------------
 * Inside one LOD NodeGroup, children alternate as:
 *     [Texture|NodeReference|NodeSwitch], FaceData, [Texture|...], FaceData, ...
 * A NodeSwitch carries N alternate textures (e.g. damage states). Both the
 * primary texture and the full state list get attached to the next FaceData.
 */
#define OPT_MAX_NODE_SWITCH_STATES  16   /* observed max is 4 across all IVFILES */

typedef struct {
    opt_face_group_t *groups;
    int32_t           group_count;
    int32_t           group_capacity;
    int32_t           current_texture;        /* state 0 binding, -1 if none */
    int32_t           current_state_count;    /* >=1 once any texture is bound */
    int32_t           current_states[OPT_MAX_NODE_SWITCH_STATES];
    int               pending_binding;        /* 1 if a binding child was the
                                                 last seen and no FaceData
                                                 has consumed it yet. */
} opt_lod_builder_t;

/* Resolve a single Texture / NodeReference child to its texture-table index. */
static int32_t resolve_texture_node(opt_ctx_t *c, const opt_node_t *n) {
    if (n->type == OPT_NODE_TEXTURE) {
        const char *name = cstring(c, ptr_to_off(c, n->name_ptr));
        int32_t idx = texture_lookup_or_add(c, name);
        if (idx >= 0) parse_texture_data(c, n, idx);
        return idx;
    }
    if (n->type == OPT_NODE_NODE_REFERENCE) {
        const char *name = cstring(c, ptr_to_off(c, n->p2));
        return texture_lookup_or_add(c, name);
    }
    return -1;
}

static void parse_face_data(opt_ctx_t *c, const opt_node_t *n, opt_lod_builder_t *lb) {
    int32_t face_count = n->p1;
    size_t  off        = ptr_to_off(c, n->p2);
    if (face_count <= 0 || off == 0) return;

    /* Layout of the data block:
     *     int32 edges_count
     *     int32 indices[face_count][16]         (verts/edges/uvs/normals)
     *     vec3  face_normals[face_count]
     *     vec3  tex_direction[face_count], tex_magnitude[face_count]   (interleaved)
     */
    size_t need = sizeof(int32_t) + (size_t)face_count * OPT_FACE_BYTES;
    if (!range_ok(c, off, need)) return;

    int32_t edges_count = ru32(c->buf, off);
    size_t idx_off  = off + sizeof(int32_t);
    size_t nrm_off  = idx_off + (size_t)face_count * OPT_FACE_INDEX_BYTES;
    size_t tex_off  = nrm_off + (size_t)face_count * OPT_VEC3_BYTES;

    if (lb->group_count == lb->group_capacity) {
        int32_t new_cap = lb->group_capacity == 0 ? 4 : lb->group_capacity * 2;
        opt_face_group_t *grown = (opt_face_group_t *)realloc(
            lb->groups, (size_t)new_cap * sizeof *grown);
        if (!grown) { set_err(c, "out of memory (face groups)"); return; }
        lb->groups = grown;
        lb->group_capacity = new_cap;
    }
    opt_face_group_t *fg = &lb->groups[lb->group_count++];
    fg->texture_index = lb->current_texture;
    fg->edges_count   = edges_count;
    fg->face_count    = face_count;
    fg->state_count   = (lb->current_state_count >= 1) ? lb->current_state_count : 1;
    fg->state_textures = NULL;
    if (fg->state_count > 1) {
        fg->state_textures = (int32_t *)malloc((size_t)fg->state_count * sizeof(int32_t));
        if (!fg->state_textures) {
            set_err(c, "out of memory (state textures)");
            lb->group_count--; return;
        }
        memcpy(fg->state_textures, lb->current_states,
               (size_t)fg->state_count * sizeof(int32_t));
    }
    fg->faces = (opt_face_t *)calloc((size_t)face_count, sizeof *fg->faces);
    if (!fg->faces) {
        free(fg->state_textures);
        set_err(c, "out of memory (faces)");
        lb->group_count--; return;
    }

    for (int32_t i = 0; i < face_count; i++) {
        opt_face_t *f = &fg->faces[i];
        size_t ix = idx_off + (size_t)i * OPT_FACE_INDEX_BYTES;
        for (int k = 0; k < 4; k++) f->verts[k]   = ru32(c->buf, ix + (size_t)(0 + k) * 4);
        for (int k = 0; k < 4; k++) f->edges[k]   = ru32(c->buf, ix + (size_t)(4 + k) * 4);
        for (int k = 0; k < 4; k++) f->uvs[k]     = ru32(c->buf, ix + (size_t)(8 + k) * 4);
        for (int k = 0; k < 4; k++) f->normals[k] = ru32(c->buf, ix + (size_t)(12 + k) * 4);
        rvec3(c->buf, nrm_off + (size_t)i * OPT_VEC3_BYTES, &f->face_normal);
        rvec3(c->buf, tex_off + (size_t)i * 2 * OPT_VEC3_BYTES + 0 * OPT_VEC3_BYTES,
              &f->tex_direction);
        rvec3(c->buf, tex_off + (size_t)i * 2 * OPT_VEC3_BYTES + 1 * OPT_VEC3_BYTES,
              &f->tex_magnitude);
    }
}

/* Texture-like node -> update lb->current_texture and lb->current_states. */
static void bind_texture(opt_ctx_t *c, const opt_node_t *n, opt_lod_builder_t *lb) {
    if (n->type == OPT_NODE_TEXTURE || n->type == OPT_NODE_NODE_REFERENCE) {
        int32_t idx = resolve_texture_node(c, n);
        if (idx < 0) return;
        lb->current_texture        = idx;
        lb->current_state_count    = 1;
        lb->current_states[0]      = idx;
    } else if (n->type == OPT_NODE_NODE_SWITCH) {
        /* Per-face-group runtime state slots. Children are Texture or
         * NodeReference nodes, one per state. State 0 is the base
         * binding; higher slots are selected at runtime by the engine
         * (camoflage byte / variant index).
         *
         * Two reasons we must walk EVERY child, not just the first
         * OPT_MAX_NODE_SWITCH_STATES: (1) some NodeSwitch parents
         * embed Texture data nodes as trailing children that other
         * meshes reference by name elsewhere in the file — capping
         * the loop early would skip resolve_texture_node on those and
         * leave their pixel data unparsed (Z-95.OPT does this for
         * Tex00026 at child[22] of a 23-child NodeSwitch). (2) The
         * cap still applies to the recorded state list because
         * current_states[] is a fixed-size buffer. */
        size_t  table = ptr_to_off(c, n->child_table_ptr);
        int32_t count = 0;
        for (int32_t i = 0; i < n->child_count; i++) {
            if (!range_ok(c, table + (size_t)i * 4, 4)) break;
            int32_t cptr = ru32(c->buf, table + (size_t)i * 4);
            if (cptr == 0) continue;
            opt_node_t child;
            if (!read_node(c, ptr_to_off(c, cptr), &child)) continue;
            int32_t idx = resolve_texture_node(c, &child);
            if (idx < 0) continue;
            if (count < OPT_MAX_NODE_SWITCH_STATES)
                lb->current_states[count++] = idx;
        }
        if (count > 0) {
            lb->current_state_count = count;
            lb->current_texture     = lb->current_states[0];
        }
    }
}

static void on_lod_child(opt_ctx_t *c, const opt_node_t *child, void *user) {
    opt_lod_builder_t *lb = (opt_lod_builder_t *)user;
    switch (child->type) {
        case OPT_NODE_TEXTURE:
        case OPT_NODE_NODE_REFERENCE:
        case OPT_NODE_NODE_SWITCH:
            bind_texture(c, child, lb);
            lb->pending_binding = 1;
            break;
        case OPT_NODE_FACE_DATA:
            parse_face_data(c, child, lb);
            /* A FaceData consumes the pending preceding binding. */
            lb->pending_binding = 0;
            break;
        case OPT_NODE_GROUP:
            /* Some 1998 OPTs nest Group nodes inside the LOD container
             * before the Texture/FaceData pairs. SHUTTLE.OPT's Fuselage
             * LOD 0 has 3 levels of nested Groups; without recursion
             * the parser drops every Texture/FaceData and the mesh
             * comes out empty (renderer skips it, opt2gltf doesn't
             * emit it). Recurse — current_texture / state / pending
             * carry across siblings via lb, so the binding semantics
             * are preserved even when the bindings sit deeper than
             * the LOD's direct children. */
            for_each_child(c, child, on_lod_child, lb);
            break;
        default:
            break;
    }
}

static void parse_lod_node(opt_ctx_t *c, const opt_node_t *lod_group_node,
                           float distance, opt_lod_t *out) {
    opt_lod_builder_t lb = { 0 };
    /* Inherit the top-level Texture root binding (see ctx.root_texture). */
    lb.current_texture     = c->root_texture;
    lb.current_state_count = 0;
    lb.pending_binding     = 0;
    if (c->root_texture >= 0) {
        lb.current_state_count = 1;
        lb.current_states[0]   = c->root_texture;
    }
    for_each_child(c, lod_group_node, on_lod_child, &lb);
    /* Trailing-orphan texture binding: when a LOD ends with a Texture /
     * NodeReference / NodeSwitch that wasn't consumed by a following
     * FaceData, the original 1998 X-Wing/TIE engine applies it to the
     * LAST emitted face group. The export tools sometimes emit the
     * texture binding AFTER the face data (instead of the usual before-
     * pattern) — most visibly on the top turrets of PLATA where without
     * this rule the second and third turret render untextured even
     * though they reference Tex00004.
     *
     * Only RECOVER missing bindings (texture_index == -1) — never
     * OVERWRITE an existing one. FRIGA's launcher mesh has a
     * Texture+FaceData pair (correctly binding Tex00031) followed by a
     * stray malformed NodeReference; the stray binding is meaningless
     * scratch data and the face group already has its real texture. */
    if (lb.pending_binding && lb.group_count > 0 &&
        lb.current_texture >= 0) {
        opt_face_group_t *last = &lb.groups[lb.group_count - 1];
        if (last->texture_index < 0) {
            last->texture_index = lb.current_texture;
            if (last->state_textures && last->state_count > 0)
                last->state_textures[0] = lb.current_texture;
        }
    }
    out->distance_threshold = distance;
    out->group_count = lb.group_count;
    out->groups      = lb.groups;
}

/* --- Mesh-level walking --------------------------------------------------
 * One mesh = one top-level NodeGroup. Its direct children mix the component
 * nodes (MeshVertices, MeshDescriptor, Hardpoint, RotationScale, ...) with
 * a sub-NodeGroup that wraps the FaceGrouping (LOD list).
 *
 * The XWA reader handles both layouts (children at the top vs nested one
 * level deeper) by treating the union of the two child lists; we do the
 * same via a recursive walker that descends through plain NodeGroups.
 */

typedef struct {
    opt_mesh_t *mesh;
    int         depth;       /* limits NodeGroup recursion */
} opt_mesh_builder_t;

static void on_mesh_descendant(opt_ctx_t *c, const opt_node_t *n, void *user);

static void parse_face_grouping(opt_ctx_t *c, const opt_node_t *fg,
                                opt_mesh_t *mesh) {
    int32_t lod_count = fg->p1;
    size_t  dist_off  = ptr_to_off(c, fg->p2);
    if (lod_count <= 0 || dist_off == 0) return;
    if (!range_ok(c, dist_off, (size_t)lod_count * sizeof(float))) return;

    mesh->lods = (opt_lod_t *)calloc((size_t)lod_count, sizeof *mesh->lods);
    if (!mesh->lods) { set_err(c, "out of memory (lods)"); return; }
    mesh->lod_count = lod_count;

    /* Read distances, then iterate the FaceGrouping's children (each is one
     * LOD NodeGroup). */
    size_t table = ptr_to_off(c, fg->child_table_ptr);
    for (int32_t i = 0; i < lod_count; i++) {
        float dist = rf32(c->buf, dist_off + (size_t)i * sizeof(float));
        if (i < fg->child_count && range_ok(c, table + (size_t)i * 4, 4)) {
            int32_t cptr = ru32(c->buf, table + (size_t)i * 4);
            if (cptr != 0) {
                opt_node_t lod_node;
                if (read_node(c, ptr_to_off(c, cptr), &lod_node)) {
                    parse_lod_node(c, &lod_node, dist, &mesh->lods[i]);
                    continue;
                }
            }
        }
        mesh->lods[i].distance_threshold = dist;
    }
}

static void on_mesh_descendant(opt_ctx_t *c, const opt_node_t *n, void *user) {
    opt_mesh_builder_t *mb = (opt_mesh_builder_t *)user;
    opt_mesh_t *m = mb->mesh;

    switch (n->type) {
        case OPT_NODE_MESH_VERTICES:       parse_mesh_vertices  (c, n, m); break;
        case OPT_NODE_VERTEX_NORMALS:      parse_vertex_normals (c, n, m); break;
        case OPT_NODE_TEXTURE_COORDINATES: parse_texcoords      (c, n, m); break;
        case OPT_NODE_MESH_DESCRIPTOR:     parse_mesh_descriptor(c, n, m); break;
        case OPT_NODE_HARDPOINT:           parse_hardpoint      (c, n, m); break;
        case OPT_NODE_ENGINE_GLOW:         parse_engine_glow    (c, n, m); break;
        case OPT_NODE_ROTATION_SCALE:      parse_rotation_scale (c, n, m); break;
        case OPT_NODE_FACE_GROUPING:       parse_face_grouping  (c, n, m); break;
        case OPT_NODE_GROUP:
            /* Recurse through nested groups (LOD container layer). */
            if (mb->depth < 2) {
                mb->depth++;
                for_each_child(c, n, on_mesh_descendant, mb);
                mb->depth--;
            }
            break;
        default:
            break;
    }
}

static void parse_mesh(opt_ctx_t *c, const opt_node_t *top, opt_mesh_t *out) {
    memset(out, 0, sizeof *out);
    opt_mesh_builder_t mb = { .mesh = out, .depth = 0 };
    for_each_child(c, top, on_mesh_descendant, &mb);
}

/* --- Top-level parse ------------------------------------------------------ */

static void opt_free_contents(opt_file_t *opt);  /* forward */

static opt_file_t *parse_from_buffer(const uint8_t *data, size_t size,
                                     opt_error_t *err) {
    if (size < OPT_HEADER_BYTES_LEGACY + OPT_CONTAINER_BYTES) {
        if (err) snprintf(err->msg, sizeof err->msg, "file too small (%zu bytes)", size);
        return NULL;
    }

    /* Parse the variable-length header. */
    int32_t first = ru32(data, 0);
    int32_t version;
    size_t  hdr_bytes;
    size_t  size_in_header;
    if (first > 0) {
        version        = 0;
        size_in_header = (size_t)first;
        hdr_bytes      = OPT_HEADER_BYTES_LEGACY;
    } else {
        version        = -first;
        size_in_header = (size_t)ru32(data, sizeof(int32_t));
        hdr_bytes      = OPT_HEADER_BYTES;
    }
    if (size - hdr_bytes != size_in_header) {
        if (err) snprintf(err->msg, sizeof err->msg,
                          "size mismatch: file=%zu header_expects=%zu",
                          size, size_in_header + hdr_bytes);
        return NULL;
    }

    /* Container header: global pointer bias + root node table. */
    size_t  cont = hdr_bytes;
    int32_t base_raw      = ru32(data, cont);
    int32_t root_count    = ru32(data, cont + sizeof(int32_t) + OPT_CONTAINER_PAD_BYTES);
    int32_t root_table_p  = ru32(data, cont + sizeof(int32_t) + OPT_CONTAINER_PAD_BYTES
                                          + sizeof(int32_t));
    int32_t global_base   = base_raw - (int32_t)hdr_bytes;

    opt_ctx_t c = { 0 };
    c.buf          = data;
    c.size         = size;
    c.global_base  = global_base;
    c.err          = err;
    c.root_texture = -1;

    if (root_count <= 0 || root_table_p == 0) {
        opt_file_t *f = (opt_file_t *)calloc(1, sizeof *f);
        if (f) f->version = version;
        return f;
    }

    /* Each root node is one mesh. */
    opt_mesh_t *meshes = (opt_mesh_t *)calloc((size_t)root_count, sizeof *meshes);
    if (!meshes) {
        if (err) snprintf(err->msg, sizeof err->msg, "out of memory (meshes)");
        return NULL;
    }

    size_t root_table = ptr_to_off(&c, root_table_p);
    int32_t mesh_count = 0;
    for (int32_t i = 0; i < root_count; i++) {
        int32_t cptr = ru32(data, root_table + (size_t)i * 4);
        if (cptr == 0) continue;
        opt_node_t top;
        if (!read_node(&c, ptr_to_off(&c, cptr), &top)) continue;
        if (top.type != OPT_NODE_GROUP && top.type != OPT_NODE_TEXTURE) continue;
        if (top.type == OPT_NODE_TEXTURE) {
            /* Shared Texture at top level: carries pixel data and becomes
             * the default binding for every mesh root after it. */
            const char *name = cstring(&c, ptr_to_off(&c, top.name_ptr));
            int32_t idx = texture_lookup_or_add(&c, name);
            parse_texture_data(&c, &top, idx);
            if (idx >= 0) c.root_texture = idx;
            continue;
        }
        parse_mesh(&c, &top, &meshes[mesh_count++]);
        if (c.failed) goto fail;
    }

    opt_file_t *f = (opt_file_t *)calloc(1, sizeof *f);
    if (!f) {
        if (err) snprintf(err->msg, sizeof err->msg, "out of memory (file)");
        goto fail;
    }
    f->version       = version;
    f->mesh_count    = mesh_count;
    f->meshes        = meshes;
    f->texture_count = c.texture_count;
    f->textures      = c.textures;
    return f;

fail: {
    /* Free everything assembled so far without touching opt_file storage. */
    opt_file_t tmp = { 0 };
    tmp.meshes        = meshes;
    tmp.mesh_count    = mesh_count;
    tmp.textures      = c.textures;
    tmp.texture_count = c.texture_count;
    opt_free_contents(&tmp);
    return NULL;
}
}

opt_file_t *opt_load_memory(const void *data, size_t size, opt_error_t *err) {
    if (err) err->msg[0] = 0;
    return parse_from_buffer((const uint8_t *)data, size, err);
}

opt_file_t *opt_load_file(const char *path, opt_error_t *err) {
    if (err) err->msg[0] = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err) snprintf(err->msg, sizeof err->msg, "cannot open '%s'", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        if (err) snprintf(err->msg, sizeof err->msg, "empty file '%s'", path);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        if (err) snprintf(err->msg, sizeof err->msg, "out of memory");
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf); fclose(fp);
        if (err) snprintf(err->msg, sizeof err->msg, "short read on '%s'", path);
        return NULL;
    }
    fclose(fp);
    opt_file_t *f = parse_from_buffer(buf, (size_t)size, err);
    free(buf);  /* parsed data has been copied out */
    return f;
}

/* Internal: free the arrays inside an opt_file without freeing the struct. */
static void opt_free_contents(opt_file_t *opt) {
    if (!opt) return;
    for (int32_t i = 0; i < opt->mesh_count; i++) {
        opt_mesh_t *m = &opt->meshes[i];
        free(m->vertices);
        free(m->normals);
        free(m->uvs);
        free(m->hardpoints);
        free(m->engine_glows);
        for (int32_t j = 0; j < m->lod_count; j++) {
            opt_lod_t *lod = &m->lods[j];
            for (int32_t k = 0; k < lod->group_count; k++) {
                free(lod->groups[k].faces);
                free(lod->groups[k].state_textures);
            }
            free(lod->groups);
        }
        free(m->lods);
    }
    free(opt->meshes);
    for (int32_t i = 0; i < opt->texture_count; i++) {
        free(opt->textures[i].pixels);
        free(opt->textures[i].alpha);
    }
    free(opt->textures);
}

void opt_free(opt_file_t *opt) {
    if (!opt) return;
    opt_free_contents(opt);
    free(opt);
}

/* --- Name-string helpers -------------------------------------------------- */

const char *opt_node_type_name(opt_node_type_t t) {
    switch (t) {
        case OPT_NODE_NULL:                return "Null";
        case OPT_NODE_GROUP:               return "NodeGroup";
        case OPT_NODE_FACE_DATA:           return "FaceData";
        case OPT_NODE_MESH_VERTICES:       return "MeshVertices";
        case OPT_NODE_NODE_REFERENCE:      return "NodeReference";
        case OPT_NODE_VERTEX_NORMALS:      return "VertexNormals";
        case OPT_NODE_TEXTURE_COORDINATES: return "TextureCoordinates";
        case OPT_NODE_TEXTURE:             return "Texture";
        case OPT_NODE_FACE_GROUPING:       return "FaceGrouping";
        case OPT_NODE_HARDPOINT:           return "Hardpoint";
        case OPT_NODE_ROTATION_SCALE:      return "RotationScale";
        case OPT_NODE_NODE_SWITCH:         return "NodeSwitch";
        case OPT_NODE_MESH_DESCRIPTOR:     return "MeshDescriptor";
        case OPT_NODE_TEXTURE_ALPHA:       return "TextureAlpha";
        case OPT_NODE_ENGINE_GLOW:         return "EngineGlow";
    }
    return "?";
}

const char *opt_mesh_type_name(opt_mesh_type_t t) {
    switch (t) {
        case OPT_MT_DEFAULT:             return "Default";
        case OPT_MT_MAIN_HULL:           return "MainHull";
        case OPT_MT_WING:                return "Wing";
        case OPT_MT_FUSELAGE:            return "Fuselage";
        case OPT_MT_GUN_TURRET:          return "GunTurret";
        case OPT_MT_SMALL_GUN:           return "SmallGun";
        case OPT_MT_ENGINE:              return "Engine";
        case OPT_MT_BRIDGE:              return "Bridge";
        case OPT_MT_SHIELD_GEN:          return "ShieldGen";
        case OPT_MT_ENERGY_GEN:          return "EnergyGen";
        case OPT_MT_LAUNCHER:            return "Launcher";
        case OPT_MT_COMM_SYS:            return "CommSys";
        case OPT_MT_BEAM_SYS:            return "BeamSys";
        case OPT_MT_COMMAND_BEAM:        return "CommandBeam";
        case OPT_MT_DOCKING_PLAT:        return "DockingPlat";
        case OPT_MT_LANDING_PLAT:        return "LandingPlat";
        case OPT_MT_HANGAR:              return "Hangar";
        case OPT_MT_CARGO_POD:           return "CargoPod";
        case OPT_MT_MISC_HULL:           return "MiscHull";
        case OPT_MT_ANTENNA:             return "Antenna";
        case OPT_MT_ROTARY_WING:         return "RotaryWing";
        case OPT_MT_ROTARY_GUN_TURRET:   return "RotaryGunTurret";
        case OPT_MT_ROTARY_LAUNCHER:     return "RotaryLauncher";
        case OPT_MT_ROTARY_COMM_SYS:     return "RotaryCommSys";
        case OPT_MT_ROTARY_BEAM_SYS:     return "RotaryBeamSys";
        case OPT_MT_ROTARY_COMMAND_BEAM: return "RotaryCommandBeam";
        case OPT_MT_HATCH:               return "Hatch";
        case OPT_MT_CUSTOM:              return "Custom";
        case OPT_MT_WEAPON_SYS_1:        return "WeaponSys1";
        case OPT_MT_WEAPON_SYS_2:        return "WeaponSys2";
        case OPT_MT_POWER_REGENERATOR:   return "PowerRegenerator";
        case OPT_MT_REACTOR:             return "Reactor";
    }
    return "?";
}

const char *opt_hardpoint_type_name(opt_hardpoint_type_t t) {
    switch (t) {
        case OPT_HP_NONE:                  return "None";
        case OPT_HP_LASER_CANNON:          return "LaserCannon";
        case OPT_HP_ION_CANNON:            return "IonCannon";
        case OPT_HP_TURBO_LASER:           return "TurboLaser";
        case OPT_HP_ION_TURBO_LASER:       return "IonTurboLaser";
        case OPT_HP_CLUSTER_MISSILE:       return "ClusterMissile";
        case OPT_HP_TORPEDO_MAG_PULSE:     return "TorpedoMagPulse";
        case OPT_HP_CONCUSSION_MISSILE:    return "ConcussionMissile";
        case OPT_HP_PROTON_TORPEDO:        return "ProtonTorpedo";
        case OPT_HP_ADV_CONCUSSION:        return "AdvConcussion";
        case OPT_HP_ADV_PROTON_TORPEDO:    return "AdvProtonTorpedo";
        case OPT_HP_ADV_TORPEDO_MAG_PULSE: return "AdvTorpedoMagPulse";
        case OPT_HP_BOMB:                  return "Bomb";
        case OPT_HP_BEAM_WEAPON:           return "BeamWeapon";
        case OPT_HP_DUMB_BOMB:             return "DumbBomb";
        case OPT_HP_FIRE_ROCKET:           return "FireRocket";
    }
    return "?";
}

int opt_mesh_type_is_rotary(opt_mesh_type_t t) {
    switch (t) {
        case OPT_MT_GUN_TURRET:
        case OPT_MT_ROTARY_WING:
        case OPT_MT_ROTARY_GUN_TURRET:
        case OPT_MT_ROTARY_LAUNCHER:
        case OPT_MT_ROTARY_COMM_SYS:
        case OPT_MT_ROTARY_BEAM_SYS:
        case OPT_MT_ROTARY_COMMAND_BEAM:
            return 1;
        default:
            return 0;
    }
}

int opt_rotation_scale_is_identity(const opt_rotation_scale_t *rs) {
    /* Static meshes carry one of two identity frames depending on chirality;
     * everything else is a real rotation. */
    static const opt_vec3_t za_pos = { 0, 0,  1 }, za_neg = { 0, 0, -1 };
    static const opt_vec3_t ya     = { 0, 1,  0 };
    static const opt_vec3_t xa_pos = { 1, 0,  0 }, xa_neg = {-1, 0,  0 };
    const float tol = 64.0f;
    #define NEAR(a, b) (fabsf((a).x - (b).x * OPT_Q15_UNIT) < tol && \
                        fabsf((a).y - (b).y * OPT_Q15_UNIT) < tol && \
                        fabsf((a).z - (b).z * OPT_Q15_UNIT) < tol)
    int pos = NEAR(rs->rotation_axis, za_pos) && NEAR(rs->direction_axis, ya)
              && NEAR(rs->up_axis, xa_pos);
    int neg = NEAR(rs->rotation_axis, za_neg) && NEAR(rs->direction_axis, ya)
              && NEAR(rs->up_axis, xa_neg);
    #undef NEAR
    return pos || neg;
}

size_t opt_texture_mip_offset(const opt_texture_t *t, int level,
                              int32_t *w, int32_t *h) {
    int32_t ww = t->width, hh = t->height;
    size_t off = 0;
    if (level < 0 || level >= t->mip_count) {
        if (w) *w = ww;
        if (h) *h = hh;
        return 0;
    }
    for (int i = 0; i < level; i++) {
        off += (size_t)ww * (size_t)hh;
        ww = (ww > 1) ? ww / 2 : 1;
        hh = (hh > 1) ? hh / 2 : 1;
    }
    if (w) *w = ww;
    if (h) *h = hh;
    return off;
}

int32_t opt_face_group_texture(const opt_face_group_t *g, int state) {
    if (g->state_count <= 1 || state == 0 || g->state_textures == NULL) {
        return g->texture_index;
    }
    if (state < 0 || state >= g->state_count) return -1;
    return g->state_textures[state];
}

int opt_palette_base_shade(const opt_file_t *opt) {
    /* The 16 palette rows are a brightness ramp; which row holds the
     * texture's unlit base color is a per-game convention. XWA (version 5)
     * centers the ramp on shade 8: rows 1..7 darken the base toward black,
     * rows 9..15 brighten it toward white, row 0 is cleared. The XWA
     * reference reader (JeremyAnsel.Xwa.Opt) always samples row 8. The
     * earlier TIE98/XvT/BoP family (version 2, or legacy 0) instead authors
     * the ramp in rows 8..15 with the fully-lit primary at row 15.
     *
     * Sampling row 15 of an XWA texture lands at the near-white top of its
     * ramp, where green sits one 565-step below red/blue — a washed-out,
     * faintly magenta result (the "wrong palette / red tint" symptom). */
    return (opt && opt->version >= 5) ? OPT_XWA_BASE_SHADE : OPT_PRIMARY_SHADE;
}

int opt_palette_classic_lit(const uint8_t palette[OPT_PALETTE_BYTES], int index) {
    /* Exact mirror of ModelTexture_FilterHardwarePalette @ 0x44A600:
     * 5-bit channels with green's LSB dropped ((c >> 6) & 0x1f). */
    uint16_t c0;
    memcpy(&c0, palette + (size_t)index * OPT_PALETTE_BPP, sizeof c0);
    const int b0 = c0 & 0x1f;
    const int g0 = (c0 >> 6) & 0x1f;
    const int r0 = (c0 >> 11) & 0x1f;
    if (b0 * b0 + g0 * g0 + r0 * r0 < 32) {
        return 0; /* dark when unlit — a normal surface */
    }
    for (int plane = 1; plane <= 6; plane++) {
        uint16_t cp;
        memcpy(&cp, palette + ((size_t)plane * OPT_PALETTE_COLORS +
                               (size_t)index) * OPT_PALETTE_BPP,
               sizeof cp);
        const int db = (int)(cp & 0x1f) - b0;
        const int dg = (int)((cp >> 6) & 0x1f) - g0;
        const int dr = (int)((cp >> 11) & 0x1f) - r0;
        if (db * db + dg * dg + dr * dr > 16) {
            return 0; /* brightness responds to illumination — lit surface */
        }
    }
    return 1;
}

int opt_palette_index_emissive(const opt_file_t *opt,
                               const uint8_t palette[OPT_PALETTE_BYTES],
                               int index, int tol) {
    int base = opt_palette_base_shade(opt);
    int dim  = (opt && opt->version >= 5) ? OPT_XWA_DIM_SHADE
                                          : OPT_TIE_DIM_SHADE;
    size_t base_off = (size_t)base * OPT_PALETTE_COLORS * OPT_PALETTE_BPP
                    + (size_t)index * OPT_PALETTE_BPP;
    size_t dim_off  = (size_t)dim  * OPT_PALETTE_COLORS * OPT_PALETTE_BPP
                    + (size_t)index * OPT_PALETTE_BPP;
    uint16_t cb, cd;
    memcpy(&cb, palette + base_off, sizeof cb);
    memcpy(&cd, palette + dim_off,  sizeof cd);
    unsigned rb = (cb >> 11) & 0x1F, gb = (cb >> 5) & 0x3F, bb = cb & 0x1F;
    unsigned rd = (cd >> 11) & 0x1F, gd = (cd >> 5) & 0x3F, bd = cd & 0x1F;
    /* Near-black colors are flat only because the 5-6-5 ramp collapses at
     * tiny values; they are dark surfaces, not lights. Match the XWA
     * reader's RGB565 thresholds. */
    if (rb <= 8 && gb <= 16 && bb <= 8) return 0;
    /* Glowing texels keep (within `tol`) their base color under reduced
     * lighting; normal texels darken by several steps. */
    if (abs((int)rb - (int)rd) > tol) return 0;
    if (abs((int)gb - (int)gd) > tol) return 0;
    if (abs((int)bb - (int)bd) > tol) return 0;
    return 1;
}

void opt_palette_rgb(const uint8_t palette[OPT_PALETTE_BYTES],
                     int shade, int index,
                     uint8_t *r, uint8_t *g, uint8_t *b) {
    size_t off = (size_t)shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP
               + (size_t)index * OPT_PALETTE_BPP;
    uint16_t c;
    memcpy(&c, palette + off, sizeof c);
    /* RGB565: RRRRR GGGGGG BBBBB. Scale up to 8-bit with rounding to match
     * Jeremy Ansel's XWA reader. */
    unsigned r5 = (c >> 11) & 0x1F;
    unsigned g6 = (c >> 5)  & 0x3F;
    unsigned b5 =  c        & 0x1F;
    *r = (uint8_t)((r5 * (255u * 2) + 31u)  / (31u * 2));
    *g = (uint8_t)((g6 * (255u * 2) + 63u)  / (63u * 2));
    *b = (uint8_t)((b5 * (255u * 2) + 31u)  / (31u * 2));
}
