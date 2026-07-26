#ifndef AERON_SCENE_MESH_OVERLAY_H
#define AERON_SCENE_MESH_OVERLAY_H

/* Compact receiver-local geometry drawn over an articulated scene mesh.
 * Games build the geometry (decal projection, highlight selection, etc.);
 * aeron_scene owns the per-frame copy/upload and depth-tested blend pass. */

#include <stdint.h>

#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronSceneMeshOverlayBlend {
	AERON_SCENE_MESH_OVERLAY_BLEND_ALPHA = 0,
	AERON_SCENE_MESH_OVERLAY_BLEND_ADDITIVE,
	AERON_SCENE_MESH_OVERLAY_BLEND_PMA,
} AeronSceneMeshOverlayBlend;

typedef struct AeronSceneMeshOverlayVertex {
	float pos[3];
	float uv[2];
	float mesh_index;
} AeronSceneMeshOverlayVertex;

typedef struct AeronSceneMeshOverlayDesc {
	const AeronSceneMeshOverlayVertex* vertices;
	uint32_t                           vertex_count; /* triangle list */
	AeronTexture*                      texture;
	float                              transform[16];
	const AeronSceneMeshTable*         mesh_table;
	/* atlas_uv = uv * zw + xy. uv_rect is minU,minV,maxU,maxV. */
	float   uv_xform[4];
	float   uv_rect[4];
	float   color[4]; /* linear straight RGBA; all-zero means white */
	float   depth_bias_view;
	uint8_t blend;
	uint8_t cull_mode;
} AeronSceneMeshOverlayDesc;

void AeronScene_AddMeshOverlay(AeronScene3D* scene, const AeronSceneMeshOverlayDesc* overlay);

#ifdef __cplusplus
}
#endif

#endif
