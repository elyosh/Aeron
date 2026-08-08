/*
 * flight_gltf_mesh — cooked-.glb loader for OPT-derived ship assets.
 *
 * Consumes the output of `aeron_gltf_cook` (tools/gltf_cook/) which packs
 * artist-authored .gltf assets into a single .glb with four BC7/BC5 KTX2
 * channel atlases (KHR_texture_basisu) and per-material UV transforms
 * (KHR_texture_transform). At runtime the loader walks the cgltf
 * graph and:
 *   - lifts each atlas KTX2 blob out of the GLB BIN chunk and copies
 *     it into a AeronGltfChannelKtx2-owned buffer the SDL_GPU side
 *     decodes via ktx2_open_mem at upload time
 *   - reads per-material PBR factors + per-binding KHR_texture_transform
 *     into AeronGltfMaterial.uv_xform[ch]
 *   - decodes per-primitive vertex / index / variant tables into one
 *     merged ship-level buffer the renderer issues a single indexed
 *     draw against
 *   - flattens hardpoint child nodes (`hp_*` with tieHardpoint extras)
 *     into a per-ship list for the AI / weapon-spawn paths
 *
 * PNG decoding, atlas packing, and mip generation happen offline in
 * aeron_gltf_cook.
 *
 * Diagnostics flow through SDL_Log (host-side abstraction provides
 * the symbol). Errors return false; partially-built ships are freed
 * before returning.
 */

#include "aeron/scene/gltf_mesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "cgltf.h"

/* Host-provided log (SDL3 via SDL_Log; standalone tools stub it). */
extern void SDL_Log(const char *fmt, ...);

/* ===== Coordinate-frame swap =======================================
 *
 * aeron_gltf_cook preserves opt2gltf's coord swap. opt2gltf emits OPT (+Z up,
 * -Y forward) in glTF convention (+Y up, -Z forward) by swapping Y/Z;
 * we undo that swap so vertices land in the renderer's native frame.
 * Y<->Z is an involution. */
static inline void swap_yz3(const float in[3], float out[3])
{
    out[0] = in[0];
    out[1] = in[2];
    out[2] = in[1];
}

/* Same swap for the 4-component tangent: xyz swap, sign-of-bitangent
 * (w) passes through unchanged. */
static inline void swap_yz4_tangent(const float in[4], float out[4])
{
    out[0] = in[0];
    out[1] = in[2];
    out[2] = in[1];
    out[3] = in[3];
}

/* ===== Tiny JSON extras parser =====================================
 *
 * cgltf gives us `extras.data` as a raw JSON string; we parse the
 * specific shape opt2gltf emits without pulling in a full JSON lib.
 * Caller passes a key (e.g. "tieMeshIndex") and a typed extractor.
 * No nesting recovery — we walk the string char-by-char looking for
 * "key":<value> and parse the value type assumed by the caller.
 *
 * Safe against well-formed input from our own tool. Defends against
 * the obvious failure modes (missing key, truncated value) but won't
 * cope with adversarial JSON. */

static const char *json_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    const size_t klen = strlen(key);
    for (const char *p = json; *p; p++) {
        if (*p != '"') continue;
        const char *kstart = p + 1;
        if (strncmp(kstart, key, klen) != 0) continue;
        if (kstart[klen] != '"') continue;
        const char *q = kstart + klen + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        return q;
    }
    return NULL;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    const char *v = json_find_key(json, key);
    if (!v) return false;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return false;
    *out = (int)n;
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    const char *v = json_find_key(json, key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_string_equals(const char *json, const char *key,
                               const char *expected)
{
    const char *v = json_find_key(json, key);
    if (!v || *v != '"' || !expected) return false;
    const char *end = strchr(v + 1, '"');
    if (!end) return false;
    const size_t len = strlen(expected);
    return (size_t)(end - (v + 1)) == len && memcmp(v + 1, expected, len) == 0;
}

/* Parse a fixed-length float array of the form [a, b, c, ...] */
static bool json_get_vec(const char *json, const char *key,
                         int len, float *out)
{
    const char *v = json_find_key(json, key);
    if (!v || *v != '[') return false;
    v++;
    for (int i = 0; i < len; i++) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        char *end = NULL;
        out[i] = strtof(v, &end);
        if (end == v) return false;
        v = end;
    }
    return true;
}

/* ===== cgltf accessor decoders ===================================== */

static void decode_vec3(const cgltf_accessor *a, float *out, uint32_t n)
{
    if (!a || a->count != n) return;
    for (uint32_t i = 0; i < n; i++)
        cgltf_accessor_read_float(a, i, out + i * 3, 3);
}

static void decode_vec2(const cgltf_accessor *a, float *out, uint32_t n)
{
    if (!a || a->count != n) return;
    for (uint32_t i = 0; i < n; i++)
        cgltf_accessor_read_float(a, i, out + i * 2, 2);
}

static void decode_vec4(const cgltf_accessor *a, float *out, uint32_t n)
{
    if (!a || a->count != n) return;
    for (uint32_t i = 0; i < n; i++)
        cgltf_accessor_read_float(a, i, out + i * 4, 4);
}

/* Find an attribute accessor by glTF attribute type. */
static const cgltf_accessor *prim_attr(const cgltf_primitive *p,
                                       cgltf_attribute_type t)
{
    for (cgltf_size i = 0; i < p->attributes_count; i++) {
        if (p->attributes[i].type == t && p->attributes[i].index == 0)
            return p->attributes[i].data;
    }
    return NULL;
}

/* ===== Channel KTX2 extraction =====================================
 *
 * aeron_gltf_cook names each atlas image `atlas_<channel>` with mimeType
 * `image/ktx2` and stores the KTX2 bytes in a buffer_view. We match by
 * name and copy the bytes out of the GLB BIN chunk into a
 * AeronGltfChannelKtx2-owned buffer so the SDL_GPU side has a stable
 * pointer after cgltf_free.
 *
 * Returns NULL when no image carries the expected name; caller decides
 * whether that's fatal. */
static const cgltf_image *find_atlas_image(const cgltf_data *data,
                                           const char *atlas_name)
{
    for (cgltf_size i = 0; i < data->images_count; i++) {
        const cgltf_image *im = &data->images[i];
        if (im->name && strcmp(im->name, atlas_name) == 0)
            return im;
    }
    return NULL;
}

static bool copy_channel_ktx2(const cgltf_data *data,
                              const char *atlas_name,
                              AeronGltfChannelKtx2 *out)
{
    const cgltf_image *im = find_atlas_image(data, atlas_name);
    if (!im) {
        SDL_Log("[flight_gltf] missing atlas image '%s'", atlas_name);
        return false;
    }
    if (!im->buffer_view || !im->buffer_view->buffer ||
        !im->buffer_view->buffer->data) {
        SDL_Log("[flight_gltf] atlas '%s' has no buffer_view data",
                atlas_name);
        return false;
    }
    const uint8_t *src = (const uint8_t *)im->buffer_view->buffer->data
                       + im->buffer_view->offset;
    size_t size = im->buffer_view->size;
    uint8_t *copy = (uint8_t *)malloc(size);
    if (!copy) return false;
    memcpy(copy, src, size);
    out->data = copy;
    out->size = size;
    return true;
}

/* ===== Per-material read ============================================
 *
 * Pulls factors + per-channel UV transform out of cgltf_material.
 * Channel ordering matches AERON_GLTF_CHANNEL_* — keep in sync with
 * the cooker and the FS. */
static const cgltf_texture_view *material_channel_view(
    const cgltf_material *m, int channel)
{
    switch (channel) {
    case AERON_GLTF_CHANNEL_BASE_COLOR:
        return m->has_pbr_metallic_roughness
            ? &m->pbr_metallic_roughness.base_color_texture : NULL;
    case AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS:
        return m->has_pbr_metallic_roughness
            ? &m->pbr_metallic_roughness.metallic_roughness_texture : NULL;
    case AERON_GLTF_CHANNEL_NORMAL:    return &m->normal_texture;
    case AERON_GLTF_CHANNEL_EMISSIVE:  return &m->emissive_texture;
    default:                            return NULL;
    }
}

static bool read_material(const cgltf_material *m, AeronGltfMaterial *out)
{
    /* Factor defaults per glTF spec. */
    out->base_color_factor[0] = 1.0f;
    out->base_color_factor[1] = 1.0f;
    out->base_color_factor[2] = 1.0f;
    out->base_color_factor[3] = 1.0f;
    out->emissive_factor[0]   = 0.0f;
    out->emissive_factor[1]   = 0.0f;
    out->emissive_factor[2]   = 0.0f;
    out->emissive_strength    = 1.0f;
    out->metallic_factor      = 0.0f;
    out->roughness_factor     = 1.0f;
    out->double_sided         = 0u;
    out->alpha_mode           = AERON_GLTF_ALPHA_OPAQUE;
    out->alpha_cutoff         = 0.5f;
    out->emissive_mode        = AERON_GLTF_EMISSIVE_ADDITIVE;
    /* uv_xform sentinel zero = "channel not authored" — FS falls back
     * to factor. Overwritten below for channels that do bind. */
    memset(out->uv_xform, 0, sizeof out->uv_xform);

    if (!m) return true;

    out->double_sided = m->double_sided ? 1u : 0u;
    switch (m->alpha_mode) {
    case cgltf_alpha_mode_opaque:
        out->alpha_mode = AERON_GLTF_ALPHA_OPAQUE;
        break;
    case cgltf_alpha_mode_mask:
        if (!isfinite(m->alpha_cutoff) || m->alpha_cutoff < 0.0f ||
            m->alpha_cutoff > 1.0f) return false;
        out->alpha_mode = AERON_GLTF_ALPHA_MASK;
        out->alpha_cutoff = m->alpha_cutoff;
        break;
    case cgltf_alpha_mode_blend:
        out->alpha_mode = AERON_GLTF_ALPHA_BLEND;
        break;
    default:
        return false;
    }
    if (json_string_equals(m->extras.data, "aeronEmissiveMode",
                           "legacy_srgb_srcalpha")) {
        out->emissive_mode = AERON_GLTF_EMISSIVE_LEGACY_SRGB_SRCALPHA;
    }

    if (m->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *pbr = &m->pbr_metallic_roughness;
        memcpy(out->base_color_factor, pbr->base_color_factor,
               sizeof out->base_color_factor);
        out->metallic_factor  = pbr->metallic_factor;
        out->roughness_factor = pbr->roughness_factor;
    }
    out->emissive_factor[0] = m->emissive_factor[0];
    out->emissive_factor[1] = m->emissive_factor[1];
    out->emissive_factor[2] = m->emissive_factor[2];
    if (m->has_emissive_strength)
        out->emissive_strength = m->emissive_strength.emissive_strength;

    for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
        const cgltf_texture_view *tv = material_channel_view(m, c);
        if (!tv || !tv->texture || !tv->has_transform) continue;
        out->uv_xform[c][0] = tv->transform.offset[0];
        out->uv_xform[c][1] = tv->transform.offset[1];
        out->uv_xform[c][2] = tv->transform.scale [0];
        out->uv_xform[c][3] = tv->transform.scale [1];
    }
    return true;
}

static AeronGltfAlphaMode material_alpha_mode(const AeronGltfModel *model,
                                               uint32_t material)
{
    if (material == AERON_GLTF_NO_MATERIAL) return AERON_GLTF_ALPHA_OPAQUE;
    if (!model || material >= model->material_count)
        return (AeronGltfAlphaMode)-1;
    return model->materials[material].alpha_mode;
}

/* ===== Vertex append ===============================================
 *
 * Append decoded vertices + indices for one glTF primitive into the
 * ship's merged buffers. Index values are biased to reference the
 * primitive's range relative to its global vertex_offset so the
 * renderer issues a single indexed draw across all primitives.
 *
 * Also bakes per-vertex `mesh_index` (= node's opt_mesh_index) and
 * `prim_id` (= global prim slot). */
static bool append_primitive_vertices(
    const cgltf_primitive *p,
    uint16_t opt_mesh_index,
    uint32_t prim_id,
    AeronGltfVertex *verts, uint16_t *indices,
    uint32_t *voff_io, uint32_t *ioff_io)
{
    const cgltf_accessor *pos = prim_attr(p, cgltf_attribute_type_position);
    const cgltf_accessor *nrm = prim_attr(p, cgltf_attribute_type_normal);
    const cgltf_accessor *uv  = prim_attr(p, cgltf_attribute_type_texcoord);
    const cgltf_accessor *tan = prim_attr(p, cgltf_attribute_type_tangent);
    const cgltf_accessor *idx = p->indices;
    if (!pos || !idx) return true;            /* skipped; not fatal */

    uint32_t vcount = (uint32_t)pos->count;
    uint32_t icount = (uint32_t)idx->count;
    uint32_t voff   = *voff_io;
    uint32_t ioff   = *ioff_io;

    float *positions = (float *)malloc((size_t)vcount * 3 * sizeof(float));
    float *normals   = (float *)calloc(vcount, 3 * sizeof(float));
    float *uvs       = (float *)calloc(vcount, 2 * sizeof(float));
    float *tangents  = (float *)calloc(vcount, 4 * sizeof(float));
    if (!positions || !normals || !uvs || !tangents) {
        free(positions); free(normals); free(uvs); free(tangents);
        return false;
    }
    decode_vec3(pos, positions, vcount);
    if (nrm) decode_vec3(nrm, normals, vcount);
    else     for (uint32_t i = 0; i < vcount; i++) normals[i*3+2] = 1.0f;
    if (uv)  decode_vec2(uv, uvs, vcount);
    if (tan) decode_vec4(tan, tangents, vcount);
    else     for (uint32_t i = 0; i < vcount; i++) tangents[i*4+3] = 1.0f;

    /* Interleave into AeronGltfVertex with coord swap. Positions stay
     * in raw OPT units — the runtime scales them via craft_to_world's
     * 1/65536 factor, matching the OPT pipeline. */
    for (uint32_t i = 0; i < vcount; i++) {
        AeronGltfVertex *v = &verts[voff + i];
        swap_yz3(&positions[i*3], v->pos);
        swap_yz3(&normals[i*3],   v->normal);
        swap_yz4_tangent(&tangents[i*4], v->tangent);
        v->uv[0]       = uvs[i*2 + 0];
        v->uv[1]       = uvs[i*2 + 1];
        v->mesh_index  = (float)opt_mesh_index;
        v->prim_id     = prim_id;
    }
    free(positions); free(normals); free(uvs); free(tangents);

    /* Indices — biased to the primitive's range inside the ship's
     * merged buffer. */
    uint16_t *dst_idx = indices + ioff;
    for (uint32_t i = 0; i < icount; i++) {
        cgltf_size raw = cgltf_accessor_read_index(idx, i);
        uint32_t   adj = (uint32_t)raw + voff;
        dst_idx[i] = (adj < 0xFFFFu) ? (uint16_t)adj : 0xFFFFu;
    }

    *voff_io = voff + vcount;
    *ioff_io = ioff + icount;
    return true;
}

/* ===== Hardpoint collection ========================================= */

static bool collect_hardpoints(const cgltf_node *mesh_node,
                               AeronGltfHardpoint **list,
                               uint32_t *count, uint32_t *cap)
{
    for (cgltf_size i = 0; i < mesh_node->children_count; i++) {
        const cgltf_node *ch = mesh_node->children[i];
        if (ch->mesh || !ch->extras.data ||
            !strstr(ch->extras.data, "tieHardpoint"))
            continue;
        if (*count == *cap) {
            uint32_t new_cap = *cap ? *cap * 2u : 8u;
            AeronGltfHardpoint *grown = (AeronGltfHardpoint *)
                realloc(*list, new_cap * sizeof(AeronGltfHardpoint));
            if (!grown) return false;
            *list = grown;
            *cap  = new_cap;
        }
        AeronGltfHardpoint *hp = &(*list)[(*count)++];
        memset(hp, 0, sizeof *hp);
        float pos_gltf[3] = {0, 0, 0};
        if (ch->has_translation) {
            pos_gltf[0] = ch->translation[0];
            pos_gltf[1] = ch->translation[1];
            pos_gltf[2] = ch->translation[2];
        }
        swap_yz3(pos_gltf, hp->position);
        int t;
        if (json_get_int(ch->extras.data, "type", &t))
            hp->type = (uint8_t)t;
    }
    return true;
}

/* ===== Engine glow collection ======================================= */

/* "#AARRGGBB" -> linear-ish 0..1 RGBA floats. */
static bool json_get_hex_color(const char *json, const char *key, float out[4])
{
    const char *v = json_find_key(json, key);
    if (!v || *v != '"' || v[1] != '#') return false;
    uint32_t word = (uint32_t)strtoul(v + 2, NULL, 16);
    out[0] = (float)((word >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((word >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(word & 0xFF) / 255.0f;
    out[3] = (float)((word >> 24) & 0xFF) / 255.0f;
    return true;
}

static bool collect_engine_glows(const cgltf_node *mesh_node, uint16_t mesh_slot,
                                 AeronGltfEngineGlow **list,
                                 uint32_t *count, uint32_t *cap)
{
    for (cgltf_size i = 0; i < mesh_node->children_count; i++) {
        const cgltf_node *ch = mesh_node->children[i];
        if (ch->mesh || !ch->extras.data ||
            !strstr(ch->extras.data, "tieEngineGlow"))
            continue;
        if (*count == *cap) {
            uint32_t new_cap = *cap ? *cap * 2u : 8u;
            AeronGltfEngineGlow *grown = (AeronGltfEngineGlow *)
                realloc(*list, new_cap * sizeof(AeronGltfEngineGlow));
            if (!grown) return false;
            *list = grown;
            *cap  = new_cap;
        }
        AeronGltfEngineGlow *eg = &(*list)[(*count)++];
        memset(eg, 0, sizeof *eg);
        float pos_gltf[3] = {0, 0, 0};
        if (ch->has_translation) {
            pos_gltf[0] = ch->translation[0];
            pos_gltf[1] = ch->translation[1];
            pos_gltf[2] = ch->translation[2];
        }
        swap_yz3(pos_gltf, eg->position);
        const char *ex = ch->extras.data;
        float v3[3];
        if (json_get_vec(ex, "lookAxis", 3, v3))  swap_yz3(v3, eg->look);
        if (json_get_vec(ex, "upAxis", 3, v3))    swap_yz3(v3, eg->up);
        if (json_get_vec(ex, "rightAxis", 3, v3)) swap_yz3(v3, eg->right);
        /* Dimensions are the OPT's raw half extents (not axis-swapped
         * by opt2gltf — they pair with the axis vectors above). */
        json_get_vec(ex, "dimensions", 3, eg->dimensions);
        json_get_hex_color(ex, "coreColor", eg->core_rgba);
        json_get_hex_color(ex, "outerColor", eg->outer_rgba);
        bool disabled = false;
        json_get_bool(ex, "disabled", &disabled);
        eg->disabled  = disabled ? 1 : 0;
        eg->mesh_slot = (uint8_t)(mesh_slot < 0xFFu ? mesh_slot : 0xFFu);
    }
    return true;
}

/* Parse mesh-node extras into the ship-level mesh_rot slot. Returns
 * false on missing identity (tieMeshIndex), which the caller treats
 * as a soft skip of that node. */
static bool parse_node_extras(const cgltf_node *node,
                              uint16_t *opt_mesh_index_out,
                              AeronMeshRot *slot)
{
    const char *ex = node->extras.data;
    int v;
    if (!json_get_int(ex, "tieMeshIndex", &v)) return false;
    *opt_mesh_index_out = (uint16_t)v;
    memset(slot, 0, sizeof *slot);
    if (json_get_int(ex, "tieMeshType", &v))
        slot->mesh_type = (uint8_t)v;
    float pivot[3] = {0}, axis[3] = {0};
    if (json_get_vec(ex, "pivot",        3, pivot) &&
        json_get_vec(ex, "rotationAxis", 3, axis)) {
        float tmp[3];
        swap_yz3(pivot, tmp); memcpy(slot->pivot, tmp, sizeof tmp);
        swap_yz3(axis,  tmp); memcpy(slot->axis,  tmp, sizeof tmp);
        /* has_rotation = the mesh HAS a rotation axis. Whether it
         * currently rotates is runtime state — the game gates on its
         * per-mesh angle (XWA rotates plain MainHull meshes like the
         * hangar crane, so the cook's rotary-type `animated` heuristic
         * must not gate the data). Rotary classification stays
         * available via mesh_type. */
        slot->has_rotation = 1u;
    }
    return true;
}

/* ===== Bound box ==================================================== */

static void accumulate_bbox(AeronGltfModel *out,
                            const AeronGltfVertex *verts, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const float *p = verts[i].pos;
        for (int k = 0; k < 3; k++) {
            if (p[k] < out->bound_min[k]) out->bound_min[k] = p[k];
            if (p[k] > out->bound_max[k]) out->bound_max[k] = p[k];
        }
    }
}

/* ===== Public API =================================================== */

static const char *kAtlasNames[AERON_GLTF_CHANNEL_COUNT] = {
    "atlas_base_color",
    "atlas_normal",
    "atlas_metallic_roughness",
    "atlas_emissive",
};

bool Aeron_GltfMeshBuildData(const cgltf_data *data,
                             const char *source_label,
                             AeronGltfModel *out)
{
    if (!data || !out) return false;
    if (!source_label) source_label = "<memory glTF>";
    memset(out, 0, sizeof *out);
    out->bound_min[0] = out->bound_min[1] = out->bound_min[2] = +INFINITY;
    out->bound_max[0] = out->bound_max[1] = out->bound_max[2] = -INFINITY;

    bool succeeded = false;

    typedef struct NodePlan {
        const cgltf_node *node;
        uint16_t opt_mi;
    } NodePlan;
    NodePlan *plans = NULL;

    /* ---- Channel KTX2 atlases ------------------------------------- */
    for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
        if (!copy_channel_ktx2(data, kAtlasNames[c], &out->channels[c]))
            goto cleanup;
    }

    /* ---- Variants ------------------------------------------------- */
    out->variant_count = (uint32_t)data->variants_count;
    out->variant_slots = out->variant_count > 0 ? out->variant_count : 1u;

    /* ---- Materials (factors + per-channel UV transform) ----------- */
    if (data->materials_count > AERON_GLTF_MAX_MATERIALS) {
        SDL_Log("[flight_gltf] '%s' has %zu materials (over cap %u)",
                source_label, data->materials_count,
                (unsigned)AERON_GLTF_MAX_MATERIALS);
        goto cleanup;
    }
    if (data->materials_count > 0) {
        out->materials = (AeronGltfMaterial *)calloc(
            data->materials_count, sizeof *out->materials);
        if (!out->materials) goto cleanup;
        out->material_count = (uint32_t)data->materials_count;
        for (uint32_t i = 0; i < out->material_count; i++) {
            if (!read_material(&data->materials[i], &out->materials[i])) {
                SDL_Log("[flight_gltf] '%s' material %u has invalid alpha state",
                        source_label, i);
                goto cleanup;
            }
        }
    }

    /* ---- Pass 1: count merged vertex / index / primitive totals, and
     * record per-node identities (opt_mesh_index, rotation slot).
     * Hardpoint child nodes are collected after Pass 2 has visited
     * their parents. */
    uint32_t total_v = 0, total_i = 0, total_prims = 0;
    uint32_t hp_cap = 0, hp_count = 0;
    uint32_t eg_cap = 0, eg_count = 0;

    plans = (NodePlan *)calloc(data->nodes_count, sizeof *plans);
    if (!plans) goto cleanup;
    uint32_t plan_count = 0;

    for (cgltf_size i = 0; i < data->nodes_count; i++) {
        const cgltf_node *n = &data->nodes[i];
        if (!n->mesh) continue;
        uint16_t opt_mi = 0;
        AeronMeshRot mr_slot;
        if (!parse_node_extras(n, &opt_mi, &mr_slot)) {
            SDL_Log("[flight_gltf] node %zu has no tieMeshIndex; skipped",
                    i);
            continue;
        }
        if (opt_mi >= AERON_MAX_MESH_SLOTS) {
            SDL_Log("[flight_gltf] node %zu opt_mesh_index=%u over cap; skipped",
                    i, (unsigned)opt_mi);
            continue;
        }
        out->mesh_rot[opt_mi] = mr_slot;

        NodePlan *plan = &plans[plan_count++];
        plan->node   = n;
        plan->opt_mi = opt_mi;

        for (cgltf_size pi = 0; pi < n->mesh->primitives_count; pi++) {
            const cgltf_primitive *p = &n->mesh->primitives[pi];
            const cgltf_accessor *pos = prim_attr(p, cgltf_attribute_type_position);
            const cgltf_accessor *idx = p->indices;
            if (!pos || !idx) continue;
            total_v     += (uint32_t)pos->count;
            total_i     += (uint32_t)idx->count;
            total_prims += 1u;
        }
    }

    if (total_v > 0xFFFFu) {
        SDL_Log("[flight_gltf] '%s' merged vertex count %u over uint16 cap",
                source_label, total_v);
        goto cleanup;
    }
    if (total_v > 0 && total_i > 0) {
        out->vertices = (AeronGltfVertex *)calloc(total_v,
                                                   sizeof *out->vertices);
        out->indices  = (uint16_t *)calloc(total_i, sizeof *out->indices);
        if (!out->vertices || !out->indices) goto cleanup;
        out->vertex_count = total_v;
        out->index_count  = total_i;
    }
    out->total_prim_count = total_prims;
    if (total_prims > 0) {
        out->prim_variant_material = (uint32_t *)malloc(
            (size_t)total_prims * out->variant_slots * sizeof(uint32_t));
        if (!out->prim_variant_material) goto cleanup;
        for (size_t k = 0; k < (size_t)total_prims * out->variant_slots; k++)
            out->prim_variant_material[k] = AERON_GLTF_NO_MATERIAL;
    }

    /* ---- Pass 2: decode + interleave + populate variant table. ---- */
    uint32_t voff = 0, ioff = 0, prim_id = 0;
    for (uint32_t pi = 0; pi < plan_count; pi++) {
        const NodePlan *plan = &plans[pi];
        const cgltf_mesh *mesh = plan->node->mesh;
        for (cgltf_size si = 0; si < mesh->primitives_count; si++) {
            const cgltf_primitive *p = &mesh->primitives[si];
            const cgltf_accessor *pos = prim_attr(p, cgltf_attribute_type_position);
            if (!pos || !p->indices) continue;
            if (!append_primitive_vertices(p, plan->opt_mi, prim_id,
                                           out->vertices, out->indices,
                                           &voff, &ioff))
                goto cleanup;
            uint32_t default_mat = p->material
                ? (uint32_t)cgltf_material_index(data, p->material)
                : AERON_GLTF_NO_MATERIAL;
            for (uint32_t vs = 0; vs < out->variant_slots; vs++)
                out->prim_variant_material[
                    (size_t)prim_id * out->variant_slots + vs] = default_mat;
            for (cgltf_size mi = 0; mi < p->mappings_count; mi++) {
                const cgltf_material_mapping *mp = &p->mappings[mi];
                if (mp->variant < out->variant_slots) {
                    uint32_t mat = mp->material
                        ? (uint32_t)cgltf_material_index(data, mp->material)
                        : AERON_GLTF_NO_MATERIAL;
                    out->prim_variant_material[
                        (size_t)prim_id * out->variant_slots + mp->variant] =
                        mat;
                }
            }
            prim_id++;
        }
        if (!collect_hardpoints(plan->node,
                                &out->hardpoints, &hp_count, &hp_cap))
            goto cleanup;
        if (!collect_engine_glows(plan->node, plan->opt_mi,
                                  &out->engine_glows, &eg_count, &eg_cap))
            goto cleanup;
    }
    out->hardpoint_count   = hp_count;
    out->engine_glow_count = eg_count;

    /* A fixed index partition requires every runtime material variant of a
     * primitive to remain in the same render class. */
    for (uint32_t pid = 0; pid < out->total_prim_count; ++pid) {
        const uint32_t default_material = out->prim_variant_material[
            (size_t)pid * out->variant_slots];
        const AeronGltfAlphaMode default_mode =
            material_alpha_mode(out, default_material);
        if ((int)default_mode < 0) {
            SDL_Log("[flight_gltf] '%s' primitive %u references invalid material %u",
                    source_label, pid, default_material);
            goto cleanup;
        }
        for (uint32_t variant = 1; variant < out->variant_slots; ++variant) {
            const uint32_t material = out->prim_variant_material[
                (size_t)pid * out->variant_slots + variant];
            const AeronGltfAlphaMode mode = material_alpha_mode(out, material);
            if ((int)mode < 0 || mode != default_mode) {
                SDL_Log("[flight_gltf] '%s' primitive %u variant %u changes alpha class "
                        "(material %u mode %d -> material %u mode %d)",
                        source_label, pid, variant, default_material,
                        (int)default_mode, material, (int)mode);
                goto cleanup;
            }
        }
    }

    /* ---- Partition indices: OPAQUE, MASK, BLEND, stable within each
     * class. Primitive alpha class is invariant across variants above. ---- */
    out->opaque_index_count = 0;
    out->mask_index_offset = 0;
    out->mask_index_count = 0;
    out->blend_index_offset = 0;
    out->blend_index_count = 0;
    if (out->index_count > 0) {
        uint16_t *sorted = (uint16_t *)malloc(
            (size_t)out->index_count * sizeof *sorted);
        if (!sorted) goto cleanup;
        uint32_t w = 0;
        for (int wanted_mode = AERON_GLTF_ALPHA_OPAQUE;
             wanted_mode <= AERON_GLTF_ALPHA_BLEND; ++wanted_mode) {
            const uint32_t range_start = w;
            for (uint32_t t = 0; t + 2 < out->index_count; t += 3) {
                const uint32_t pid = out->vertices[out->indices[t]].prim_id;
                const uint32_t mat = out->prim_variant_material[
                    (size_t)pid * out->variant_slots];
                const AeronGltfAlphaMode mode = material_alpha_mode(out, mat);
                if ((int)mode != wanted_mode) continue;
                sorted[w + 0] = out->indices[t + 0];
                sorted[w + 1] = out->indices[t + 1];
                sorted[w + 2] = out->indices[t + 2];
                w += 3;
            }
            if (wanted_mode == AERON_GLTF_ALPHA_OPAQUE) {
                out->opaque_index_count = w - range_start;
            } else if (wanted_mode == AERON_GLTF_ALPHA_MASK) {
                out->mask_index_offset = range_start;
                out->mask_index_count = w - range_start;
            } else {
                out->blend_index_offset = range_start;
                out->blend_index_count = w - range_start;
            }
        }
        if (w != out->index_count) {
            free(sorted);
            SDL_Log("[flight_gltf] '%s' index partition retained %u of %u indices",
                    source_label, w, out->index_count);
            goto cleanup;
        }
        memcpy(out->indices, sorted, (size_t)out->index_count * sizeof *sorted);
        free(sorted);
    }

    /* ---- Bounding sphere (snapshot world units) ------------------- */
    if (out->vertex_count > 0)
        accumulate_bbox(out, out->vertices, out->vertex_count);
    if (isfinite(out->bound_min[0])) {
        float ex = fmaxf(fabsf(out->bound_min[0]), fabsf(out->bound_max[0]));
        float ey = fmaxf(fabsf(out->bound_min[1]), fabsf(out->bound_max[1]));
        float ez = fmaxf(fabsf(out->bound_min[2]), fabsf(out->bound_max[2]));
        out->bound_radius = sqrtf(ex * ex + ey * ey + ez * ez)
                            * (1.0f / 65536.0f);
    }

    SDL_Log("[flight_gltf] %s: %u verts, %u indices (%u opaque, %u mask, "
            "%u blend), %u prims, "
            "%u materials, %u variants, atlases base=%zu nrm=%zu mr=%zu "
            "emis=%zu, radius=%g",
            source_label,
            out->vertex_count, out->index_count,
            out->opaque_index_count, out->mask_index_count,
            out->blend_index_count, out->total_prim_count,
            out->material_count, out->variant_count,
            out->channels[0].size, out->channels[1].size,
            out->channels[2].size, out->channels[3].size,
            (double)out->bound_radius);

    succeeded = true;

cleanup:
    free(plans);
    if (!succeeded) Aeron_GltfMeshFree(out);
    return succeeded;
}

bool Aeron_GltfMeshBuild(const char *glb_path, AeronGltfModel *out)
{
    if (!glb_path || !out) return false;
    cgltf_options opts = {0};
    cgltf_data *data = NULL;
    cgltf_result r = cgltf_parse_file(&opts, glb_path, &data);
    if (r != cgltf_result_success) {
        SDL_Log("[flight_gltf] parse '%s' failed: %d", glb_path, (int)r);
        return false;
    }
    r = cgltf_load_buffers(&opts, data, glb_path);
    if (r != cgltf_result_success) {
        SDL_Log("[flight_gltf] load_buffers '%s' failed: %d", glb_path, (int)r);
        cgltf_free(data);
        return false;
    }
    const bool succeeded = Aeron_GltfMeshBuildData(data, glb_path, out);
    cgltf_free(data);
    return succeeded;
}

bool Aeron_GltfMeshBuildMemory(const void *bytes, size_t size,
                               const char *source_label,
                               AeronGltfModel *out)
{
    if (!bytes || size == 0 || !source_label || !source_label[0] || !out)
        return false;
    memset(out, 0, sizeof *out);
    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse(&options, bytes, size, &data);
    if (result != cgltf_result_success) {
        SDL_Log("[flight_gltf] parse memory '%s' failed: %d",
                source_label, (int)result);
        return false;
    }
    for (cgltf_size index = 0; index < data->buffers_count; ++index) {
        if (!data->buffers[index].data) {
            SDL_Log("[flight_gltf] '%s' contains an external buffer",
                    source_label);
            cgltf_free(data);
            return false;
        }
    }
    const bool succeeded = Aeron_GltfMeshBuildData(data, source_label, out);
    cgltf_free(data);
    return succeeded;
}

void Aeron_GltfMeshFree(AeronGltfModel *m)
{
    if (!m) return;
    free(m->vertices);
    free(m->indices);
    free(m->materials);
    free(m->prim_variant_material);
    free(m->hardpoints);
    free(m->engine_glows);
    for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++)
        free(m->channels[c].data);
    memset(m, 0, sizeof *m);
}
