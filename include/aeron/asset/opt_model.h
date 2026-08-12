#ifndef AERON_ASSET_OPT_MODEL_H
#define AERON_ASSET_OPT_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#include "aeron/asset/flight_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronOptModelBuildOptions {
	float smooth_angle_degrees;
	float emissive_strength;
	bool emissive;
	const struct AeronOptAlphaOverride* alpha_overrides;
	size_t alpha_override_count;
} AeronOptModelBuildOptions;

typedef struct AeronOptAlphaOverride {
	const char* texture_name;
	AeronGltfAlphaMode alpha_mode;
	float alpha_cutoff;
} AeronOptAlphaOverride;

typedef struct AeronOptModelError {
	int code;
	char message[256];
} AeronOptModelError;

bool Aeron_OptModelBuildMemory(
		const void *bytes,
		size_t size,
		const char *label,
		const AeronOptModelBuildOptions *options,
		AeronFlightModel *out,
		AeronOptModelError *error);

#ifdef __cplusplus
}
#endif

#endif
