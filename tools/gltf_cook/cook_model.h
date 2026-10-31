/*
 * The cook of one model into the GLB that the game loads: the cook itself,
 * then the cooked AERON_flight_model data.
 */

#ifndef AERON_GLTF_COOK_COOK_MODEL_H
#define AERON_GLTF_COOK_COOK_MODEL_H

#include <stdbool.h>

#include "gltf_cook.h"

/* Cooks the .gltf `gltf` into the GLB `glb`, then adds the cooked flight
 * data, with the probes bake in `probes_dir` when it is set. The GLB is
 * written as <glb>.tmp and renamed at the end, so `glb` is never a partly
 * written file. On failure, <glb>.tmp is removed and `glb` is left as it
 * was. Messages go to the tool log. */
bool aeron_gltf_cook_model(const char* gltf, const char* glb, const AeronGltfCookOptions* opts,
						   const char* probes_dir);

#endif /* AERON_GLTF_COOK_COOK_MODEL_H */
