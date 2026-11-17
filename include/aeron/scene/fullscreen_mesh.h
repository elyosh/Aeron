#ifndef AERON_SCENE_FULLSCREEN_MESH_H
#define AERON_SCENE_FULLSCREEN_MESH_H

/* The mesh that a full-screen pass draws.
 *
 * Every full-screen pass of the scene (AO, bloom, tone map, ...) draws a
 * mesh of clip-space triangles. Normally that is a quad that covers the
 * whole target. In a headset it is the eye's visible-area mesh, so the
 * pass skips the pixels that the lens never shows. */

#include "aeron/render.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A range of a vertex buffer of clip-space positions, two floats for each
 * vertex, drawn as a triangle list. */
typedef struct AeronSceneFullscreenMesh {
	AeronBuffer* vertices;
	uint32_t     first;
	uint32_t     count;
} AeronSceneFullscreenMesh;

/* The mesh that covers the whole target: two triangles. It is created on
 * first use and released with the render backend. NULL when the buffer
 * could not be created. */
const AeronSceneFullscreenMesh* AeronSceneFullscreen_Whole(void);

/* Nonzero when the mesh covers the whole target. NULL does. */
int AeronSceneFullscreen_IsWhole(const AeronSceneFullscreenMesh* mesh);

/* Binds the mesh's vertex buffer and draws it. NULL draws the whole target.
 * The pipeline must be bound, and its vertex shader must be
 * scene_fullscreen_quad.vert. */
void AeronSceneFullscreen_Draw(AeronRenderPass* pass, const AeronSceneFullscreenMesh* mesh);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_FULLSCREEN_MESH_H */
