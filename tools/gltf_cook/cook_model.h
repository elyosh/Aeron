/*
 * The cook of one model into the GLB that the game loads: the cook itself,
 * then the cooked AERON_flight_model data.
 */

#ifndef AERON_GLTF_COOK_COOK_MODEL_H
#define AERON_GLTF_COOK_COOK_MODEL_H

#include <stdbool.h>

#include "gltf_cook.h"

/* Cooks `input` into the GLB `glb`, then adds the cooked flight data, with
 * the probes bake in `probes_dir` when it is set. Both are done in memory,
 * and the GLB is written once, as <glb>.tmp, then renamed, so `glb` is never
 * a partly written file. On failure, <glb>.tmp is removed and `glb` is left
 * as it was. Messages go to the tool log. */
bool aeron_gltf_cook_model_input(const AeronGltfCookInput* input, const char* glb,
								 const AeronGltfCookOptions* opts, const char* probes_dir);

/* aeron_gltf_cook_model_input for the .gltf file `gltf`, whose .bin and
 * images are the files next to it. */
bool aeron_gltf_cook_model(const char* gltf, const char* glb, const AeronGltfCookOptions* opts,
						   const char* probes_dir);

#endif /* AERON_GLTF_COOK_COOK_MODEL_H */
