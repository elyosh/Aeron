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
 *   - identifies component nodes from the AERON_flight_model hierarchy
 *     and assigns their model-local ordinal to render vertices
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
#include "cJSON.h"

/* Host-provided log (SDL3 via SDL_Log; standalone tools stub it). */
extern void SDL_Log(const char *fmt, ...);

/* glTF +Y up / +Z front to Aeron +X right / -Y forward / +Z up. */
static inline void gltf_to_aeron3(const float in[3], float out[3])
{
    out[0] = -in[0];
    out[1] = -in[2];
    out[2] = in[1];
}

/* The handedness-changing frame conversion also reverses tangent.w. */
static inline void gltf_to_aeron_tangent(const float in[4], float out[4])
{
    out[0] = -in[0];
    out[1] = -in[2];
    out[2] = in[1];
    out[3] = -in[3];
}

static const char *node_flight_extension(const cgltf_node *node)
{
    if (!node) return NULL;
    for (cgltf_size index = 0; index < node->extensions_count; ++index) {
        const cgltf_extension *extension = &node->extensions[index];
        if (extension->name && extension->data &&
            strcmp(extension->name, "AERON_flight_model") == 0)
            return extension->data;
    }
    return NULL;
}

static bool json_object_string_equals(const char *json, const char *key,
                                      const char *expected)
{
    if (!json || !key || !expected) return false;
    cJSON *object = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *value = cJSON_IsObject(object)
        ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    const bool matches = cJSON_IsString(value) && value->valuestring &&
                         strcmp(value->valuestring, expected) == 0;
    cJSON_Delete(object);
    return matches;
}

static bool node_has_flight_role(const cgltf_node *node, const char *expected)
{
    return json_object_string_equals(node_flight_extension(node),
                                     "role", expected);
}

static void transform_point(const float matrix[16], const float in[3],
                            float out[3])
{
    out[0] = matrix[0] * in[0] + matrix[4] * in[1] +
             matrix[8] * in[2] + matrix[12];
    out[1] = matrix[1] * in[0] + matrix[5] * in[1] +
             matrix[9] * in[2] + matrix[13];
    out[2] = matrix[2] * in[0] + matrix[6] * in[1] +
             matrix[10] * in[2] + matrix[14];
}

static void transform_direction(const float matrix[16], const float in[3],
                                float out[3])
{
    out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
    out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
    out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
    const float length = sqrtf(out[0] * out[0] + out[1] * out[1] +
                               out[2] * out[2]);
    if (length > 1.0e-9f) {
        out[0] /= length;
        out[1] /= length;
        out[2] /= length;
    }
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
    if (json_object_string_equals(m->extras.data, "aeronEmissiveMode",
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
 * Also bakes per-vertex `mesh_index` (= component ordinal) and
 * `prim_id` (= global prim slot). */
static bool append_primitive_vertices(
    const cgltf_primitive *p, const cgltf_node *node,
    uint16_t component_index, uint32_t prim_id,
    AeronGltfVertex *verts, uint16_t *indices,
    uint32_t *voff_io, uint32_t *ioff_io)
{
    const cgltf_accessor *pos = prim_attr(p, cgltf_attribute_type_position);
    const cgltf_accessor *nrm = prim_attr(p, cgltf_attribute_type_normal);
    const cgltf_accessor *uv  = prim_attr(p, cgltf_attribute_type_texcoord);
    const cgltf_accessor *tan = prim_attr(p, cgltf_attribute_type_tangent);
    const cgltf_accessor *idx = p->indices;
    if (!pos || p->type != cgltf_primitive_type_triangles) return false;

    uint32_t vcount = (uint32_t)pos->count;
    uint32_t icount = idx ? (uint32_t)idx->count : vcount;
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

    float matrix[16];
    cgltf_node_transform_world(node, matrix);
    for (uint32_t i = 0; i < vcount; i++) {
        AeronGltfVertex *v = &verts[voff + i];
        float transformed[3];
        transform_point(matrix, &positions[i*3], transformed);
        gltf_to_aeron3(transformed, v->pos);
        transform_direction(matrix, &normals[i*3], transformed);
        gltf_to_aeron3(transformed, v->normal);
        transform_direction(matrix, &tangents[i*4], transformed);
        const float tangent[4] = {
            transformed[0], transformed[1], transformed[2], tangents[i*4+3]
        };
        gltf_to_aeron_tangent(tangent, v->tangent);
        v->uv[0]      = uvs[i*2 + 0];
        v->uv[1]      = uvs[i*2 + 1];
        v->mesh_index = (float)component_index;
        v->prim_id    = prim_id;
    }
    free(positions); free(normals); free(uvs); free(tangents);

    /* Indices — biased to the primitive's range inside the ship's
     * merged buffer. */
    uint16_t *dst_idx = indices + ioff;
    for (uint32_t i = 0; i < icount; i++) {
        cgltf_size raw = idx ? cgltf_accessor_read_index(idx, i) : i;
        uint32_t   adj = (uint32_t)raw + voff;
        dst_idx[i] = (adj < 0xFFFFu) ? (uint16_t)adj : 0xFFFFu;
    }

    *voff_io = voff + vcount;
    *ioff_io = ioff + icount;
    return true;
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

    /* ---- Pass 1: count merged vertex / index / primitive totals and
     * record component ordinals. */
    uint32_t total_v = 0, total_i = 0, total_prims = 0;
    if (!data->scene || data->scene->nodes_count != 1) goto cleanup;
    const cgltf_node *root = data->scene->nodes[0];
    if (!node_has_flight_role(root, "model")) goto cleanup;
    plans = (NodePlan *)calloc(root->children_count, sizeof *plans);
    if (!plans) goto cleanup;
    uint32_t plan_count = 0;

    for (cgltf_size i = 0; i < root->children_count; i++) {
        const cgltf_node *n = root->children[i];
        const uint16_t component_index = (uint16_t)i;
        if (!n->mesh || i >= AERON_MAX_MESH_SLOTS) goto cleanup;
        if (!node_has_flight_role(n, "component")) goto cleanup;

        NodePlan *plan = &plans[plan_count++];
        plan->node   = n;
        plan->opt_mi = component_index;

        for (cgltf_size pi = 0; pi < n->mesh->primitives_count; pi++) {
            const cgltf_primitive *p = &n->mesh->primitives[pi];
            const cgltf_accessor *pos =
                prim_attr(p, cgltf_attribute_type_position);
            const cgltf_accessor *idx = p->indices;
            if (!pos || p->type != cgltf_primitive_type_triangles)
                goto cleanup;
            total_v     += (uint32_t)pos->count;
            total_i     += idx ? (uint32_t)idx->count : (uint32_t)pos->count;
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
            const cgltf_accessor *pos =
                prim_attr(p, cgltf_attribute_type_position);
            if (!pos) goto cleanup;
            if (!append_primitive_vertices(p, plan->node, plan->opt_mi,
                                           prim_id, out->vertices,
                                           out->indices, &voff, &ioff))
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
    }

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

    SDL_Log("[flight_gltf] %s: %u verts, %u indices (%u opaque, %u mask, "
            "%u blend), %u prims, "
            "%u materials, %u variants, atlases base=%zu nrm=%zu mr=%zu "
            "emis=%zu",
            source_label,
            out->vertex_count, out->index_count,
            out->opaque_index_count, out->mask_index_count,
            out->blend_index_count, out->total_prim_count,
            out->material_count, out->variant_count,
            out->channels[0].size, out->channels[1].size,
            out->channels[2].size, out->channels[3].size);

    succeeded = true;

cleanup:
    free(plans);
    if (!succeeded) Aeron_GltfMeshFree(out);
    return succeeded;
}

void Aeron_GltfMeshFree(AeronGltfModel *m)
{
    if (!m) return;
    free(m->vertices);
    free(m->indices);
    free(m->materials);
    free(m->prim_variant_material);
    for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++)
        free(m->channels[c].data);
    memset(m, 0, sizeof *m);
}
