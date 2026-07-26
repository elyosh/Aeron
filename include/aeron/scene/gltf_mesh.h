#ifndef AERON_SCENE_GLTF_MESH_H
#define AERON_SCENE_GLTF_MESH_H

/*
 * Runtime-loaded cooked-ship asset — consumer side of the
 * OPT → opt2gltf → (artist edit) → aeron_gltf_cook pipeline.
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
 * The loader expects the aeron_gltf_cook output dialect:
 *   - Standard mesh / node tree from opt2gltf, with per-mesh-node
 *     `extras.tieMeshIndex` (stable OPT mesh-slot identity, drives
 *     the scene mesh-table storage lookup), `extras.tieMeshType` (OPT mesh_type enum),
 *     and optional `extras.tieRotation` (pivot + axes; the renderer
 *     uses pivot + rotationAxis).
 *   - Hardpoint child nodes named `hp_<Type>` with
 *     `extras.tieHardpoint = { type, typeName }` and a node
 *     translation. No mesh attached.
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
 * Coordinate convention: glTF +Y up / -Z forward (right-handed).
 * aeron_gltf_cook preserves opt2gltf's axis swap; this loader undoes it so
 * vertices land in the renderer's native frame (+Z up, -Y forward).
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
    /* OPT mesh-slot identity (== node.opt_mesh_index). The scene mesh-table
     * storage record is indexed by this value. */
    float    mesh_index;
    /* Stable global identifier (0..total_prim_count-1) for the source
     * primitive this vertex belongs to. The shader resolves it through
     * the mesh-owned, variant-major material-index storage table. */
    uint32_t prim_id;
} AeronGltfVertex;     /* 56 bytes */

/* ===== Engine glow ==================================================
 *
 * XWA OPT EngineGlow billboards, carried through opt2gltf/aeron_gltf_cook as
 * `engine_glow_*` child nodes with `tieEngineGlow` extras. All vectors
 * are in the renderer-native model frame (+Z up, -Y forward); colors
 * are linear-ish 0..1 RGBA decoded from the OPT's 0xAARRGGBB words. */
typedef struct AeronGltfEngineGlow {
    float   position[3];
    float   look[3];       /* glow plane normal (exhaust direction) */
    float   up[3];
    float   right[3];
    float   dimensions[3]; /* OPT half extents: right, up, depth */
    float   core_rgba[4];
    float   outer_rgba[4];
    uint8_t disabled;
    /* OPT mesh slot the glow node belongs to — rotary-mesh glows follow
     * their mesh's articulation, and damage knockout masks index the
     * per-model emitter order (list order = OPT node order). */
    uint8_t mesh_slot;
} AeronGltfEngineGlow;

/* ===== Hardpoint ====================================================
 *
 * Position-only marker. Consumed by AI / weapon spawn, not by the
 * per-frame render path. */
typedef struct AeronGltfHardpoint {
    float    position[3];   /* mesh-local, renderer-frame */
    uint8_t  type;          /* opt_hardpoint_type_t enum value */
} AeronGltfHardpoint;

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
    /* glTF alphaMode BLEND (transparent OPT faces — canopy glass).
     * Blend prims are partitioned to the tail of the index buffer and
     * drawn by the renderer's alpha-blend pipeline variant. MASK is
     * treated as opaque. */
    uint32_t alpha_blend;            /* bool */
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
    /* Merged geometry — one buffer per ship. The index buffer is
     * partitioned: indices [0, opaque_index_count) belong to opaque
     * primitives, [opaque_index_count, index_count) to alpha-BLEND
     * primitives (classified by the prim's default-variant material),
     * each group in source order. Renderers draw the opaque range in
     * the normal passes and the blend range in a late alpha-blended
     * no-depth-write draw (classic transparent OPT faces render after
     * the opaque hull in node order). */
    AeronGltfVertex *vertices;     uint32_t vertex_count;
    uint16_t         *indices;      uint32_t index_count;
    uint32_t          opaque_index_count;

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

    /* Per-mesh-slot rotation indexed by opt_mesh_index. Unused slots
     * stay zero / has_rotation == 0. */
    AeronMeshRot mesh_rot[AERON_MAX_MESH_SLOTS];

    /* Hardpoints (AI / weapon spawn). */
    AeronGltfHardpoint *hardpoints;  uint32_t hardpoint_count;

    /* Engine glows (XWA OPTs; empty for TIE ships). */
    AeronGltfEngineGlow *engine_glows; uint32_t engine_glow_count;

    /* Bounding box in raw OPT units. Bounding-sphere radius is
     * already in snapshot world units (raw_radius / 65536). */
    float bound_min[3];
    float bound_max[3];
    float bound_radius;
} AeronGltfModel;

/* ===== Public API ===================================================
 *
 * Build from an absolute .glb path (cooked output of aeron_gltf_cook).
 * Returns true on success and populates *out; ownership transfers to
 * caller (free with Aeron_GltfMeshFree). On failure, *out is
 * zeroed and false is returned (errors emit SDL_Log lines via the
 * host's logging). */
bool Aeron_GltfMeshBuild(const char *glb_path,
                            AeronGltfModel *out);

/* Build from an already-loaded cooked cgltf graph. The graph is borrowed for
 * the duration of the call; the returned AeronGltfModel owns all of its data. */
bool Aeron_GltfMeshBuildData(const struct cgltf_data *data,
                             const char *source_label,
                             AeronGltfModel *out);

void Aeron_GltfMeshFree(AeronGltfModel *m);

#ifdef __cplusplus
}
#endif

#endif
