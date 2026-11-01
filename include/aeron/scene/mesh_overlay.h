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
	float component;
	float normal[3]; /* the receiving surface's normal, read by lit overlays only */
} AeronSceneMeshOverlayVertex;

typedef struct AeronSceneMeshOverlayDesc {
	const AeronSceneMeshOverlayVertex* vertices;
	uint32_t                           vertex_count; /* triangle list */
	AeronTexture*                      texture;
	/* A 2D array texture, used instead of `texture` when set: layer
	 * `texture_layer`, sampling mips up to `texture_max_lod` (an image packed
	 * with neighbours, as in an AeronEffectAtlas). */
	AeronTexture* texture_array;
	uint32_t      texture_layer;
	uint32_t      texture_max_lod;
	/* Array textures: outside uv_rect, sample its edge texels (D3D CLAMP
	 * addressing) instead of discarding. */
	uint8_t                            texture_clamp;
	float                              transform[16];
	const AeronSceneMeshTable*         mesh_table;
	/* atlas_uv = uv * zw + xy. uv_rect is minU,minV,maxU,maxV. */
	float   uv_xform[4];
	float   uv_rect[4];
	float   color[4]; /* literal linear straight RGBA */
	float   depth_bias_view;
	uint8_t blend;
	uint8_t cull_mode;
	/* 0: the texture times `color` is the radiance drawn. 1: it is a base
	 * colour that the scene's lights shade like the opaque surface under it:
	 * the key light with its shadow, the other directional lights, the
	 * ambient and environment light with the ambient occlusion, and the
	 * point lights, with the vertices' normals and a rough dielectric
	 * specular. The probe sets don't light it. */
	uint8_t lit;
} AeronSceneMeshOverlayDesc;

void AeronScene_AddMeshOverlay(AeronScene3D* scene, const AeronSceneMeshOverlayDesc* overlay);

#ifdef __cplusplus
}
#endif

#endif
