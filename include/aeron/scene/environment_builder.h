#ifndef AERON_SCENE_ENVIRONMENT_BUILDER_H
#define AERON_SCENE_ENVIRONMENT_BUILDER_H

/*
 * The environment builder makes lighting cubes on the GPU from six rendered
 * faces.
 *
 * The game renders the six faces of a cube around the camera, and the
 * builder turns them into a diffuse cube and a specular cube, with the same
 * filters as the offline tool environment_bake (tools/imgbake/envmap.c):
 *   - the source cube is the faces with a box mip chain;
 *   - the diffuse cube is 32 px with one level, the cosine-weighted sum of
 *     the source's largest level of at most 64 px;
 *   - the specular cube has the faces' size and a full mip chain. Level 0 is
 *     the source's level 0. Level m is a GGX prefilter of roughness
 *     m / (levels - 1).
 *
 * Compute shaders write only 2D textures, because Metal cannot write one
 * face of a cube. Copies then move the results into the cubes. The builder
 * knows nothing of the game. It keeps its textures and pipelines for the
 * next build, and each build rewrites the cubes in place. All calls are
 * render-thread only.
 */

#include <stdbool.h>

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronEnvironmentBuilder AeronEnvironmentBuilder;

typedef struct AeronEnvironmentBuilderDesc {
	int size;             /* capture and specular face size; a power of two, 64 to 512 */
	int specular_samples; /* GGX samples per texel for levels 1 and up; 1 to 1024 */
} AeronEnvironmentBuilderDesc;

/* Creates the textures and pipelines. Returns NULL and logs on failure. */
AeronEnvironmentBuilder* AeronEnvironmentBuilder_Create(const AeronEnvironmentBuilderDesc* desc);
void                     AeronEnvironmentBuilder_Destroy(AeronEnvironmentBuilder* b);

/* Records face `face` (0 to 5, +X to -Z) from `rendered`, a size x size
 * RGBA16F texture such as a scene's colour target. With `mirror_x`, its
 * columns run from right to left in the face. */
bool AeronEnvironmentBuilder_StoreFace(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd, int face,
									   AeronTexture* rendered, bool mirror_x);
/* Records the filters and the copies into the cubes. Needs the six faces
 * stored since the last call. */
bool AeronEnvironmentBuilder_Finish(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd);
/* Gets the cubes of the last Finish, with strengths 1 and an identity
 * basis. Returns false before the first Finish. */
bool AeronEnvironmentBuilder_Result(const AeronEnvironmentBuilder* b, AeronScenePbrEnvironment* out);

#ifdef __cplusplus
}
#endif

#endif
