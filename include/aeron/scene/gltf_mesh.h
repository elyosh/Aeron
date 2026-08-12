#ifndef AERON_SCENE_GLTF_MESH_H
#define AERON_SCENE_GLTF_MESH_H

/*
 * Render payload for a cooked Aeron flight model.
 *
 * Input format: a `.glb` produced by `aeron_gltf_cook` (tools/gltf_cook/).
 * The cooker bakes every material's PBR textures into four KTX2 atlases
 * (BC7 for base_color / metallic_roughness / emissive, BC5 for normal),
 * embedded in the GLB BIN chunk via
 * `KHR_texture_basisu`. Each material's texture bindings carry a
 * `KHR_texture_transform` that remaps material-local UVs into the
 * material's sub-rect inside its channel atlas. No PNG decoding or
 * software atlas packing happens at runtime — the loader just lifts
 * the KTX2 blobs out of the BIN chunk and reads the per-binding UV
 * transform off cgltf.
 *
 * Component identity comes from direct-child order under the
 * `AERON_flight_model` model root. Flight semantics are owned by
 * AeronFlightModel rather than this GPU-oriented subobject.
 *   - `KHR_materials_variants` declares the asset-level variant list
 *     and per-primitive (variant_idx, material_idx) mappings. The
 *     variant selector at runtime is FlightObjectState.decal_color.
 *   - Exactly 4 textures, all `KHR_texture_basisu` referencing 4
 *     KTX2-payload images (one per channel), in this order:
 *       [0] base_color   (BC7 sRGB)
 *       [1] normal       (BC7 UNORM)
 *       [2] metallic_roughness (BC7 UNORM)
 *       [3] emissive     (BC7 sRGB)
 *
 * Flight files use glTF +Y up / +Z front. The loader converts them to
 * Aeron's +X right / -Y forward / +Z up frame.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/scene/mesh_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cgltf_data;

/* ===== Vertex format ================================================
 *
 * Standard PBR vertex stream with optional tangent slot. POSITION and
 * NORMAL are mandatory; TANGENT defaults to (0,0,0,1) when absent;
 * TEXCOORD_0 defaults to (0,0).
 *
 * 56 bytes per vertex. The renderer's pipeline declaration must
 * mirror this layout exactly. */
typedef struct AeronGltfVertex {
    float    pos[3];       /* renderer-frame: +Z up, -Y forward */
    float    normal[3];    /* unit, renderer-frame */
    float    tangent[4];   /* xyz unit + handedness sign in w */
    float    uv[2];        /* TEXCOORD_0 (material-local) */
    /* Model-local component ordinal used by the scene mesh table. */
    float    mesh_index;
    /* Stable global identifier (0..total_prim_count-1) for the source
     * primitive this vertex belongs to. The shader resolves it through
     * the mesh-owned, variant-major material-index storage table. */
    uint32_t prim_id;
} AeronGltfVertex;     /* 56 bytes */

/* ===== Channel slots ================================================
 *
 * Channel ordering — keep in sync with the cooker, the SDL_GPU
 * pipeline's texture register slots, and the FS sampling order. */
#define AERON_GLTF_CHANNEL_BASE_COLOR         0
#define AERON_GLTF_CHANNEL_NORMAL             1
#define AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS 2
#define AERON_GLTF_CHANNEL_EMISSIVE           3
#define AERON_GLTF_CHANNEL_COUNT              4

/* Per-model material cap used to validate and bound cooked assets. It is not
 * an original-engine value: the classic renderers walk per-face textures.
 * The measured XWA corpus peaks at 104 materials. GPU storage is allocated
 * to the actual retained count rather than this maximum. */
#define AERON_GLTF_MAX_MATERIALS 128
#define AERON_GLTF_NO_MATERIAL   0xFFFFFFFFu

/* ===== Channel KTX2 payload =========================================
 *
 * One KTX2 image per channel, copied out of the cooked graph's BIN data.
 * Shipped GLBs normally use BC5/BC7; runtime OPT conversion uses RGBA8.
 * The SDL_GPU side feeds these bytes to ktx2_open_mem at upload time,
 * creates the corresponding GPU texture, and uploads every mip. Bytes
 * live for the lifetime of the AeronGltfModel. */
typedef struct AeronGltfChannelKtx2 {
    uint8_t *data;
    size_t   size;
} AeronGltfChannelKtx2;

typedef enum AeronGltfEmissiveMode {
    /* Standard glTF behavior: emissive RGB is added to the lit material. */
    AERON_GLTF_EMISSIVE_ADDITIVE = 0,
    /* Emissive alpha is coverage. Reconstruct legacy fixed-function sRGB
     * filtering and SRCALPHA composition in the linear-HDR shader. */
    AERON_GLTF_EMISSIVE_LEGACY_SRGB_SRCALPHA = 1,
} AeronGltfEmissiveMode;

typedef enum AeronGltfAlphaMode {
    AERON_GLTF_ALPHA_OPAQUE = 0,
    AERON_GLTF_ALPHA_MASK   = 1,
    AERON_GLTF_ALPHA_BLEND  = 2,
} AeronGltfAlphaMode;

/* ===== Per-material entry ===========================================
 *
 * CPU source data copied into the mesh-owned material storage buffer.
 * Carries pure factor / flag data plus per-channel UV transform.
 *
 * `uv_xform[c]` = (offset_u, offset_v, scale_u, scale_v). The FS
 * applies:
 *     atlas_uv = vertex_uv * scale + offset
 * before sampling channel `c`'s atlas. Sentinel `scale_u == 0` (or
 * scale_v == 0) means "material doesn't author this channel" — FS
 * falls back to the per-material factor. */
typedef struct AeronGltfMaterial {
    float    base_color_factor[4];   /* RGBA, default (1,1,1,1) */
    float    emissive_factor[3];     /* default (0,0,0) */
    float    emissive_strength;      /* KHR default 1 */
    float    metallic_factor;        /* default 0 */
    float    roughness_factor;       /* default 1 */
    uint32_t double_sided;           /* bool */
    AeronGltfAlphaMode alpha_mode;
    float alpha_cutoff;               /* glTF MASK cutoff; default 0.5 */
    /* Material extras `aeronEmissiveMode`; AeronGltfEmissiveMode. */
    uint32_t emissive_mode;
    float    uv_xform[AERON_GLTF_CHANNEL_COUNT][4];
} AeronGltfMaterial;

/* ===== Ship asset ===================================================
 *
 * One merged VBO + IBO covers the whole ship; per-vertex `mesh_index`
 * + `prim_id` drive per-mesh affine lookup through scene storage and
 * per-primitive material resolution through mesh-owned variant storage.
 * The renderer issues one indexed draw per instance.
 *
 * `prim_variant_material` is the [total_prim_count][variant_slots]
 * resolution table — row prim, column variant_idx. variant_slots is
 * max(variant_count, 1) so ships without KHR_materials_variants still
 * have a column 0 holding the default material. AERON_GLTF_NO_MATERIAL
 * in a slot means "no material" (renderer skips fragments via factor-
 * only fallback). Mesh creation transposes this source table into
 * variant-major packed storage; each draw selects a row by index. */
typedef struct AeronGltfModel {
    /* Merged geometry — one buffer per ship. The index buffer contains
     * stable opaque, alpha-mask, and alpha-blend ranges in that order. */
    AeronGltfVertex *vertices;     uint32_t vertex_count;
    uint16_t         *indices;      uint32_t index_count;
    uint32_t          opaque_index_count;
    uint32_t          mask_index_offset;
    uint32_t          mask_index_count;
    uint32_t          blend_index_offset;
    uint32_t          blend_index_count;

    /* Per-channel cooked KTX2 atlases (4): BC5/BC7 or RGBA8. */
    AeronGltfChannelKtx2 channels[AERON_GLTF_CHANNEL_COUNT];

    /* Per-material entries (factors + per-channel UV transform). */
    uint32_t            material_count;
    AeronGltfMaterial *materials;   /* sized [material_count] */

    /* Variant table — flat [total_prim_count * variant_slots] row-
     * major, indexed (prim_id * variant_slots + variant_idx). */
    uint32_t  variant_count;     /* asset-level KHR count, 0 = none */
    uint32_t  variant_slots;     /* max(variant_count, 1) */
    uint32_t  total_prim_count;
    uint32_t *prim_variant_material;

} AeronGltfModel;

/* Internal render builder used by the common flight-model construction. */
bool Aeron_GltfMeshBuildData(const struct cgltf_data *data,
                             const char *source_label,
                             AeronGltfModel *out);

void Aeron_GltfMeshFree(AeronGltfModel *m);

#ifdef __cplusplus
}
#endif

#endif
