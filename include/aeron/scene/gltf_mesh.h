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
 *   - KTX2-payload atlas images, each one channel's pages for one set of
 *     variants, which the textures name with their page through
 *     AERON_flight_model (tools/extensions/AERON_flight_model).
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
 * The measured corpora peak at 156 materials.
 * GPU storage is allocated to the actual retained count rather than this
 * maximum, so no static array depends on it. */
#define AERON_GLTF_MAX_MATERIALS 4096
#define AERON_GLTF_NO_MATERIAL 0xFFFFFFFFu

/* Deepest mip level a channel binding may sample: a cooked texture's
 * AERON_flight_model `maxLod`, this value without it (no limit). Its
 * `layer`, the page of its image, is at most AERON_GLTF_ATLAS_MAX_PAGE. */
#define AERON_GLTF_ATLAS_UNCLAMPED_LOD 15u
#define AERON_GLTF_ATLAS_MAX_PAGE 4095u

/* ===== Channel KTX2 payload =========================================
 *
 * A cooked image: a KTX2 2D array of one channel's atlas pages, copied out
 * of the cooked graph's BIN data. A channel's core image is the one the
 * primitives' own materials use.
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
 * The images only some variants use: those no primitive's own material
 * references, in a channel some own material uses. Each is the group of the
 * variants whose materials reference it, in image order. A group's pages
 * follow its channel's core pages, in the order of that channel's groups; a
 * mesh uploads only the groups of the variants it shows
 * (AeronScene_MeshCreateWithVariants). */
#define AERON_GLTF_NO_VARIANT 0xFFFFFFFFu

typedef struct AeronGltfVariantGroup {
	uint32_t             channel;  /* AERON_GLTF_CHANNEL_* */
	AeronGltfChannelKtx2 ktx2;     /* the channel's layers */
	uint32_t             layers;   /* its layer count */
	uint32_t*            variants; /* the variants using it */
	uint32_t             variant_count;
} AeronGltfVariantGroup;

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
	float              uv_xform[AERON_GLTF_CHANNEL_COUNT][4];
	/* Atlas layer (core pages, then the channel's groups') and deepest
	 * sampled mip level of each channel binding. */
	uint32_t atlas_page[AERON_GLTF_CHANNEL_COUNT];
	uint32_t atlas_max_lod[AERON_GLTF_CHANNEL_COUNT];
	/* KHR_materials_specular factors; defaults (1, (1,1,1)). */
	float specular_factor;
	float specular_color_factor[3];
	/* KHR_materials_transmission factor, 0 without. A transmissive material
	 * draws in the blend range as thin glass, without refraction: it
	 * diffuses with 1 - transmission, passes the background times the
	 * transmission and the base colour's luminance (the spec's tint, made
	 * grey), and keeps its specular reflection. The base texture's alpha
	 * holds the transmission texture (aeron_gltf_cook), and the
	 * base-colour factor's alpha is unused. Its
	 * primitives' variant materials must then be BLEND or transmissive. */
	float transmission_factor;
	/* Screen surface (the material's AERON_flight_model `screen`): rectangles
	 * [screen_first, screen_first + screen_count) of the model's
	 * screen_rects and AERON_GLTF_SCREEN_* flags. All zero otherwise. */
	uint32_t screen_first;
	uint32_t screen_count;
	uint32_t screen_flags;
	/* Animation (`flipbooks`, "Material animation" below): the
	 * material's flipbooks [flipbook_first, flipbook_first +
	 * flipbook_count) of the model's. */
	uint32_t flipbook_first;
	uint32_t flipbook_count;
	/* Hull damage (`damage`): alternatives [damage_first, damage_first
	 * + damage_count) of the model's, and the sections whose random choice
	 * and placement apply. */
	uint32_t damage_first;
	uint32_t damage_count;
	uint32_t damage_choice;
	uint32_t damage_placement;
	/* Energy effects the material receives (its `energy`, or the model
	 * root's): bit per AERON_EFFECT_ENERGY_* (shields 0, tractor 1,
	 * jamming 2). */
	uint32_t energy_mask;
} AeronGltfMaterial;

/* ===== Material animation ===========================================
 *
 * A material's AERON_flight_model object (tools/extensions/AERON_flight_model):
 *
 *   "flipbooks": [ { "state", "layer": "baseColor" | "emissive",
 *                    "mode": "loop" | "once", "library",
 *                    "frames": [ [key, seconds], ... ],
 *                    "emissiveStrength": s, "unlit": u } ]
 *   "damage": { "library", "choice", "placement",
 *               "alternatives": [ { "stages": [key | null] x 3,
 *                                   "scale": [min, max],
 *                                   "offset": "inside" |
 *                                             { "min": [x, y], "max": [x, y] } } ] }
 *   "energy": [ "shields" | "tractor" | "jamming" ]
 *
 * The model root's `energy` applies to materials without their own. Frames
 * and damage stages name images of an effect library
 * (aeron/scene/effect_atlas.h). The `baseColor` layer is the renderer's
 * colour layer, `emissive` its illumination layer. `unlit` (0..1, default
 * 0), for a colour flipbook, is the part of its frames drawn unlit: while
 * one plays without an illumination frame, its colour c becomes base
 * c·(1 − u) and emission c·u, replacing the material's emission (0 keeps
 * the material's emission). On illumination frames, the optional
 * `strength` (default 1) multiplies the flipbook's `emissiveStrength`. */
typedef struct AeronGltfFlipbookFrame {
	char* key;
	float seconds;
	float strength; /* multiplies emissive_strength on illumination frames (1) */
} AeronGltfFlipbookFrame;

/* One hull-damage alternative: the `multiply` images of the 75, 50 and 25 %
 * stages (NULL: none at that stage) and its placement. */
typedef struct AeronGltfDamage {
	char*    library;
	char*    stages[3];
	bool     has_scale;
	float    scale[2];    /* RAND_SCALE min, max */
	uint32_t offset_mode; /* AERON_ANIM_OFFSET_* */
	float    offset[4];   /* range: min x, min y, max x, max y */
} AeronGltfDamage;

typedef struct AeronGltfFlipbook {
	uint32_t state; /* AERON_ANIM_STATE_* */
	uint32_t layer; /* AERON_ANIM_LAYER_* */
	uint32_t mode;  /* AERON_ANIM_MODE_* */
	char*    library;
	uint32_t frame_first; /* the model's flipbook_frames */
	uint32_t frame_count;
	float    emissive_strength; /* illumination frames' emission scale (1) */
	float    unlit;             /* colour frames' unlit weight (0) */
} AeronGltfFlipbook;

/* ===== Screen surfaces ==============================================
 *
 * A screen material shows a game-provided texture pair (graphics, text)
 * through rectangles of its TEXCOORD_0 space, sampled at per-frame source
 * rectangles (AeronScene_SetScreenSources), the material's AERON_flight_model
 * `screen`:
 *
 *   "screen": { "cover", "transparent", "alwaysVisible", "hologram",
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
 * resolution table — row prim, column 0 the primitive's own material and
 * column v + 1 variant v's, so ships without KHR_materials_variants have
 * the one column. AERON_GLTF_NO_MATERIAL
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
	uint32_t         mask_screen_index_count;
	uint32_t         blend_screen_index_count;

	/* Per-channel cooked KTX2 atlases (4): BC5/BC7 or RGBA8. */
	AeronGltfChannelKtx2 channels[AERON_GLTF_CHANNEL_COUNT];

	/* Per-material entries (factors + per-channel UV transform). */
	uint32_t           material_count;
	AeronGltfMaterial* materials; /* sized [material_count] */

	/* Variant table — flat [total_prim_count * variant_columns] row-
	 * major, indexed (prim_id * variant_columns + column): column 0 holds
	 * the primitive's own material, column v + 1 variant v's. */
	uint32_t  variant_count; /* asset-level KHR count, 0 = none */
	uint32_t  variant_columns; /* variant_count + 1 */
	uint32_t  total_prim_count;
	uint32_t* prim_variant_material;
	/* Variant names ([variant_count], NULL for an unnamed variant). */
	char** variant_names;
	/* The markings list (the model root's `markings`): the variant markings
	 * value m shows, or AERON_GLTF_NO_VARIANT for the own materials; values
	 * past the end show the last entry (Aeron_GltfMarkingsVariant). */
	uint32_t  markings_count;
	uint32_t* markings;
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
	/* Hull-damage alternatives of every material. */
	uint32_t         damage_count;
	AeronGltfDamage* damages;
} AeronGltfModel;

/* Internal render builder used by the common flight-model construction.
 * `data` must already satisfy the AERON_flight_model hierarchy (its
 * semantics were built from it, or opt2gltf generated it): roles are not
 * checked again here. */
bool Aeron_GltfMeshBuildData(const struct cgltf_data* data, const char* source_label, AeronGltfModel* out);

void Aeron_GltfMeshFree(AeronGltfModel* m);

/* The variant markings value `markings` shows: the markings list's entry
 * min(markings, count - 1); AERON_GLTF_NO_VARIANT (the own materials)
 * without a list. */
uint32_t Aeron_GltfMarkingsVariant(const AeronGltfModel* m, uint32_t markings);

#ifdef __cplusplus
}
#endif

#endif
