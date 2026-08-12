#ifndef AERON_ASSET_FLIGHT_MODEL_H
#define AERON_ASSET_FLIGHT_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/scene/gltf_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AERON_FLIGHT_MODEL_EXTENSION "AERON_flight_model"

typedef struct AeronFlightVec3 {
	float x;
	float y;
	float z;
} AeronFlightVec3;

typedef struct AeronFlightBounds {
	AeronFlightVec3 min;
	AeronFlightVec3 max;
} AeronFlightBounds;

typedef struct AeronFlightFace {
	uint32_t        indices[3];
	AeronFlightVec3 normal;
} AeronFlightFace;

typedef struct AeronFlightTopology {
	uint32_t         position_count;
	AeronFlightVec3* positions;
	uint32_t         face_count;
	AeronFlightFace* faces;
} AeronFlightTopology;

typedef struct AeronFlightRotation {
	AeronFlightVec3 pivot;
	AeronFlightVec3 rotation_axis;
	AeronFlightVec3 direction_axis;
	AeronFlightVec3 up_axis;
} AeronFlightRotation;

typedef struct AeronFlightHardpoint {
	int32_t         type;
	AeronFlightVec3 position;
} AeronFlightHardpoint;

typedef struct AeronFlightEngineGlow {
	AeronFlightVec3 position;
	AeronFlightVec3 look;
	AeronFlightVec3 up;
	AeronFlightVec3 right;
	AeronFlightVec3 dimensions;
	float           core_rgba[4];
	float           outer_rgba[4];
	uint32_t        component_index;
	bool            enabled;
} AeronFlightEngineGlow;

typedef struct AeronFlightComponent {
	int32_t               mesh_type;
	uint32_t              explosion_flags;
	int32_t               target_id;
	AeronFlightVec3       target;
	bool                  has_rotation;
	AeronFlightRotation   rotation;
	AeronFlightTopology   topology;
	AeronFlightBounds     bounds;
	AeronFlightVec3       span;
	AeronFlightVec3       center;
	uint32_t              hardpoint_count;
	AeronFlightHardpoint* hardpoints;
	uint32_t              first_engine_glow;
	uint32_t              engine_glow_count;
} AeronFlightComponent;

typedef struct AeronFlightModel {
	AeronGltfModel         render;
	uint32_t               component_count;
	AeronFlightComponent*  components;
	uint32_t               engine_glow_count;
	AeronFlightEngineGlow* engine_glows;
	AeronFlightBounds      bounds;
	float                  max_extent;
	int32_t                bridge_component;
} AeronFlightModel;

bool Aeron_FlightModelBuildData(const struct cgltf_data* data, const char* source_label,
								AeronFlightModel* out);

bool Aeron_FlightModelBuildMemory(const void* bytes, size_t size, const char* source_label,
								  AeronFlightModel* out);

bool Aeron_FlightModelBuild(const char* glb_path, AeronFlightModel* out);

void Aeron_FlightModelReleaseRenderData(AeronFlightModel* model);
void Aeron_FlightModelFree(AeronFlightModel* model);

#ifdef __cplusplus
}
#endif

#endif
