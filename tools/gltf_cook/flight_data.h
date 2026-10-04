#ifndef AERON_GLTF_COOK_FLIGHT_DATA_H
#define AERON_GLTF_COOK_FLIGHT_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Adds cooked `simulationData` (per component) and `renderData` (model root)
 * to a cooked GLB that uses AERON_flight_model, replacing any existing ones,
 * and verifies the result loads warning-free. GLBs without the extension are
 * left alone (*applied = false, returns true). On success with *applied, the
 * caller frees *out. */
bool aeron_gltf_cook_flight_data(const uint8_t* in, size_t in_size, uint8_t** out, size_t* out_size, bool* applied,
								 char* error, size_t error_size);

#endif
