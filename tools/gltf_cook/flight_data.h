#ifndef AERON_GLTF_COOK_FLIGHT_DATA_H
#define AERON_GLTF_COOK_FLIGHT_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Adds cooked `simulationData` (per component) and `renderData` (model root)
 * in AERON_flight_model_cooked to a cooked GLB that uses AERON_flight_model,
 * replacing any existing ones, and verifies that the result loads without
 * warnings. If `probes_dir` is set, the probes bake in that directory
 * (bake.json, probes.ktx2, grid.ktx2) is added as the model root's
 * `probeData`, and the call fails if the bake files do not match. GLBs
 * without the extension are left unchanged (*applied = false, returns true);
 * with `probes_dir` set, they fail instead. On success with *applied, the
 * caller frees *out. */
bool aeron_gltf_cook_flight_data(const uint8_t* in, size_t in_size, const char* probes_dir, uint8_t** out,
								 size_t* out_size, bool* applied, char* error, size_t error_size);

#endif
