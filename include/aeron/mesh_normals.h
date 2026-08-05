#ifndef AERON_MESH_NORMALS_H
#define AERON_MESH_NORMALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronMeshNormalsInput {
	const float *positions;
	size_t position_stride;
	uint32_t position_count;
	const uint32_t *triangle_position_indices;
	uint32_t triangle_count;
	float smooth_angle_degrees;
} AeronMeshNormalsInput;

typedef struct AeronMeshNormalsError {
	int code;
	char message[192];
} AeronMeshNormalsError;

/* Input buffers are borrowed and must remain valid until this call returns. */
bool Aeron_MeshNormalsBuildCorners(
		const AeronMeshNormalsInput *input,
		float *corner_normals,
		AeronMeshNormalsError *error);

#ifdef __cplusplus
}
#endif

#endif
