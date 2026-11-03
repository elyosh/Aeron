#ifndef AERON_GLTF_COOK_PROJECT_BUILD_H
#define AERON_GLTF_COOK_PROJECT_BUILD_H

/* aeron_gltf_cook --build-project: builds a project folder of the Blender
 * add-on into a mod folder:
 *   - each effects/<library>/<library>.yaml into <mod>/flight/effects, as
 *     --library does;
 *   - effects/effects.yaml is copied to <mod>/flight/effects;
 *   - each models/<key>/<key>.gltf is cooked into <mod>/flight/models/<key>.glb,
 *     after its materials are checked against the libraries. When the add-on
 *     has baked the model's probes into models/<key>/bake/probes, they are
 *     embedded as with --probes. */

#include "gltf_cook.h"

#include <stdbool.h>

typedef struct ProjectBuildOptions {
	const char*          project;
	const char*          mod;
	const char*          only; /* the key of the only model to cook, or NULL */
	int                  jobs; /* the number of models cooked at once */
	AeronGltfCookOptions cook;
} ProjectBuildOptions;

/* Builds the project. Returns false when something couldn't be built, after
 * building the rest. */
bool ProjectBuild_Run(const ProjectBuildOptions* options);

#endif /* AERON_GLTF_COOK_PROJECT_BUILD_H */
