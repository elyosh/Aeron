/*
 * opt2gltf — OPT → glTF 2.0 conversion. See opt2gltf.h for the
 * data-preservation contract.
 *
 * Coordinate convention conversion:
 *   OPT:  +X right, +Z up, -Y forward (right-handed)
 *   glTF: +X right, +Y up, -Z forward (right-handed)
 * Mapping:
 *   glTF.x = OPT.x
 *   glTF.y = OPT.z
 *   glTF.z = OPT.y
 * Applied to positions, normals, axes, pivots, and bounding boxes.
 * NOTE: a two-axis swap is a REFLECTION (det -1) — it flips handedness
 * and triangle winding (OPT's CW-with-outward-normal becomes CCW in
 * the glTF). Consumers must apply the same involution to EVERYTHING
 * (the aeron glb loader swaps Y/Z back on positions AND axes/pivots,
 * restoring the OPT frame consistently); mixing converted and
 * unconverted quantities silently negates rotations.
 */

#include "opt2gltf.h"
#include "opt.h"
#include "aeron/mesh_normals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "cgltf_write.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ===== Helpers ====================================================== */

static void swap_axis_v3(const opt_vec3_t *in, float out[3])
{
    out[0] = in->x;
    out[1] = in->z;
    out[2] = in->y;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static char *xprintf_dup(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

static void path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    int need_sep = (la > 0 && a[la - 1] != '/');
    snprintf(out, cap, "%s%s%s", a, need_sep ? "/" : "", b);
}

/* RGB565 word → 8-bit triple. */
static inline void rgb565_to_rgb8(uint16_t w,
                                  uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (w >> 11) & 0x1F;
    uint8_t g6 = (w >>  5) & 0x3F;
    uint8_t b5 =  w        & 0x1F;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Decode the base mip of an OPT texture into RGBA8 at the given palette
 * shade row (the file's base-color row; see opt_palette_base_shade).
 * Caller owns the returned buffer. Returns NULL on bad inputs. */
static uint8_t *decode_texture_rgba8(const opt_texture_t *t, int shade)
{
    if (!t || !t->pixels || t->width <= 0 || t->height <= 0) return NULL;
    const size_t base_bytes = (size_t)t->width * (size_t)t->height;
    uint8_t *out = (uint8_t *)malloc(base_bytes * 4);
    if (!out) return NULL;
    const uint8_t *src = t->pixels;
    const uint8_t *pal =
        t->palette + ((size_t)shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
    for (size_t i = 0; i < base_bytes; ++i) {
        uint8_t idx = src[i];
        uint16_t w = (uint16_t)pal[idx * 2] |
                     (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
        uint8_t r, g, b;
        rgb565_to_rgb8(w, &r, &g, &b);
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        /* Alpha from an XWA TextureAlpha node (base mip), else opaque. */
        out[i * 4 + 3] = t->alpha ? t->alpha[i] : 255;
    }
    return out;
}

/* Decode the base mip into an emissive RGBA8 map: glow color + alpha 255
 * on self-illuminated texels, transparent black elsewhere. The alpha is
 * the classic lightmap coverage mask; gltf_cook premultiplies it before
 * mip generation so the runtime can reconstruct the original gamma-space
 * filtering and SRCALPHA pass in the opaque PBR shader. Returns NULL if
 * the texture builds no emissive layer (so no emissive PNG/material is
 * emitted for it). Caller owns the returned buffer.
 *
 * XWA (version 5) files replicate the engine EXACTLY
 * (ModelTexture_FilterHardwarePalette + CreateD3DfromTexture): entries
 * classified by opt_palette_classic_lit, glow color from palette row 10,
 * no lightmap for textures named "_*" or when all/none of the 256
 * entries classify lit. Legacy files keep the flat-ramp heuristic with
 * `tol`, sampling the base shade. */
static uint8_t *decode_texture_emissive_rgba8(const opt_file_t *opt,
                                              const opt_texture_t *t,
                                              int shade, int tol)
{
    if (!t || !t->pixels || t->width <= 0 || t->height <= 0) return NULL;
    const int xwa = opt && opt->version >= 5;
    if (xwa && t->name[0] == '_') {
        return NULL; /* engine gate: underscore textures never glow */
    }

    int lit[OPT_PALETTE_COLORS];
    int lit_count = 0;
    for (int i = 0; i < OPT_PALETTE_COLORS; ++i) {
        lit[i] = xwa ? opt_palette_classic_lit(t->palette, i)
                     : opt_palette_index_emissive(opt, t->palette, i, tol);
        lit_count += lit[i];
    }
    /* Engine gate (clearedCount at 0x44A600): a lightmap builds only
     * when SOME but not ALL palette entries classify lit. */
    if (xwa && (lit_count == 0 || lit_count == OPT_PALETTE_COLORS)) {
        return NULL;
    }

    const int glow_shade = xwa ? OPT_XWA_GLOW_SHADE : shade;
    const size_t base_bytes = (size_t)t->width * (size_t)t->height;
    const uint8_t *src = t->pixels;
    const uint8_t *pal =
        t->palette + ((size_t)glow_shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
    uint8_t *out = NULL;
    for (size_t i = 0; i < base_bytes; ++i) {
        uint8_t idx = src[i];
        if (!lit[idx]) {
            if (out) { out[i * 4 + 0] = 0; out[i * 4 + 1] = 0;
                       out[i * 4 + 2] = 0; out[i * 4 + 3] = 0; }
            continue;
        }
        if (!out) {
            out = (uint8_t *)calloc(base_bytes, 4);
            if (!out) return NULL;
        }
        uint16_t w = (uint16_t)pal[idx * 2] |
                     (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
        uint8_t r, g, b;
        rgb565_to_rgb8(w, &r, &g, &b);
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        out[i * 4 + 3] = 255;
    }
    return out;  /* NULL when no texel qualified */
}

static int ensure_dir(const char *path)
{
    /* mkdir -p semantics for one level; assumes the parent exists. */
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* ===== Build context ================================================ */

/* Linear allocator for arrays of cgltf structs — we know all the
 * sizes after a counting pass, so one big malloc per array. */
typedef struct {
    /* Buffers (one .bin file with vertices + indices interleaved per
     * mesh). Byte stream + buffer_views into it + accessors. */
    uint8_t           *bin;
    size_t             bin_size;
    size_t             bin_cap;

    cgltf_buffer       buffer;
    cgltf_buffer_view *views;       size_t views_count, views_cap;
    cgltf_accessor    *accessors;   size_t accessors_count, accessors_cap;
    cgltf_image       *images;      size_t images_count;
    cgltf_texture     *textures;    size_t textures_count;
    cgltf_sampler      sampler;     /* single repeat/linear sampler shared by all */
    cgltf_material    *materials;   size_t materials_count;
    cgltf_mesh        *meshes;      size_t meshes_count;
    cgltf_primitive   *primitives;  size_t primitives_count, primitives_cap;
    cgltf_attribute   *attributes;  size_t attributes_count, attributes_cap;
    cgltf_material_mapping *mappings; size_t mappings_count, mappings_cap;
    cgltf_material_variant *variants; size_t variants_count;
    cgltf_node        *nodes;       size_t nodes_count, nodes_cap;
    cgltf_node       **root_children; size_t root_children_count;

    /* Stable strings emitted into the JSON (cgltf doesn't deep-copy on
     * write — we own the pointer lifetimes). */
    char             **owned_strings; size_t owned_strings_count, owned_strings_cap;
} GltfBuild;

typedef struct OptGltfOwnedImage {
    uint8_t *rgba;
    uint32_t width;
    uint32_t height;
} OptGltfOwnedImage;

struct OptGltfDocument {
    GltfBuild gb;
    cgltf_data data;
    cgltf_scene scene;
    cgltf_node **scene_roots;
    OptGltfOwnedImage *image_pixels;
    size_t image_pixels_count;
};

static void *push_array(void **arr, size_t *count, size_t *cap,
                        size_t elem_size, size_t want)
{
    if (*count + want > *cap) {
        /* Reaching this branch means the pre-counting pass missed an
         * entry. cgltf records pointers (not indices) into these arrays
         * and writes them by pointer arithmetic at serialization time,
         * so a realloc here would dangle every pointer captured before
         * this call. Abort with a diagnostic instead of silently
         * corrupting the output. */
        fprintf(stderr,
                "opt2gltf: push_array would realloc "
                "(count=%zu + want=%zu > cap=%zu); pre-count missed.\n",
                *count, want, *cap);
        abort();
    }
    void *slot = (char *)*arr + *count * elem_size;
    *count += want;
    return slot;
}

/* owned_strings: realloc-safe pointer vector (cgltf indexes nothing
 * into this; it only references the strings themselves, which we
 * allocate separately and never move). */
static char *gb_keep_string(GltfBuild *gb, char *s)
{
    if (gb->owned_strings_count + 1 > gb->owned_strings_cap) {
        size_t ncap = gb->owned_strings_cap ? gb->owned_strings_cap * 2 : 64;
        char **grown = (char **)realloc(gb->owned_strings,
                                        ncap * sizeof(char *));
        if (!grown) { fprintf(stderr, "opt2gltf: oom\n"); abort(); }
        gb->owned_strings     = grown;
        gb->owned_strings_cap = ncap;
    }
    gb->owned_strings[gb->owned_strings_count++] = s;
    return s;
}

/* Append bytes to the .bin buffer with 4-byte alignment as required by
 * the glTF accessor spec for component bounds. Returns the byte offset
 * where the data starts. */
static size_t bin_append(GltfBuild *gb, const void *data, size_t bytes)
{
    if (gb->bin_size + bytes + 4 > gb->bin_cap) {
        size_t ncap = gb->bin_cap ? gb->bin_cap * 2 : 4096;
        while (ncap < gb->bin_size + bytes + 4) ncap *= 2;
        uint8_t *grown = (uint8_t *)realloc(gb->bin, ncap);
        if (!grown) { fprintf(stderr, "opt2gltf: oom\n"); abort(); }
        gb->bin = grown;
        gb->bin_cap = ncap;
    }
    /* 4-byte align (cgltf writes accessor.componentType-specific offsets,
     * float and uint16 alignments fit inside 4-byte alignment). */
    size_t off = (gb->bin_size + 3) & ~(size_t)3;
    while (gb->bin_size < off) gb->bin[gb->bin_size++] = 0;
    memcpy(gb->bin + gb->bin_size, data, bytes);
    gb->bin_size += bytes;
    return off;
}

static cgltf_buffer_view *gb_new_view(GltfBuild *gb,
                                      size_t offset, size_t bytes,
                                      cgltf_buffer_view_type type)
{
    cgltf_buffer_view *v = (cgltf_buffer_view *)push_array(
        (void **)&gb->views, &gb->views_count, &gb->views_cap,
        sizeof *v, 1);
    v->buffer = &gb->buffer;
    v->offset = offset;
    v->size   = bytes;
    v->type   = type;
    return v;
}

static cgltf_accessor *gb_new_accessor(GltfBuild *gb,
                                       cgltf_buffer_view *view,
                                       cgltf_size count,
                                       cgltf_type type,
                                       cgltf_component_type comp)
{
    cgltf_accessor *a = (cgltf_accessor *)push_array(
        (void **)&gb->accessors, &gb->accessors_count, &gb->accessors_cap,
        sizeof *a, 1);
    a->buffer_view    = view;
    a->offset         = 0;
    a->count          = count;
    a->type           = type;
    a->component_type = comp;
    /* Parsed cgltf graphs derive this during pointer fixup. Our graph is
     * constructed directly, so populate it explicitly for accessor reads. */
    a->stride         = cgltf_calc_size(type, comp);
    return a;
}

/* ===== Per-OPT-mesh emit ============================================ */

/* OPT face data is per-corner-indexed (separate verts/uvs/normals
 * arrays per face vertex). glTF needs (position, normal, uv) tuples
 * per vertex. In stored-normal mode we first reproduce the classic
 * renderer's position-index remap (see built_mesh_from_opt), then
 * dedup-by-tuple within a mesh so UV seams still split vertices while
 * carrying the same effective lighting normal.
 *
 * For the IVFILES set, faces are small (3-4 corners) and dedup keys
 * fit in a small hash. We use an unsorted linear scan — meshes are
 * under a couple thousand vertices, so it's fast enough.
 */

typedef struct {
    float pos[3];
    float nrm[3];
    float uv [2];
} GltfVertex;

typedef struct {
    GltfVertex *vertices;
    uint16_t   *indices;        /* per face group, contiguous */
    uint32_t    vertex_count;
    uint32_t    vertex_cap;
    uint32_t    index_count;
    uint32_t    index_cap;
    uint32_t   *fg_index_start;
    uint32_t   *fg_index_count_arr;
    uint32_t    fg_count;       /* face groups with non-empty indices */
    /* Defective canonical source normals repaired at emit (see the
     * classic-remap block in built_mesh_from_opt). */
    uint32_t    fixed_zero_normals;
    uint32_t    fixed_flipped_normals;
} BuiltMesh;

static void built_mesh_free(BuiltMesh *bm)
{
    free(bm->vertices);
    free(bm->indices);
    free(bm->fg_index_start);
    free(bm->fg_index_count_arr);
    memset(bm, 0, sizeof *bm);
}

static uint16_t built_mesh_intern_vertex(BuiltMesh *bm,
                                         const GltfVertex *v)
{
    /* Linear dedup. fine for OPT mesh sizes. */
    for (uint32_t i = 0; i < bm->vertex_count; ++i) {
        const GltfVertex *e = &bm->vertices[i];
        if (e->pos[0] == v->pos[0] && e->pos[1] == v->pos[1] && e->pos[2] == v->pos[2]
         && e->nrm[0] == v->nrm[0] && e->nrm[1] == v->nrm[1] && e->nrm[2] == v->nrm[2]
         && e->uv [0] == v->uv [0] && e->uv [1] == v->uv [1])
            return (uint16_t)i;
    }
    if (bm->vertex_count >= bm->vertex_cap) {
        uint32_t ncap = bm->vertex_cap ? bm->vertex_cap * 2 : 64;
        GltfVertex *grown = (GltfVertex *)realloc(bm->vertices,
                                                  ncap * sizeof *grown);
        if (!grown) { fprintf(stderr, "opt2gltf: oom\n"); abort(); }
        bm->vertices = grown;
        bm->vertex_cap = ncap;
    }
    bm->vertices[bm->vertex_count] = *v;
    return (uint16_t)bm->vertex_count++;
}

static void built_mesh_emit_index(BuiltMesh *bm, uint16_t v)
{
    if (bm->index_count >= bm->index_cap) {
        uint32_t ncap = bm->index_cap ? bm->index_cap * 2 : 128;
        uint16_t *grown = (uint16_t *)realloc(bm->indices,
                                              ncap * sizeof *grown);
        if (!grown) { fprintf(stderr, "opt2gltf: oom\n"); abort(); }
        bm->indices = grown;
        bm->index_cap = ncap;
    }
    bm->indices[bm->index_count++] = v;
}

/* ===== Angle-based normal regeneration =============================
 *
 * This path deliberately ignores all normals stored in the OPT. Some
 * shipped models contain mutually incompatible per-corner normals, and
 * even their stored face normals need not be derived consistently from
 * the polygon geometry.
 *
 * A smoothing angle describes EDGES, not an arbitrary set of faces that
 * happen to reference the same position. Build the connected smoothing
 * fan containing the current corner by walking shared edges whose
 * geometric face normals meet the threshold. Faces that touch only at a
 * point, or belong to a different fan separated by a hard edge, cannot
 * contaminate the result.
 *
 * Within the fan, weight each geometric face normal by its corner angle.
 * This is invariant under ordinary triangulation of a flat polygon: the
 * diagonal of a two-triangle panel no longer gives one side extra weight.
 * It also avoids making a dense patch of small triangles dominate a
 * neighbouring coarse patch. */
static float *built_mesh_build_corner_normals(const opt_mesh_t *mesh,
                                               const opt_lod_t *lod,
                                               float smooth_angle_degrees)
{
    uint32_t triangle_count = 0;
    for (int32_t group = 0; group < lod->group_count; ++group) {
        const opt_face_group_t *face_group = &lod->groups[group];
        for (int32_t face = 0; face < face_group->face_count; ++face)
            triangle_count += face_group->faces[face].verts[3] >= 0 ? 2u : 1u;
    }
    if (!triangle_count || mesh->vertex_count <= 0) return NULL;

    uint32_t *indices = malloc((size_t)triangle_count * 3 * sizeof *indices);
    float *normals = malloc((size_t)triangle_count * 9 * sizeof *normals);
    if (!indices || !normals) {
        free(indices);
        free(normals);
        return NULL;
    }
    uint32_t triangle = 0;
    for (int32_t group = 0; group < lod->group_count; ++group) {
        const opt_face_group_t *face_group = &lod->groups[group];
        for (int32_t face = 0; face < face_group->face_count; ++face) {
            const opt_face_t *source = &face_group->faces[face];
            const int source_corner[2][3] = {{0, 2, 1}, {0, 3, 2}};
            const int count = source->verts[3] >= 0 ? 2 : 1;
            for (int item = 0; item < count; ++item, ++triangle) {
                for (int corner = 0; corner < 3; ++corner) {
                    const int32_t vertex = source->verts[source_corner[item][corner]];
                    if (vertex < 0 || vertex >= mesh->vertex_count) {
                        free(indices);
                        free(normals);
                        return NULL;
                    }
                    indices[(size_t)triangle * 3 + corner] = (uint32_t)vertex;
                }
            }
        }
    }
    AeronMeshNormalsError error = {0};
    const AeronMeshNormalsInput input = {
        .positions = &mesh->vertices[0].x,
        .position_stride = sizeof mesh->vertices[0],
        .position_count = (uint32_t)mesh->vertex_count,
        .triangle_position_indices = indices,
        .triangle_count = triangle_count,
        .smooth_angle_degrees = smooth_angle_degrees,
    };
    const bool built = Aeron_MeshNormalsBuildCorners(&input, normals, &error);
    free(indices);
    if (!built) {
        fprintf(stderr, "opt2gltf: normal build failed: %s\n", error.message);
        free(normals);
        return NULL;
    }
    return normals;
}

static uint16_t built_mesh_emit_corner(BuiltMesh *bm, const opt_mesh_t *m,
                                       int32_t vi, int32_t ui,
                                       const opt_vec3_t *normal,
                                       float vertex_scale)
{
    const opt_vec3_t *p = &m->vertices[vi];
    const opt_vec2_t *uv =
        (ui >= 0 && ui < m->uv_count) ? &m->uvs[ui] : NULL;
    GltfVertex gv = {0};
    swap_axis_v3(p, gv.pos);
    gv.pos[0] *= vertex_scale;
    gv.pos[1] *= vertex_scale;
    gv.pos[2] *= vertex_scale;
    swap_axis_v3(normal, gv.nrm);
    /* Stored OPT normals are decoded from Q15 and may be slightly off
     * unit length; regenerated normals are already unit length, but use
     * the same final normalization for both paths. */
    float nl = sqrtf(gv.nrm[0] * gv.nrm[0] +
                     gv.nrm[1] * gv.nrm[1] +
                     gv.nrm[2] * gv.nrm[2]);
    if (nl > 1e-9f) {
        gv.nrm[0] /= nl;
        gv.nrm[1] /= nl;
        gv.nrm[2] /= nl;
    }
    if (uv) {
        gv.uv[0] = uv->u;
        gv.uv[1] = uv->v;
    }
    return built_mesh_intern_vertex(bm, &gv);
}

/* Walk LOD 0 of an OPT mesh and emit deduplicated vertices + indices
 * grouped by face group. Skips degenerate faces and empty groups.
 * `regen` enables angle-based normal regeneration; when false the stored
 * OPT normals are used. */
static bool built_mesh_from_opt(BuiltMesh *bm, const opt_mesh_t *m,
                                float vertex_scale,
                                bool regen, float smooth_angle_degrees,
                                bool repair_normals)
{
    memset(bm, 0, sizeof *bm);
    if (m->lod_count <= 0) return false;
    const opt_lod_t *lod = &m->lods[0];
    if (lod->group_count <= 0) return false;

    bm->fg_index_start     = (uint32_t *)calloc(lod->group_count,
                                                sizeof(uint32_t));
    bm->fg_index_count_arr = (uint32_t *)calloc(lod->group_count,
                                                sizeof(uint32_t));
    if (!bm->fg_index_start || !bm->fg_index_count_arr) return false;

    float *corner_normals = regen
        ? built_mesh_build_corner_normals(m, lod, smooth_angle_degrees)
        : NULL;
    if (regen && !corner_normals) {
        built_mesh_free(bm);
        return false;
    }

    /* The classic renderer resets g_vertexRemap for every FaceData group,
     * then computes lighting only when a POSITION index is first seen in
     * the visible, file-ordered face list. Later corners with the same
     * position reuse that already-lit vertex even when their normal index
     * differs. Many shipped OPTs rely on this: the ignored corner normals
     * can disagree by more than 90 degrees across the diagonal of an
     * otherwise coplanar two-triangle panel.
     *
     * A static glTF cannot reproduce the view-dependent "first visible"
     * choice at a non-coplanar junction. File order is exact for coplanar
     * pairs, however, because both faces have identical culling, and is the
     * closest stable representation elsewhere. Keep --no-normal-repair as
     * the raw per-corner diagnostic path, so this compatibility remap is
     * enabled together with the normal sanitization default. */
    opt_vec3_t *classic_normals = NULL;
    uint8_t *classic_normal_set = NULL;
    if (!regen && repair_normals && m->vertex_count > 0) {
        classic_normals = (opt_vec3_t *)calloc((size_t)m->vertex_count,
                                               sizeof *classic_normals);
        classic_normal_set = (uint8_t *)calloc((size_t)m->vertex_count,
                                               sizeof *classic_normal_set);
        if (!classic_normals || !classic_normal_set) {
            free(classic_normals);
            free(classic_normal_set);
            free(corner_normals);
            return false;
        }
    }

    int32_t smooth_face_index = 0;
    for (int32_t g = 0; g < lod->group_count; ++g) {
        const opt_face_group_t *grp = &lod->groups[g];
        if (grp->face_count <= 0) continue;
        if (classic_normal_set) {
            memset(classic_normal_set, 0, (size_t)m->vertex_count);
        }
        uint32_t start = bm->index_count;
        for (int32_t fi = 0; fi < grp->face_count; ++fi) {
            const opt_face_t *f = &grp->faces[fi];
            int corners = (f->verts[3] >= 0) ? 4 : 3;

            if (regen) {
                int source_corner[2][3] = {{0, 1, 2}, {0, 2, 3}};
                int triangle_count = (corners == 4) ? 2 : 1;
                for (int t = 0; t < triangle_count;
                     ++t, ++smooth_face_index) {
                    uint16_t v[3] = {0};
                    int ok = 1;
                    for (int q = 0; q < 3; ++q) {
                        int k = source_corner[t][q];
                        int32_t vi = f->verts[k];
                        if (vi < 0 || vi >= m->vertex_count) {
                            ok = 0;
                            break;
                        }
                        static const int normal_corner[3] = {0, 2, 1};
                        const float *normal = &corner_normals[
                            ((size_t)smooth_face_index * 3 + normal_corner[q]) * 3];
                        opt_vec3_t n = {normal[0], normal[1], normal[2]};
                        v[q] = built_mesh_emit_corner(
                            bm, m, vi, f->uvs[k], &n, vertex_scale);
                    }
                    if (!ok) continue;
                    built_mesh_emit_index(bm, v[0]);
                    built_mesh_emit_index(bm, v[1]);
                    built_mesh_emit_index(bm, v[2]);
                }
                continue;
            }

            uint16_t v[4] = {0};
            int ok = 1;
            for (int k = 0; k < corners; ++k) {
                int32_t vi = f->verts[k];
                int32_t ni = f->normals[k];
                if (vi < 0 || vi >= m->vertex_count) { ok = 0; break; }
                const opt_vec3_t *n =
                    (ni >= 0 && ni < m->normal_count) ? &m->normals[ni]
                                                      : &f->face_normal;
                if (classic_normal_set) {
                    if (!classic_normal_set[vi]) {
                        /* Sanitize only the canonical, first-use normal.
                         * Later per-corner normals are ignored by the
                         * classic renderer and must not be repaired
                         * independently: doing so turns them into real
                         * glTF seams. */
                        double nlen2 = (double)n->x * n->x +
                                       (double)n->y * n->y +
                                       (double)n->z * n->z;
                        if (nlen2 < 1e-12) {
                            classic_normals[vi] = f->face_normal;
                            bm->fixed_zero_normals++;
                        } else {
                            double d = (double)n->x * f->face_normal.x +
                                       (double)n->y * f->face_normal.y +
                                       (double)n->z * f->face_normal.z;
                            if (d < 0.0) {
                                classic_normals[vi] = (opt_vec3_t){
                                    -n->x, -n->y, -n->z
                                };
                                bm->fixed_flipped_normals++;
                            } else {
                                classic_normals[vi] = *n;
                            }
                        }
                        classic_normal_set[vi] = 1;
                    }
                    n = &classic_normals[vi];
                }
                v[k] = built_mesh_emit_corner(
                    bm, m, vi, f->uvs[k], n, vertex_scale);
            }
            if (!ok) continue;
            built_mesh_emit_index(bm, v[0]);
            built_mesh_emit_index(bm, v[1]);
            built_mesh_emit_index(bm, v[2]);
            if (corners == 4) {
                built_mesh_emit_index(bm, v[0]);
                built_mesh_emit_index(bm, v[2]);
                built_mesh_emit_index(bm, v[3]);
            }
        }
        bm->fg_index_start[g]     = start;
        bm->fg_index_count_arr[g] = bm->index_count - start;
        if (bm->fg_index_count_arr[g] > 0) bm->fg_count++;
    }
    free(corner_normals);
    free(classic_normals);
    free(classic_normal_set);
    return bm->index_count > 0;
}

/* ===== Conversion ==================================================== */

bool OptGltf_BuildMemory(const opt_file_t *opt,
                         const char *basename,
                         const OptGltfBuildOptions *options,
                         OptGltfDocument **out_document,
                         opt_error_t *error)
{
    if (out_document) *out_document = NULL;
    if (error) error->msg[0] = '\0';
    if (!opt || !basename || !out_document) return false;
    const float smooth_angle_deg = options ? options->smooth_angle_degrees : -1.0f;
    const bool repair_normals = options ? options->repair_normals : true;
    const bool emissive = options ? options->emissive : false;
    float vertex_scale = options ? options->vertex_scale : 1.0f;
    if (!(vertex_scale > 0.0f)) vertex_scale = 1.0f;

    OptGltfDocument *document = (OptGltfDocument *)calloc(1, sizeof *document);
    if (!document) {
        if (error) snprintf(error->msg, sizeof error->msg, "out of memory");
        return false;
    }
    GltfBuild *build = &document->gb;
    int *tex_emissive = NULL;
    BuiltMesh *built = NULL;

    /* Negative angle → keep the original OPT normals; otherwise
     * regenerate with an angle-based smoothing threshold. */
    const bool  regen      = (smooth_angle_deg >= 0.0f);
    build->buffer.size = 0;       /* filled at end */
    build->buffer.uri  = xprintf_dup("%s.bin", basename);
    gb_keep_string(build, build->buffer.uri);

    /* Sampler — single, repeat + trilinear. */
    build->sampler.mag_filter = 9729;          /* LINEAR */
    build->sampler.min_filter = 9987;          /* LINEAR_MIPMAP_LINEAR */
    build->sampler.wrap_s     = 10497;         /* REPEAT */
    build->sampler.wrap_t     = 10497;         /* REPEAT */

    /* ---- Textures + images + materials --------------------------- */
    /* Base-color palette row depends on the authoring game (XWA = 8,
     * TIE98/XvT = 15); sampling the wrong row washes XWA textures out
     * toward a faintly magenta white. */
    const int base_shade = opt_palette_base_shade(opt);

    /* Self-illuminated texels (flat palette ramp) become a second, emissive
     * texture/image per affected base texture (opt-in via --emissive). Count
     * them first so the image/texture arrays — which cgltf indexes by
     * pointer — never realloc. */
    tex_emissive = (int *)calloc(opt->texture_count ? opt->texture_count : 1,
                                 sizeof *tex_emissive);
    size_t n_emissive = 0;
    if (!tex_emissive) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
    if (emissive) {
        for (int32_t i = 0; i < opt->texture_count; ++i) {
            uint8_t *em = decode_texture_emissive_rgba8(opt, &opt->textures[i],
                                                        base_shade, OPT_EMISSIVE_SHADE_TOL);
            if (em) { tex_emissive[i] = 1; n_emissive++; free(em); }
        }
    }

    const size_t image_total = (size_t)opt->texture_count + n_emissive;
    build->images   = (cgltf_image   *)calloc(image_total ? image_total : 1, sizeof *build->images);
    build->textures = (cgltf_texture *)calloc(image_total ? image_total : 1, sizeof *build->textures);
    build->materials= (cgltf_material*)calloc(opt->texture_count + 1, sizeof *build->materials);
    document->image_pixels = (OptGltfOwnedImage *)calloc(
        image_total ? image_total : 1, sizeof *document->image_pixels);
    document->image_pixels_count = image_total;
    if (!build->images || !build->textures || !build->materials || !document->image_pixels) {
        fprintf(stderr, "opt2gltf: oom (textures)\n");
        goto fail;
    }
    size_t emi = (size_t)opt->texture_count;   /* next emissive image/texture slot */
    for (int32_t i = 0; i < opt->texture_count; ++i) {
        const opt_texture_t *t = &opt->textures[i];
        uint8_t *rgba = decode_texture_rgba8(t, base_shade);
        if (rgba) {
            char png_rel[256];
            snprintf(png_rel, sizeof png_rel,
                     "textures/%s_Tex%02d.png", basename, (int)i);
            document->image_pixels[i].rgba = rgba;
            document->image_pixels[i].width = (uint32_t)t->width;
            document->image_pixels[i].height = (uint32_t)t->height;
            build->images[i].name = gb_keep_string(build,
                xprintf_dup("Tex%02d", (int)i));
            build->images[i].uri  = gb_keep_string(build, xstrdup(png_rel));
            build->images[i].mime_type = "image/png";
        } else {
            build->images[i].name = gb_keep_string(build,
                xprintf_dup("Tex%02d (missing pixels)", (int)i));
        }
        build->textures[i].image   = &build->images[i];
        build->textures[i].sampler = &build->sampler;
        build->textures[i].name    = build->images[i].name;

        /* PBR material: baseColorTexture only; metallic 0, roughness
         * 1 by default — author can refine. */
        cgltf_material *mat = &build->materials[i];
        mat->name = gb_keep_string(build,
            xprintf_dup("Mat_Tex%02d", (int)i));
        mat->has_pbr_metallic_roughness = 1;
        mat->pbr_metallic_roughness.base_color_factor[0] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[1] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[2] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
        mat->pbr_metallic_roughness.metallic_factor  = 0.0f;
        mat->pbr_metallic_roughness.roughness_factor = 1.0f;
        mat->pbr_metallic_roughness.base_color_texture.texture = &build->textures[i];
        mat->double_sided = 1;
        /* XWA textures may carry a per-pixel alpha map; blend when present. */
        if (t->alpha) mat->alpha_mode = cgltf_alpha_mode_blend;

        /* Emissive: self-illuminated texels (e.g. lit windows) get a second
         * texture holding their base color on black, wired as emissiveTexture
         * with a unit emissiveFactor. */
        if (tex_emissive[i]) {
            uint8_t *em = decode_texture_emissive_rgba8(
                opt, t, base_shade, OPT_EMISSIVE_SHADE_TOL);
            if (em) {
                char png_rel[256];
                snprintf(png_rel, sizeof png_rel,
                         "textures/%s_Tex%02d_emissive.png", basename, (int)i);
                document->image_pixels[emi].rgba = em;
                document->image_pixels[emi].width = (uint32_t)t->width;
                document->image_pixels[emi].height = (uint32_t)t->height;
                build->images[emi].name = gb_keep_string(build,
                    xprintf_dup("Tex%02d_emissive", (int)i));
                build->images[emi].uri  = gb_keep_string(build, xstrdup(png_rel));
                build->images[emi].mime_type = "image/png";
                build->textures[emi].image   = &build->images[emi];
                build->textures[emi].sampler = &build->sampler;
                build->textures[emi].name    = build->images[emi].name;
                mat->emissive_texture.texture = &build->textures[emi];
                mat->emissive_factor[0] = 1.0f;
                mat->emissive_factor[1] = 1.0f;
                mat->emissive_factor[2] = 1.0f;
                /* Generic Aeron material mode: emissive alpha carries
                 * coverage for a legacy fixed-function sRGB/SRCALPHA pass. */
                mat->extras.data = gb_keep_string(build,
                    xstrdup("{\"aeronEmissiveMode\":\"legacy_srgb_srcalpha\"}"));
                emi++;
            }
        }
    }
    /* One extra material for untextured face groups (texture_index = -1). */
    cgltf_material *untex = &build->materials[opt->texture_count];
    untex->name = gb_keep_string(build, xstrdup("Mat_Untextured"));
    untex->has_pbr_metallic_roughness = 1;
    untex->pbr_metallic_roughness.base_color_factor[0] = 0.6f;
    untex->pbr_metallic_roughness.base_color_factor[1] = 0.6f;
    untex->pbr_metallic_roughness.base_color_factor[2] = 0.6f;
    untex->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
    untex->pbr_metallic_roughness.metallic_factor  = 0.0f;
    untex->pbr_metallic_roughness.roughness_factor = 1.0f;
    untex->double_sided = 1;
    build->materials_count = (size_t)opt->texture_count + 1;
    const size_t untex_idx = (size_t)opt->texture_count;

    build->images_count   = emi;
    build->textures_count = emi;
    free(tex_emissive);
    tex_emissive = NULL;

    /* ---- Pass 1: build per-mesh geometry into BuiltMesh[] -------- */
    built = (BuiltMesh *)calloc(opt->mesh_count ? opt->mesh_count : 1, sizeof *built);
    if (!built) {
        fprintf(stderr, "opt2gltf: oom (meshes)\n");
        goto fail;
    }
    size_t total_meshes = 0;        /* meshes with geometry */
    int    max_state_count = 1;     /* drives KHR_materials_variants */
    uint32_t fixed_zero = 0, fixed_flip = 0;
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
        const opt_mesh_t *m = &opt->meshes[mi];
        if (!built_mesh_from_opt(&built[mi], m, vertex_scale,
                                 regen, smooth_angle_deg, repair_normals)) continue;
        total_meshes++;
        fixed_zero += built[mi].fixed_zero_normals;
        fixed_flip += built[mi].fixed_flipped_normals;
        const opt_lod_t *lod = &m->lods[0];
        for (int32_t g = 0; g < lod->group_count; ++g) {
            int sc = lod->groups[g].state_count;
            if (sc > max_state_count) max_state_count = sc;
        }
    }
    if (fixed_zero || fixed_flip) {
        fprintf(stderr, "opt2gltf: %s: repaired %u zero + %u face-opposed canonical normals\n",
                basename, fixed_zero, fixed_flip);
    }
    if (max_state_count > 4) max_state_count = 4;

    /* ---- Variants (asset-level) ---------------------------------- */
    if (max_state_count > 1) {
        build->variants = (cgltf_material_variant *)
            calloc(max_state_count, sizeof *build->variants);
        if (!build->variants) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
        for (int v = 0; v < max_state_count; ++v) {
            build->variants[v].name = gb_keep_string(build,
                xprintf_dup("Variant%d", v));
        }
        build->variants_count = (size_t)max_state_count;
    }

    /* ---- Pass 2: write geometry to .bin + accessors + meshes ----- */
    build->meshes = (cgltf_mesh *)calloc(total_meshes, sizeof *build->meshes);
    if (!build->meshes) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
    build->meshes_count = total_meshes;

    /* Exact pre-sized allocations for every array cgltf indexes by
     * pointer arithmetic at write time. ANY realloc mid-build would
     * dangle the cgltf_* pointers stored in dependent records (e.g.
     * primitive.indices points into accessors[]; attribute.data points
     * into accessors[]; node.mesh points into meshes[]). pre-counting
     * is one extra walk; reallocs are forbidden. */
    size_t total_prims = 0, total_attrs = 0, total_maps = 0;
    size_t total_views = 0, total_accs = 0;
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
        if (built[mi].vertex_count == 0) continue;
        const opt_mesh_t *m = &opt->meshes[mi];
        const opt_lod_t *lod = &m->lods[0];
        /* 3 vertex-attribute streams (pos/nrm/uv): 3 views + 3 accessors. */
        total_views += 3;
        total_accs  += 3;
        for (int32_t g = 0; g < lod->group_count; ++g) {
            if (built[mi].fg_index_count_arr[g] == 0) continue;
            total_prims++;
            total_attrs += 3;       /* POSITION, NORMAL, TEXCOORD_0 */
            total_views += 1;       /* index buffer view */
            total_accs  += 1;       /* index accessor */
            int sc = lod->groups[g].state_count;
            if (sc > 1) total_maps += (sc > 4 ? 4 : sc);
        }
    }
    build->views      = (cgltf_buffer_view *)calloc(total_views ? total_views : 1, sizeof *build->views);
    build->accessors  = (cgltf_accessor    *)calloc(total_accs  ? total_accs  : 1, sizeof *build->accessors);
    build->primitives = (cgltf_primitive *)calloc(total_prims ? total_prims : 1, sizeof *build->primitives);
    build->attributes = (cgltf_attribute *)calloc(total_attrs ? total_attrs : 1, sizeof *build->attributes);
    build->mappings   = (cgltf_material_mapping *)calloc(total_maps ? total_maps : 1, sizeof *build->mappings);
    build->views_cap      = total_views;
    build->accessors_cap  = total_accs;
    build->primitives_cap = total_prims;
    build->attributes_cap = total_attrs;
    build->mappings_cap   = total_maps;
    if (!build->views || !build->accessors || !build->primitives || !build->attributes || !build->mappings) {
        fprintf(stderr, "opt2gltf: oom (geometry arrays)\n");
        goto fail;
    }

    size_t mesh_out = 0;
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
        if (built[mi].vertex_count == 0) continue;
        BuiltMesh *bm = &built[mi];
        const opt_mesh_t *m = &opt->meshes[mi];
        const opt_lod_t *lod = &m->lods[0];
        cgltf_mesh *mesh = &build->meshes[mesh_out++];
        mesh->name = gb_keep_string(build, xprintf_dup("Mesh%02d_%s",
                          (int)mi, opt_mesh_type_name(m->descriptor.mesh_type)));

        /* Vertex stream: positions / normals / uvs. */
        size_t pos_off = bin_append(build, NULL, 0);
        /* compute min/max for POSITION accessor */
        float pmin[3] = { +INFINITY, +INFINITY, +INFINITY };
        float pmax[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (uint32_t i = 0; i < bm->vertex_count; ++i) {
            const float *p = bm->vertices[i].pos;
            for (int k = 0; k < 3; ++k) {
                if (p[k] < pmin[k]) pmin[k] = p[k];
                if (p[k] > pmax[k]) pmax[k] = p[k];
            }
        }
        for (uint32_t i = 0; i < bm->vertex_count; ++i)
            bin_append(build, bm->vertices[i].pos, sizeof bm->vertices[i].pos);
        size_t pos_bytes = bm->vertex_count * sizeof bm->vertices[0].pos;
        cgltf_buffer_view *pos_view = gb_new_view(build, pos_off, pos_bytes,
                                                  cgltf_buffer_view_type_vertices);
        cgltf_accessor *pos_acc = gb_new_accessor(build, pos_view,
            bm->vertex_count, cgltf_type_vec3, cgltf_component_type_r_32f);
        pos_acc->has_min = 1; pos_acc->has_max = 1;
        memcpy(pos_acc->min, pmin, sizeof pmin);
        memcpy(pos_acc->max, pmax, sizeof pmax);

        size_t nrm_off = bin_append(build, NULL, 0);
        for (uint32_t i = 0; i < bm->vertex_count; ++i)
            bin_append(build, bm->vertices[i].nrm, sizeof bm->vertices[i].nrm);
        size_t nrm_bytes = bm->vertex_count * sizeof bm->vertices[0].nrm;
        cgltf_buffer_view *nrm_view = gb_new_view(build, nrm_off, nrm_bytes,
                                                  cgltf_buffer_view_type_vertices);
        cgltf_accessor *nrm_acc = gb_new_accessor(build, nrm_view,
            bm->vertex_count, cgltf_type_vec3, cgltf_component_type_r_32f);

        size_t uv_off = bin_append(build, NULL, 0);
        for (uint32_t i = 0; i < bm->vertex_count; ++i)
            bin_append(build, bm->vertices[i].uv, sizeof bm->vertices[i].uv);
        size_t uv_bytes = bm->vertex_count * sizeof bm->vertices[0].uv;
        cgltf_buffer_view *uv_view = gb_new_view(build, uv_off, uv_bytes,
                                                  cgltf_buffer_view_type_vertices);
        cgltf_accessor *uv_acc = gb_new_accessor(build, uv_view,
            bm->vertex_count, cgltf_type_vec2, cgltf_component_type_r_32f);

        /* One primitive per face group with indices. */
        size_t mesh_prim_count = 0;
        cgltf_primitive *mesh_prims_start = build->primitives + build->primitives_count;
        for (int32_t g = 0; g < lod->group_count; ++g) {
            uint32_t ic = bm->fg_index_count_arr[g];
            if (ic == 0) continue;
            const opt_face_group_t *grp = &lod->groups[g];

            /* Indices for this face group. */
            size_t idx_off = bin_append(build,
                bm->indices + bm->fg_index_start[g], ic * sizeof(uint16_t));
            cgltf_buffer_view *idx_view = gb_new_view(build, idx_off,
                ic * sizeof(uint16_t), cgltf_buffer_view_type_indices);
            cgltf_accessor *idx_acc = gb_new_accessor(build, idx_view, ic,
                cgltf_type_scalar, cgltf_component_type_r_16u);

            /* Build the primitive. */
            cgltf_primitive *prim = (cgltf_primitive *)push_array(
                (void **)&build->primitives, &build->primitives_count,
                &build->primitives_cap, sizeof *prim, 1);
            prim->type = cgltf_primitive_type_triangles;
            prim->indices = idx_acc;

            /* Attributes: POSITION, NORMAL, TEXCOORD_0. */
            cgltf_attribute *attrs = (cgltf_attribute *)push_array(
                (void **)&build->attributes, &build->attributes_count,
                &build->attributes_cap, sizeof *attrs, 3);
            attrs[0].name = gb_keep_string(build, xstrdup("POSITION"));
            attrs[0].type = cgltf_attribute_type_position;
            attrs[0].index = 0;
            attrs[0].data  = pos_acc;
            attrs[1].name = gb_keep_string(build, xstrdup("NORMAL"));
            attrs[1].type = cgltf_attribute_type_normal;
            attrs[1].index = 0;
            attrs[1].data  = nrm_acc;
            attrs[2].name = gb_keep_string(build, xstrdup("TEXCOORD_0"));
            attrs[2].type = cgltf_attribute_type_texcoord;
            attrs[2].index = 0;
            attrs[2].data  = uv_acc;
            prim->attributes       = attrs;
            prim->attributes_count = 3;

            /* Default material = state 0 (or untextured fallback). */
            int t0 = grp->texture_index;
            prim->material = (t0 >= 0 && t0 < opt->texture_count)
                              ? &build->materials[t0]
                              : &build->materials[untex_idx];

            /* Variant mappings — only if this face group has > 1 state. */
            int sc = grp->state_count;
            if (sc > 1 && grp->state_textures) {
                if (sc > 4) sc = 4;
                cgltf_material_mapping *maps = (cgltf_material_mapping *)
                    push_array((void **)&build->mappings, &build->mappings_count,
                               &build->mappings_cap, sizeof *maps, sc);
                for (int v = 0; v < sc; ++v) {
                    int ti = grp->state_textures[v];
                    cgltf_material *m_ref = (ti >= 0 && ti < opt->texture_count)
                                             ? &build->materials[ti]
                                             : &build->materials[untex_idx];
                    maps[v].variant  = (cgltf_size)v;
                    maps[v].material = m_ref;
                }
                prim->mappings = maps;
                prim->mappings_count = (cgltf_size)sc;
            }
            mesh_prim_count++;
        }
        mesh->primitives = mesh_prims_start;
        mesh->primitives_count = mesh_prim_count;
    }

    /* ---- Nodes: one root + one per mesh (with hardpoint children) -- */
    /* Count nodes: 1 root + (visible meshes) + (hardpoint + engine-glow
     * child nodes across all visible meshes). */
    size_t total_hardpoints = 0;
    size_t total_glows = 0;
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
        if (built[mi].vertex_count == 0) continue;
        total_hardpoints += opt->meshes[mi].hardpoint_count;
        total_glows      += opt->meshes[mi].engine_glow_count;
    }
    size_t total_nodes = 1 + total_meshes + total_hardpoints + total_glows;
    build->nodes = (cgltf_node *)calloc(total_nodes, sizeof *build->nodes);
    if (!build->nodes) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
    build->nodes_count = total_nodes;

    /* Layout in build->nodes[]:
     *   [0]                   = scene root
     *   [1 .. total_meshes]   = per-mesh nodes
     *   [1+total_meshes ..]   = hardpoint + engine-glow child nodes,
     *                           contiguous per parent mesh */
    cgltf_node *root_node = &build->nodes[0];
    root_node->name = gb_keep_string(build,
        xprintf_dup("OPT_%s", basename));
    /* Blender-friendly display scale on the root node — display only.
     * OPT files carry vertex coords in the engine's Q16.16 raw units
     * (TIE Fighter ±146, CRUSA ±3000+); without this scale Blender shows
     * the asset hundreds of metres wide. 0.02 (= 1/50) puts a TIE
     * Fighter at ~5.8 m and an X-wing at ~6.5 m — small but
     * inspectable, with capital ships still fitting in a single
     * viewport.
     *
     * The renderer-side loader IGNORES this node scale: it reads vertex
     * positions straight from the accessors (raw OPT units) and never
     * composes the node TRS, then applies 1/65536 in craft_to_world at
     * draw time — the same factor the classic OPT pipeline uses. The two
     * are independent: changing 0.02 only affects the Blender view, not
     * the in-engine size. */
    root_node->has_scale = 1;
    root_node->scale[0] = 0.02f;
    root_node->scale[1] = 0.02f;
    root_node->scale[2] = 0.02f;

    cgltf_node **root_children = (cgltf_node **)calloc(total_meshes,
                                                       sizeof *root_children);
    if (!root_children) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
    root_node->children = root_children;
    root_node->children_count = total_meshes;

    cgltf_node *next_mesh_node  = &build->nodes[1];
    cgltf_node *next_child_node = &build->nodes[1 + total_meshes];

    mesh_out = 0;
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
        if (built[mi].vertex_count == 0) continue;
        const opt_mesh_t *m = &opt->meshes[mi];
        cgltf_node *node = next_mesh_node++;
        root_children[mesh_out] = node;
        node->parent = root_node;
        node->mesh = &build->meshes[mesh_out];
        /* Alias the mesh's already-kept name; do NOT re-push into
         * owned_strings or the cleanup loop will double-free. */
        node->name = build->meshes[mesh_out].name;

        /* Per-mesh extras: meshType + symbolic name + (optional)
         * rotation block. Hardpoints are emitted as child nodes for
         * better tool interop (Blender renders them as empties). */
        const char *type_name = opt_mesh_type_name(m->descriptor.mesh_type);
        char extras[1024];
        size_t off = 0;
        /* tieMeshIndex = the original OPT mesh array index. This is the
         * stable identity used by the bundle's mesh_map (engine_mesh_idx
         * → opt_mesh_idx) and by anything else that needs to address a
         * specific mesh in the original file. Glb-tool reordering of
         * scene nodes is allowed — the renderer keys off this extras
         * field, not the node ordering. */
        off += snprintf(extras + off, sizeof extras - off,
            "{\"tieMeshIndex\":%d,\"tieMeshType\":%d,\"tieMeshTypeName\":\"%s\"",
            (int)mi, (int)m->descriptor.mesh_type, type_name);

        /* Emit for EVERY mesh carrying the node — the default axis
         * frame is NOT "static": XWA rotates default-frame meshes
         * about their pivot whenever their runtime angle is nonzero
         * (hangar droid antennas spin about the default vertical
         * axis). The identity check only classified axes, dropping
         * exactly those. */
        if (m->has_rotation_scale) {
            float pivot[3], rax[3], dax[3], uax[3];
            swap_axis_v3(&m->rotation_scale.pivot,          pivot);
            /* pivot is a position — scale it; axes are directions — don't. */
            pivot[0] *= vertex_scale; pivot[1] *= vertex_scale; pivot[2] *= vertex_scale;
            swap_axis_v3(&m->rotation_scale.rotation_axis,  rax);
            swap_axis_v3(&m->rotation_scale.direction_axis, dax);
            swap_axis_v3(&m->rotation_scale.up_axis,        uax);
            /* No "animated" flag: whether a mesh rotates is RUNTIME
             * state (XWA gates on its per-mesh angle and rotates plain
             * MainHull meshes); rotary classification is derivable
             * from tieMeshType. */
            off += snprintf(extras + off, sizeof extras - off,
                ",\"tieRotation\":{"
                "\"pivot\":[%g,%g,%g],"
                "\"rotationAxis\":[%g,%g,%g],"
                "\"directionAxis\":[%g,%g,%g],"
                "\"upAxis\":[%g,%g,%g]}",
                pivot[0], pivot[1], pivot[2],
                rax[0],   rax[1],   rax[2],
                dax[0],   dax[1],   dax[2],
                uax[0],   uax[1],   uax[2]);
        }
        snprintf(extras + off, sizeof extras - off, "}");
        node->extras.data = gb_keep_string(build, xstrdup(extras));

        /* Child nodes: hardpoints first, then engine glows (both emitted
         * as empties so Blender and other tools surface them). */
        int child_total = m->hardpoint_count + m->engine_glow_count;
        if (child_total > 0) {
            cgltf_node **children = (cgltf_node **)calloc(child_total,
                                                          sizeof *children);
            if (!children) { fprintf(stderr, "opt2gltf: oom\n"); goto fail; }
            int ci = 0;
            for (int h = 0; h < m->hardpoint_count; ++h) {
                const opt_hardpoint_t *hp = &m->hardpoints[h];
                cgltf_node *hpn = next_child_node++;
                hpn->parent = node;
                const char *hp_name = opt_hardpoint_type_name(hp->type);
                hpn->name = gb_keep_string(build,
                    xprintf_dup("hp_%s", hp_name));
                float pos[3];
                swap_axis_v3(&hp->pos, pos);
                hpn->has_translation = 1;
                hpn->translation[0] = pos[0] * vertex_scale;
                hpn->translation[1] = pos[1] * vertex_scale;
                hpn->translation[2] = pos[2] * vertex_scale;
                char hpe[512];
                snprintf(hpe, sizeof hpe,
                    "{\"tieHardpoint\":{\"type\":%d,\"typeName\":\"%s\"}}",
                    (int)hp->type, hp_name);
                hpn->extras.data = gb_keep_string(build, xstrdup(hpe));
                children[ci++] = hpn;
            }
            for (int e = 0; e < m->engine_glow_count; ++e) {
                const opt_engine_glow_t *eg = &m->engine_glows[e];
                cgltf_node *egn = next_child_node++;
                egn->parent = node;
                egn->name = gb_keep_string(build, xprintf_dup("engine_glow_%d", e));
                float pos[3], look[3], up[3], right[3];
                swap_axis_v3(&eg->position,   pos);
                swap_axis_v3(&eg->look_axis,  look);
                swap_axis_v3(&eg->up_axis,    up);
                swap_axis_v3(&eg->right_axis, right);
                egn->has_translation = 1;
                egn->translation[0] = pos[0] * vertex_scale;
                egn->translation[1] = pos[1] * vertex_scale;
                egn->translation[2] = pos[2] * vertex_scale;
                char ege[768];
                snprintf(ege, sizeof ege,
                    "{\"tieEngineGlow\":{\"disabled\":%s,"
                    "\"coreColor\":\"#%08X\",\"outerColor\":\"#%08X\","
                    "\"dimensions\":[%g,%g,%g],"
                    "\"lookAxis\":[%g,%g,%g],\"upAxis\":[%g,%g,%g],"
                    "\"rightAxis\":[%g,%g,%g]}}",
                    eg->is_disabled ? "true" : "false",
                    eg->core_color, eg->outer_color,
                    eg->dimensions.x, eg->dimensions.y, eg->dimensions.z,
                    look[0], look[1], look[2],
                    up[0], up[1], up[2],
                    right[0], right[1], right[2]);
                egn->extras.data = gb_keep_string(build, xstrdup(ege));
                children[ci++] = egn;
            }
            node->children = children;
            node->children_count = child_total;
        }

        mesh_out++;
    }

    /* ---- Scene + asset ------------------------------------------- */
    document->scene.name = gb_keep_string(build, xstrdup("Scene"));
    document->scene.nodes_count = 1;
    /* The scene has exactly one root: build->nodes[0]. */
    cgltf_node **scene_roots = (cgltf_node **)calloc(1, sizeof *scene_roots);
    if (!scene_roots) goto fail;
    scene_roots[0] = root_node;
    document->scene.nodes = scene_roots;
    document->scene_roots = scene_roots;

    /* Buffer size = total .bin bytes. */
    build->buffer.size = build->bin_size;
    build->buffer.data = build->bin;

    /* Final cgltf_data wiring. */
    document->data.asset.version   = "2.0";
    document->data.asset.generator = "opt2gltf";

    document->data.buffers              = &build->buffer;
    document->data.buffers_count        = 1;
    document->data.buffer_views         = build->views;
    document->data.buffer_views_count   = build->views_count;
    document->data.accessors            = build->accessors;
    document->data.accessors_count      = build->accessors_count;
    document->data.images               = build->images;
    document->data.images_count         = build->images_count;
    document->data.textures             = build->textures;
    document->data.textures_count       = build->textures_count;
    document->data.samplers             = &build->sampler;
    document->data.samplers_count       = 1;
    document->data.materials            = build->materials;
    document->data.materials_count      = build->materials_count;
    document->data.meshes               = build->meshes;
    document->data.meshes_count         = build->meshes_count;
    document->data.nodes                = build->nodes;
    document->data.nodes_count          = build->nodes_count;
    document->data.scenes               = &document->scene;
    document->data.scenes_count         = 1;
    document->data.scene                = &document->scene;
    document->data.variants             = build->variants;
    document->data.variants_count       = build->variants_count;

    /* Declare KHR_materials_variants in extensionsUsed if used. */
    const char *ext_variants = "KHR_materials_variants";
    if (build->variants_count > 0) {
        document->data.extensions_used = (char **)calloc(1, sizeof(char *));
        if (document->data.extensions_used) {
            document->data.extensions_used[0] = (char *)ext_variants;
            document->data.extensions_used_count = 1;
        }
    }

    /* The geometry build is temporary; the cgltf accessors now reference
     * copies in build->bin. */
    for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
        built_mesh_free(&built[mi]);
    free(built);
    built = NULL;

    *out_document = document;
    return true;

fail:
    free(tex_emissive);
    if (built) {
        for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
            built_mesh_free(&built[mi]);
        free(built);
    }
    if (error && !error->msg[0])
        snprintf(error->msg, sizeof error->msg, "out of memory while converting %s", basename);
    OptGltf_Free(document);
    return false;
}

cgltf_data *OptGltf_Data(OptGltfDocument *document)
{
    return document ? &document->data : NULL;
}

bool OptGltf_ImageView(const OptGltfDocument *document,
                       const cgltf_image *image,
                       OptGltfImageView *out_view)
{
    if (!document || !image || !out_view || !document->gb.images) return false;
    const ptrdiff_t index = image - document->gb.images;
    if (index < 0 || (size_t)index >= document->image_pixels_count) return false;
    const OptGltfOwnedImage *owned = &document->image_pixels[index];
    if (!owned->rgba || owned->width == 0 || owned->height == 0) return false;
    out_view->rgba = owned->rgba;
    out_view->width = owned->width;
    out_view->height = owned->height;
    return true;
}

void OptGltf_Free(OptGltfDocument *document)
{
    if (!document) return;
    GltfBuild *gb = &document->gb;

    /* The per-node children loop already covers both root_children
     * (gb.nodes[0].children) and per-mesh hp_children — both are
     * stored as node->children. Freeing root_children here would be
     * a double-free. */
    for (size_t i = 0; i < gb->nodes_count; ++i) {
        free(gb->nodes[i].children);
    }
    free(document->scene_roots);
    for (size_t i = 0; i < document->image_pixels_count; ++i)
        free(document->image_pixels[i].rgba);
    free(document->image_pixels);
    free(gb->images);
    free(gb->textures);
    free(gb->materials);
    free(gb->meshes);
    free(gb->primitives);
    free(gb->attributes);
    free(gb->mappings);
    free(gb->variants);
    free(gb->views);
    free(gb->accessors);
    free(gb->nodes);
    free(gb->buffer.data ? gb->buffer.data : gb->bin);
    free(document->data.extensions_used);

    for (size_t i = 0; i < gb->owned_strings_count; ++i)
        free(gb->owned_strings[i]);
    free(gb->owned_strings);
    free(document);
}

bool OptGltf_WriteFiles(const OptGltfDocument *document,
                        const char *out_dir,
                        const char *basename)
{
    if (!document || !out_dir || !basename) return false;
    if (ensure_dir(out_dir) != 0) return false;
    char tex_dir[1024];
    path_join(tex_dir, sizeof tex_dir, out_dir, "textures");
    if (ensure_dir(tex_dir) != 0) return false;

    for (size_t i = 0; i < document->data.images_count; ++i) {
        const cgltf_image *image = &document->data.images[i];
        const OptGltfOwnedImage *pixels = &document->image_pixels[i];
        if (!image->uri || !pixels->rgba) continue;
        char png_path[1024];
        path_join(png_path, sizeof png_path, out_dir, image->uri);
        if (!stbi_write_png(png_path, (int)pixels->width, (int)pixels->height,
                            4, pixels->rgba, (int)pixels->width * 4))
            return false;
    }

    char gltf_path[1024], bin_path[1024], filename[256];
    snprintf(filename, sizeof filename, "%s.gltf", basename);
    path_join(gltf_path, sizeof gltf_path, out_dir, filename);
    snprintf(filename, sizeof filename, "%s.bin", basename);
    path_join(bin_path, sizeof bin_path, out_dir, filename);

    cgltf_options opts = {0};
    if (cgltf_write_file(&opts, gltf_path, &document->data) != cgltf_result_success)
        return false;
    FILE *fp = fopen(bin_path, "wb");
    if (!fp) return false;
    const size_t wrote = fwrite(document->gb.buffer.data, 1, document->gb.bin_size, fp);
    const int close_result = fclose(fp);
    if (wrote != document->gb.bin_size || close_result != 0) return false;

    fprintf(stdout,
        "%s -> %s\n"
        "  meshes=%zu primitives=%zu vertices=(per-mesh) variants=%zu\n"
        "  textures=%zu materials=%zu bin=%zu bytes\n",
        basename, gltf_path,
        document->gb.meshes_count, document->gb.primitives_count,
        document->gb.variants_count, document->gb.textures_count,
        document->gb.materials_count, document->gb.bin_size);
    return true;
}

bool opt2gltf_convert(const opt_file_t *opt,
                      const char *out_dir,
                      const char *basename,
                      float vertex_scale,
                      float smooth_angle_deg,
                      bool repair_normals,
                      bool emissive)
{
    const OptGltfBuildOptions options = {
        .vertex_scale = vertex_scale,
        .smooth_angle_degrees = smooth_angle_deg,
        .repair_normals = repair_normals,
        .emissive = emissive,
    };
    OptGltfDocument *document = NULL;
    opt_error_t error = {{0}};
    if (!OptGltf_BuildMemory(opt, basename, &options, &document, &error)) {
        if (error.msg[0]) fprintf(stderr, "opt2gltf: %s\n", error.msg);
        return false;
    }
    const bool succeeded = OptGltf_WriteFiles(document, out_dir, basename);
    OptGltf_Free(document);
    return succeeded;
}
