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

/* The maximum number of images in one projected overlay. */
#define AERON_SCENE_MESH_PROJECTIONS_MAX 4

/* One image projected on a mesh. The image's UV at a point of the mesh is
 * computed from the point's position:
 *   u = dot(u_row.xyz, position) + u_row.w
 *   v = dot(v_row.xyz, position) + v_row.w
 * The position is in the mesh's own space, before the mesh table moves the
 * vertex. */
typedef struct AeronSceneMeshProjection {
	float u_row[4];
	float v_row[4];
	float uv_rect[4]; /* min u, min v, max u, max v. Nothing is drawn outside it. */
	float color[4];   /* The image is multiplied by this. Linear, straight alpha. */
	/* Which mesh-table rows (AeronGltfVertex.component) the image applies
	 * to. Row r uses bit r % 32 of word r / 32. Only rows 0 to 255 can be
	 * selected. */
	uint32_t rows[8];
} AeronSceneMeshProjection;

/* Several images from one texture, projected on a mesh. An example is the
 * flashes from hits on a craft's shields.
 *
 * The scene draws the mesh's own triangles once for all the images. The
 * game does not need to build or upload a copy of the geometry, and several
 * images only cost one pass over the mesh.
 *
 * The images are blended in order with premultiplied alpha. The result is
 * the same as drawing each one as an unlit AeronSceneMeshOverlayDesc with
 * AERON_SCENE_MESH_OVERLAY_BLEND_PMA.
 *
 * `transform`, `mesh_table`, `depth_bias_view` and `cull_mode` mean the same
 * as in AeronSceneMeshOverlayDesc. The mesh must stay alive until the frame
 * is rendered. */
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
