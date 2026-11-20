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
	uint8_t                    texture_clamp;
	float                      transform[16];
	const AeronSceneMeshTable* mesh_table;
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

/* The most images that one projected overlay draws. */
#define AERON_SCENE_MESH_PROJECTIONS_MAX 4

/* An image projected on a mesh. A point of the mesh gets the image's UV
 * from its position in the mesh's own frame, before the mesh table moves it:
 * u = dot(u_row.xyz, position) + u_row.w, and v the same way. */
typedef struct AeronSceneMeshProjection {
	float u_row[4];
	float v_row[4];
	float uv_rect[4]; /* min u, min v, max u, max v: nothing is drawn outside */
	float color[4];   /* multiplies the image; literal linear straight RGBA */
	/* The mesh-table rows (AeronGltfVertex.component) whose vertices take
	 * the image: bit r % 32 of word r / 32, for the rows 0 to 255. */
	uint32_t rows[8];
} AeronSceneMeshProjection;

/* Images of one texture projected on a mesh, such as the flashes of the
 * hits on a craft's shields. The scene draws the mesh's own triangles once
 * for all the images, so the game doesn't build or upload a copy of the
 * geometry, and several images cost one pass over the mesh. The images
 * blend one over the other in order, with premultiplied alpha, each as an
 * unlit AeronSceneMeshOverlayDesc with AERON_SCENE_MESH_OVERLAY_BLEND_PMA
 * would be drawn. `transform`, `mesh_table`, `depth_bias_view` and
 * `cull_mode` are as in AeronSceneMeshOverlayDesc. The mesh must stay alive
 * until the frame is rendered. */
typedef struct AeronSceneMeshProjectedOverlayDesc {
	const struct AeronSceneMesh*    mesh;
	AeronTexture*                   texture;
	float                           transform[16];
	const AeronSceneMeshTable*      mesh_table;
	float                           depth_bias_view;
	uint8_t                         cull_mode;
	const AeronSceneMeshProjection* projections;
	uint32_t                        projection_count; /* 1 to AERON_SCENE_MESH_PROJECTIONS_MAX */
} AeronSceneMeshProjectedOverlayDesc;

void AeronScene_AddMeshProjectedOverlay(AeronScene3D* scene, const AeronSceneMeshProjectedOverlayDesc* overlay);

#ifdef __cplusplus
}
#endif

#endif
