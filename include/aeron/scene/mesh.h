#ifndef AERON_SCENE_SCENE_MESH_H
#define AERON_SCENE_SCENE_MESH_H

/*
 * AeronSceneMesh — GPU-resident model for AeronScene3D.
 *
 * Uploads an AeronFlightModel render payload into scene-side
 * GPU resources: one merged VBO/IBO in the fixed AeronGltfVertex
 * layout, the four channel atlases, the per-material storage
 * payload the "pbr" material class consumes, the per-primitive variant
 * table, per-mesh-slot articulation, and bounds.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/asset/flight_model.h"
#include "aeron/render.h"
#include "aeron/scene/mesh_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-material entry the PBR fragment shader reads through per-mesh storage.
 * Mirrors HLSL packing (each line = one 16-byte slot). Sub-rect with
 * `zw == 0` means "channel absent for this material" — FS falls back
 * to the matching factor. */
typedef struct AeronPbrMaterialEntry {
	float    base_rect[4];         /* atlas base_color sub-rect */
	float    normal_rect[4];       /* atlas normal sub-rect */
	float    mr_rect[4];           /* atlas metallic-rough sub-rect */
	float    emissive_rect[4];     /* atlas emissive sub-rect */
	float    base_color_factor[4]; /* RGBA factor */
	float    emissive_factor[4];   /* (RGB, emissive_strength) packed */
	float    metal_rough[4];       /* (metallic, roughness, alpha_cutoff, _) */
	float    specular[4];          /* KHR_materials_specular (color.rgb, factor) */
	uint32_t atlas[4];             /* per channel (base, normal, MR, emissive):
									* bits 0-11 atlas page, 12-15 max mip level */
	uint32_t flags;                /* bit 0=has_normal, 1=has_MR, 2=has_emissive,
									* 3=alpha_blend (FS alpha = tex.a * factor.a),
									* 4=legacy sRGB/SRCALPHA emissive mode,
									* 5=alpha_mask */
	uint32_t screen_first;         /* screen rectangles (AeronPbrScreenRect) */
	uint32_t screen_count;
	uint32_t screen_flags; /* AERON_GLTF_SCREEN_* */
} AeronPbrMaterialEntry; /* 160 B */

/* Shader record of one screen rectangle (AeronGltfScreenRect): float4 and
 * uint4 members only, so the layout is the same on every backend. */
typedef struct AeronPbrScreenRect {
	float    dst[4];       /* u0, v0, u1, v1 */
	float    bg_gain[4];   /* background RGB, content gain */
	float    highlight[4]; /* background RGB while the reticle is highlighted */
	float    lock[4];      /* background RGB while a warhead lock is solid */
	float    bar[4];       /* fill-bar RGB */
	uint32_t info[4];      /* layers, source, _, _ */
} AeronPbrScreenRect;      /* 96 B */

/* Compact retained geometry for receiver-local effects such as decals.
 * It deliberately omits material data and tangents: projection only needs
 * positions, normals, mesh-slot identity, and triangle connectivity. */
typedef struct AeronSceneMeshCpuVertex {
	float pos[3];
	float normal[3];
	float mesh_index;
} AeronSceneMeshCpuVertex;

typedef struct AeronSceneMesh {
	AeronBuffer* vbo; /* merged AeronGltfVertex stream */
	AeronBuffer* ibo; /* uint32 indices */
	uint32_t     vertex_count;
	uint32_t     index_count;
	/* Stable opaque, alpha-mask, and alpha-blend ranges; the opaque and
	 * blend ranges end with their screen primitives (AeronGltfModel). */
	uint32_t                 opaque_index_count;
	uint32_t                 mask_index_offset;
	uint32_t                 mask_index_count;
	uint32_t                 blend_index_offset;
	uint32_t                 blend_index_count;
	uint32_t                 opaque_screen_index_count;
	uint32_t                 blend_screen_index_count;
	AeronSceneMeshCpuVertex* cpu_vertices;
	uint32_t*                cpu_indices;
	/* Box-only BVH over cpu_vertices/cpu_indices triangles, copied from the
	 * flight model; NULL for meshes built from stock OPTs. */
	AeronFaceBvh*            render_bvh;

	/* Channel atlases (base_color / normal / metallic_rough / emissive).
	 * Factor-only models have no atlases; textured models carry all four. */
	AeronTexture* atlas[AERON_GLTF_CHANNEL_COUNT];

	AeronBuffer* material_buffer;
	AeronBuffer* variant_buffer;
	uint32_t     material_count;
	uint32_t     total_prim_count;
	uint32_t     variant_slots;
	uint32_t     variant_count;
	uint32_t     variant_groups_per_row;
	bool         all_materials_single_sided;
	/* Screen rectangles (AeronPbrScreenRect), NULL without screen
	 * materials, the OR of the screen materials' game-defined `hide`
	 * masks (AeronGltfModel), and the sources the rectangles sample
	 * (bit per source; fill bars sample none). */
	AeronBuffer* screen_rect_buffer;
	uint32_t     screen_rect_count;
	uint32_t     screen_hide_mask;
	uint64_t     screen_source_mask;

	/* Per-mesh-slot articulation (indexed by the per-vertex mesh index);
	 * slot_count = the model's component count, the slots its mesh tables
	 * must fill. */
	AeronMeshRot mesh_rot[AERON_MAX_MESH_SLOTS];
	uint32_t     slot_count;
	bool         has_any_rotation;

	float bound_min[3];
	float bound_max[3];
	float bound_radius;
	/* Engine glows copied from the source flight model (owned). */
	AeronFlightEngineGlow* engine_glows;
	uint32_t               engine_glow_count;
	/* Point lights copied from the source flight model (owned). */
	AeronFlightLight* lights;
	uint32_t          light_count;
} AeronSceneMesh;

typedef enum AeronSceneMeshCreateStatus {
	AERON_SCENE_MESH_CREATE_SUCCESS = 0,
	/* The model payload is malformed: invalid index ranges, a missing atlas
	 * or an atlas that does not decode. */
	AERON_SCENE_MESH_CREATE_INVALID_SOURCE,
	/* CPU/GPU resource creation or command recording failed. */
	AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE,
} AeronSceneMeshCreateStatus;

/* Upload `model` through `cmd` (no pass may be open). Returns NULL on any
 * failure and classifies it through `status`; everything allocated so far is
 * released. `debug_name` labels diagnostics. The caller owns `model`. The
 * mesh's variant rows are the model's asset variants (asset_variant_count)
 * and no base-colour variant group is uploaded. */
AeronSceneMesh* AeronScene_MeshCreate(AeronCommandBuffer* cmd, const AeronFlightModel* model,
									  const char* debug_name, AeronSceneMeshCreateStatus* status);

/* A mesh's variant choice: row i (AeronSceneMeshInstance.variant) shows
 * variant `variants[i]`, or the default materials for AERON_GLTF_NO_VARIANT.
 * Only the base-colour variant groups those variants use are uploaded. */
typedef struct AeronSceneMeshVariants {
	const uint32_t* variants;
	uint32_t        count; /* at least 1 */
} AeronSceneMeshVariants;

AeronSceneMesh* AeronScene_MeshCreateWithVariants(AeronCommandBuffer* cmd, const AeronFlightModel* model,
												  const char*                   debug_name,
												  const AeronSceneMeshVariants* variants,
												  AeronSceneMeshCreateStatus*   status);

void AeronScene_MeshDestroy(AeronSceneMesh* mesh);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_SCENE_MESH_H */
