#ifndef AERON_ASSET_FLIGHT_MODEL_H
#define AERON_ASSET_FLIGHT_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/collision/face_bvh.h"
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

/* Game-defined component tag: lower-case ASCII letters, digits, '.' and '_'. */
#define AERON_FLIGHT_TAG_MAX 64

typedef struct AeronFlightTag {
	char name[AERON_FLIGHT_TAG_MAX];
} AeronFlightTag;

typedef struct AeronFlightComponent {
	int32_t             mesh_type;
	uint32_t            explosion_flags;
	int32_t             target_id;
	AeronFlightVec3     target;
	bool                has_descriptor;
	AeronFlightVec3     descriptor_span;
	AeronFlightVec3     descriptor_center;
	AeronFlightBounds   descriptor_bounds;
	bool                has_rotation;
	AeronFlightRotation rotation;
	/* Simulation topology: the collision child's mesh when has_collision is
	 * set, otherwise the component's render mesh. */
	bool                has_collision;
	AeronFlightTopology topology;
	/* Render-mesh bounds; the model bounds are their union. They remain
	 * independent from the authored descriptor and from a collision child. */
	AeronFlightBounds     bounds;
	uint32_t              hardpoint_count;
	AeronFlightHardpoint* hardpoints;
	uint32_t              first_engine_glow;
	uint32_t              engine_glow_count;
	uint32_t              tag_count;
	AeronFlightTag*       tags;
	/* Cooked simulation data (`simulationData`): an owned copy of its buffer
	 * view, verified against the topology by its consumer. */
	bool                  has_simulation_data;
	uint8_t*              simulation_data;
	size_t                simulation_data_size;
	uint64_t              simulation_topology_hash;
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
	/* Cooked render data (`renderData` on the model root): an owned copy of
	 * its buffer view, verified against the merged render arrays. */
	bool                   has_render_data;
	uint8_t*               render_data;
	size_t                 render_data_size;
	uint64_t               render_mesh_hash;
	/* Box-only BVH over the merged render triangles (renderer frame, metres),
	 * filled by the render-including builds: cooked when renderData is
	 * present, otherwise built at load. NULL for semantic-only builds. Freed
	 * with the render data. */
	AeronFaceBvh*          render_bvh;
} AeronFlightModel;

bool Aeron_FlightModelBuildData(const struct cgltf_data* data, const char* source_label,
								AeronFlightModel* out);

bool Aeron_FlightModelBuildMemory(const void* bytes, size_t size, const char* source_label,
								  AeronFlightModel* out);

/* Builds only the simulation-facing data (components, topology, hardpoints,
 * engine glows, bounds); `render` stays zeroed. Free with
 * Aeron_FlightModelFreeSemantics. */
bool Aeron_FlightModelBuildSemanticsMemory(const void* bytes, size_t size, const char* source_label,
										   AeronFlightModel* out);

void Aeron_FlightModelReleaseRenderData(AeronFlightModel* model);
/* Frees the data only simulation consumers read: each component's topology,
 * hardpoints, tags and cooked simulation data, and the cooked render data.
 * What AeronScene_MeshCreate reads stays: render data, render BVH,
 * component mesh types, rotations and bounds, engine glows and model bounds.
 * For a game that builds its simulation data first and then hands the
 * model to its renderer. */
void Aeron_FlightModelReleaseSimulationData(AeronFlightModel* model);
void Aeron_FlightModelFree(AeronFlightModel* model);
/* Frees the simulation-facing data only; `render` is left untouched. */
void Aeron_FlightModelFreeSemantics(AeronFlightModel* model);

/* Returns how many components carry `tag`. Games that bind a tag to one
 * component check for exactly one. */
uint32_t Aeron_FlightModelCountTag(const AeronFlightModel* model, const char* tag);

#ifdef __cplusplus
}
#endif

#endif
