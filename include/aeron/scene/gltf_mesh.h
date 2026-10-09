#ifndef AERON_SCENE_GLTF_MESH_H
#define AERON_SCENE_GLTF_MESH_H

/*
 * Render payload for a cooked Aeron flight model.
 *
 * Input format: glTF data produced by `aeron_gltf_cook` (tools/gltf_cook/),
 * either a shipped `.glb` or the in-memory cook of a runtime OPT conversion.
 * The cooker bakes every material's PBR textures into four KTX2 atlases
 * (BC7 for base_color / metallic_roughness / emissive and BC5 for normal, or
 * uncompressed RGBA8 for the runtime cook), embedded in the BIN chunk via
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
 *     and per-primitive (variant_idx, material_idx) mappings. The game
 *     selects the variant per instance (AeronSceneMeshInstance.variant).
 *   - Four KTX2-payload atlas images, one per channel, named
 *     `atlas_base_color`, `atlas_normal`, `atlas_metallic_roughness` and
 *     `atlas_emissive`.
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
	float pos[3];     /* renderer-frame: +Z up, -Y forward */
	float normal[3];  /* unit, renderer-frame */
	float tangent[4]; /* xyz unit + handedness sign in w */
	float uv[2];      /* TEXCOORD_0 (material-local) */
	/* Model-local component ordinal used by the scene mesh table. */
	float component;
	/* Stable global identifier (0..total_prim_count-1) for the source
	 * primitive this vertex belongs to. The shader resolves it through
	 * the mesh-owned, variant-major material-index storage table. */
	uint32_t prim_id;
} AeronGltfVertex; /* 56 bytes */

/* ===== Channel slots ================================================
 *
 * Channel ordering — keep in sync with the cooker, the SDL_GPU
 * pipeline's texture register slots, and the FS sampling order. */
#define AERON_GLTF_CHANNEL_BASE_COLOR 0
#define AERON_GLTF_CHANNEL_NORMAL 1
#define AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS 2
#define AERON_GLTF_CHANNEL_EMISSIVE 3
#define AERON_GLTF_CHANNEL_COUNT 4

/* Per-model material cap used to validate and bound cooked assets. It is not
 * an original-engine value: the classic renderers walk per-face textures.
 * The measured XWA corpus peaks at 104 materials and the XWAU corpus at 156.
 * GPU storage is allocated to the actual retained count rather than this
 * maximum, so no static array depends on it. */
#define AERON_GLTF_MAX_MATERIALS 4096
#define AERON_GLTF_NO_MATERIAL 0xFFFFFFFFu

/* Deepest mip level a channel binding may sample: the cooker names each
 * atlas texture `tex_<channel>_p<page>_l<max_lod>`; a binding whose texture
 * name carries no such suffix samples page 0 without a LOD limit. */
#define AERON_GLTF_ATLAS_UNCLAMPED_LOD 15u
#define AERON_GLTF_ATLAS_MAX_PAGE 4095u

/* ===== Channel KTX2 payload =========================================
 *
 * One KTX2 2D-array image per channel, copied out of the cooked graph's BIN
 * data.
 * Shipped GLBs normally use BC5/BC7; runtime OPT conversion uses RGBA8.
 * The SDL_GPU side feeds these bytes to ktx2_open_mem at upload time,
 * creates the corresponding GPU texture, and uploads every mip. Bytes
 * live for the lifetime of the AeronGltfModel. */
typedef struct AeronGltfChannelKtx2 {
	uint8_t* data;
	size_t   size;
} AeronGltfChannelKtx2;

/* ===== Variant groups ===============================================
 *
 * Atlas pages only some variants use (XWAU skins: base colour, emissive,
 * metallic-roughness), cooked as separate KTX2 blocks and listed in the
 * asset extras `aeronVariantGroups`. A group's pages follow its channel's
 * core pages, in the order of that channel's groups; a mesh uploads
 * only the groups of the variants it shows (AeronScene_MeshCreateWithVariants).
 * The asset extras also give `aeronAssetVariants`, the number of leading
 * variants that are the model's own paint states (the variant indices of a mesh built
 * without a variant choice), and `aeronSkinMarkings`, the variant name each
 * markings value shows by default. */
#define AERON_GLTF_NO_VARIANT 0xFFFFFFFFu

typedef struct AeronGltfVariantGroup {
	uint32_t             channel;  /* AERON_GLTF_CHANNEL_* */
	AeronGltfChannelKtx2 ktx2;     /* the channel's layers */
	uint32_t             layers;   /* its layer count */
	uint32_t*            variants; /* the variants using it */
	uint32_t             variant_count;
} AeronGltfVariantGroup;

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
	float              base_color_factor[4]; /* RGBA, default (1,1,1,1) */
	float              emissive_factor[3];   /* default (0,0,0) */
	float              emissive_strength;    /* KHR default 1 */
	float              metallic_factor;      /* default 0 */
	float              roughness_factor;     /* default 1 */
	uint32_t           double_sided;         /* bool */
	AeronGltfAlphaMode alpha_mode;
	float              alpha_cutoff; /* glTF MASK cutoff; default 0.5 */
	/* Material extras `aeronEmissiveMode`; AeronGltfEmissiveMode. */
	uint32_t emissive_mode;
	float    uv_xform[AERON_GLTF_CHANNEL_COUNT][4];
	/* Atlas layer and deepest sampled mip level of each channel binding. */
	uint32_t atlas_page[AERON_GLTF_CHANNEL_COUNT];
	uint32_t atlas_max_lod[AERON_GLTF_CHANNEL_COUNT];
	/* KHR_materials_specular factors; defaults (1, (1,1,1)). */
	float specular_factor;
	float specular_color_factor[3];
	/* Screen surface (material extras `aeronScreen`): rectangles
	 * [screen_first, screen_first + screen_count) of the model's
	 * screen_rects and AERON_GLTF_SCREEN_* flags. All zero otherwise. */
	uint32_t screen_first;
	uint32_t screen_count;
	uint32_t screen_flags;
	/* Animation (material extras, "Material animation" below): the
	 * base-atlas binding of `aeronLayerBase` (uv_xform convention, scale
	 * zero without one) and the material's flipbooks
	 * [flipbook_first, flipbook_first + flipbook_count) of the model's. */
	float    layer_base_xform[4];
	uint32_t layer_base_page;
	uint32_t layer_base_max_lod;
	uint32_t flipbook_first;
	uint32_t flipbook_count;
} AeronGltfMaterial;

/* ===== Material animation ===========================================
 *
 * XWAU animated materials (docs: xwau_animated_materials.md, "Model
 * material extras"), as converted by opt2gltf and cooked by
 * aeron_gltf_cook:
 *
 *   "aeronLayerBase": { "page", "lod", "offset": [u, v], "scale": [su, sv] }
 *   "aeronFlipbooks": [ { "state", "layer": "colour" | "illumination",
 *                         "mode": "loop" | "once", "library",
 *                         "frames": [ [key, seconds], ... ],
 *                         "uv": { "scale": [sx, 1], "offset": [ox, oy],
 *                                 "clamp": bool } } ]
 *
 * Frames name images of an effect library (aeron/scene/effect_atlas.h).
 * Illumination flipbooks are dropped from a material without
 * `aeronLayerBase`. */
typedef struct AeronGltfFlipbookFrame {
	char* key;
	float seconds;
} AeronGltfFlipbookFrame;

typedef struct AeronGltfFlipbook {
	uint32_t state; /* AERON_ANIM_STATE_* */
	uint32_t layer; /* AERON_ANIM_LAYER_* */
	uint32_t mode;  /* AERON_ANIM_MODE_* */
	char*    library;
	uint32_t frame_first; /* the model's flipbook_frames */
	uint32_t frame_count;
	bool     has_uv;
	bool     uv_clamp;
	float    uv_scale_x;
	float    uv_offset[2];
} AeronGltfFlipbook;

/* ===== Screen surfaces ==============================================
 *
 * A screen material shows a game-provided texture pair (graphics, text)
 * through rectangles of its TEXCOORD_0 space, sampled at per-frame source
 * rectangles (AeronScene_SetScreenSources). The converter writes them as
 * material extras:
 *
 *   "aeronScreen": { "cover", "transparent", "alwaysVisible", "hologram",
 *                    "noisy": bool, "hide": uint,
 *                    "rects": [ { "source": uint, "dst": [u0, v0, u1, v1],
 *                                 "bg", "highlight", "lock": linear RGB,
 *                                 "gain": float, "layers": uint,
 *                                 "bar": linear RGB (fill bars) } ] }
 *
 * `hide` is a game-defined bitmask (HUD regions the game stops drawing in
 * 2D while the material is on screen); the model ORs it over its materials. */
#define AERON_GLTF_SCREEN_MAX_SOURCES 64u
#define AERON_GLTF_SCREEN_MAX_RECTS_PER_MATERIAL 16u

#define AERON_GLTF_SCREEN_COVER 0x1u          /* base alpha < 1 shows the screen */
#define AERON_GLTF_SCREEN_TRANSPARENT 0x2u    /* unlit, alpha from luminance */
#define AERON_GLTF_SCREEN_ALWAYS_VISIBLE 0x4u /* transparent, kept with the 2D HUD */
#define AERON_GLTF_SCREEN_HOLOGRAM 0x8u
#define AERON_GLTF_SCREEN_NOISY 0x10u /* hologram scanlines */

#define AERON_GLTF_SCREEN_LAYER_GRAPHICS 0x1u
#define AERON_GLTF_SCREEN_LAYER_TEXT 0x2u
#define AERON_GLTF_SCREEN_LAYER_BAR 0x4u /* filled from the bottom; no texture */

typedef struct AeronGltfScreenRect {
	float    dst[4]; /* u0, v0, u1, v1; swapped corners mirror */
	float    bg[3];
	float    highlight[3];
	float    lock[3];
	float    bar[3];
	float    gain;
	uint32_t layers;
	uint32_t source; /* < AERON_GLTF_SCREEN_MAX_SOURCES */
} AeronGltfScreenRect;

/* ===== Ship asset ===================================================
 *
 * One merged VBO + IBO covers the whole ship; per-vertex `component`
 * + `prim_id` drive per-component affine lookup through scene storage and
 * per-primitive material resolution through mesh-owned variant storage.
 * The renderer issues one indexed draw per instance.
 *
 * `prim_variant_material` is the [total_prim_count][variant_columns]
 * resolution table — row prim, column variant_idx. variant_columns is
 * max(variant_count, 1) so ships without KHR_materials_variants still
 * have a column 0 holding the default material. AERON_GLTF_NO_MATERIAL
 * in a cell means "no material" (renderer skips fragments via factor-
 * only fallback). Mesh creation transposes this source table into
 * variant-major packed storage; each draw selects a row by index. */
typedef struct AeronGltfModel {
	/* Merged geometry — one buffer per ship. The index buffer contains
	 * stable opaque, alpha-mask, and alpha-blend ranges in that order. The
	 * opaque and blend ranges each end with the primitives that have a
	 * screen material in any variant: colour passes draw those with the
	 * screen shader, so the others run a shader without screen code. */
	AeronGltfVertex* vertices;
	uint32_t         vertex_count;
	uint32_t*        indices;
	uint32_t         index_count;
	uint32_t         opaque_index_count;
	uint32_t         mask_index_offset;
	uint32_t         mask_index_count;
	uint32_t         blend_index_offset;
	uint32_t         blend_index_count;
	uint32_t         opaque_screen_index_count;
	uint32_t         blend_screen_index_count;

	/* Per-channel cooked KTX2 atlases (4): BC5/BC7 or RGBA8. */
	AeronGltfChannelKtx2 channels[AERON_GLTF_CHANNEL_COUNT];

	/* Per-material entries (factors + per-channel UV transform). */
	uint32_t           material_count;
	AeronGltfMaterial* materials; /* sized [material_count] */

	/* Variant table — flat [total_prim_count * variant_columns] row-
	 * major, indexed (prim_id * variant_columns + variant_idx). */
	uint32_t  variant_count; /* asset-level KHR count, 0 = none */
	uint32_t  variant_columns; /* max(variant_count, 1) */
	uint32_t  total_prim_count;
	uint32_t* prim_variant_material;
	/* Variant names ([variant_count], NULL for an unnamed variant). */
	char** variant_names;
	/* Asset paint-state variants: aeronAssetVariants, else variant_count. */
	uint32_t asset_variant_count;
	/* Default variant per markings value (aeronSkinMarkings), or
	 * AERON_GLTF_NO_VARIANT for an unskinned one. */
	uint32_t  skin_markings_count;
	uint32_t* skin_markings;
	/* Base-colour variant groups. */
	uint32_t               group_count;
	AeronGltfVariantGroup* groups;

	/* Screen rectangles of every screen material, and the OR of their
	 * `hide` masks. */
	uint32_t             screen_rect_count;
	AeronGltfScreenRect* screen_rects;
	uint32_t             screen_hide_mask;

	/* Flipbooks of every material and their frames. */
	uint32_t                flipbook_count;
	AeronGltfFlipbook*      flipbooks;
	uint32_t                flipbook_frame_count;
	AeronGltfFlipbookFrame* flipbook_frames;
} AeronGltfModel;

/* Internal render builder used by the common flight-model construction.
 * `data` must already satisfy the AERON_flight_model hierarchy (its
 * semantics were built from it, or opt2gltf generated it): roles are not
 * checked again here. */
bool Aeron_GltfMeshBuildData(const struct cgltf_data* data, const char* source_label, AeronGltfModel* out);

void Aeron_GltfMeshFree(AeronGltfModel* m);

#ifdef __cplusplus
}
#endif

#endif
