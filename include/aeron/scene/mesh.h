#ifndef AERON_SCENE_SCENE_MESH_H
#define AERON_SCENE_SCENE_MESH_H

/*
 * AeronSceneMesh — GPU-resident model for AeronScene3D.
 *
 * Uploads an AeronFlightModel render payload into scene-side
 * GPU resources: one merged VBO/IBO in the fixed AeronGltfVertex
 * layout, the four BC7 channel atlases, the per-material storage
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
	uint32_t flags;                /* bit 0=has_normal, 1=has_MR, 2=has_emissive,
									* 3=alpha_blend (FS alpha = tex.a * factor.a),
									* 4=legacy sRGB/SRCALPHA emissive mode,
									* 5=alpha_mask */
	uint32_t _pad[3];
} AeronPbrMaterialEntry; /* 128 B */

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
	AeronBuffer* ibo; /* uint16 indices */
	uint32_t     vertex_count;
	uint32_t     index_count;
	/* Stable opaque, alpha-mask, and alpha-blend ranges. */
	uint32_t     opaque_index_count;
	uint32_t     mask_index_offset;
	uint32_t     mask_index_count;
	uint32_t     blend_index_offset;
	uint32_t     blend_index_count;
	AeronSceneMeshCpuVertex* cpu_vertices;
	uint16_t*                 cpu_indices;

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

	/* Per-mesh-slot articulation (indexed by the per-vertex mesh index). */
	AeronMeshRot mesh_rot[AERON_MAX_MESH_SLOTS];
	bool         has_any_rotation;

	float bound_min[3];
	float bound_max[3];
	float bound_radius;
	/* Engine glows copied from the source flight model (owned). */
	AeronFlightEngineGlow* engine_glows;
	uint32_t             engine_glow_count;
} AeronSceneMesh;

typedef enum AeronSceneMeshCreateStatus {
	AERON_SCENE_MESH_CREATE_SUCCESS = 0,
	/* The authored model payload is malformed and a caller may select another
	 * content source before recording more GPU work. */
	AERON_SCENE_MESH_CREATE_INVALID_SOURCE,
	/* CPU/GPU resource creation or command recording failed. Retrying with a
	 * different content source is not valid. */
	AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE,
} AeronSceneMeshCreateStatus;

/* Upload `model` through `cmd` (no pass may be open). Returns NULL on any
 * failure and classifies it through `status`; everything allocated so far is
 * released. `debug_name` labels diagnostics. The caller owns `model`. */
AeronSceneMesh* AeronScene_MeshCreate(AeronCommandBuffer* cmd, const AeronFlightModel* model,
									  const char* debug_name,
									  AeronSceneMeshCreateStatus* status);

void AeronScene_MeshDestroy(AeronSceneMesh* mesh);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_SCENE_MESH_H */
