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

typedef enum AeronFlightLightShape {
	AERON_FLIGHT_LIGHT_POINT = 0,
	AERON_FLIGHT_LIGHT_TUBE,
	AERON_FLIGHT_LIGHT_PANEL,
} AeronFlightLightShape;

/* Sphere radius of a point light, and thickness of a tube, without an
 * authored radius (metres). */
#define AERON_FLIGHT_LIGHT_DEFAULT_RADIUS 0.1f

/* A light on a "light" node under a component: the node's KHR_lights_punctual
 * point light gives its linear colour, its intensity in candela and its
 * range (metres; 0 when the glTF light has none), the node's
 * AERON_flight_model object its shape. Model space, metres:
 *   - `position`: the centre;
 *   - `half_x`: a tube's half axis, or a panel's half extent along its first
 *     size; zero for a point;
 *   - `half_y`: a panel's half extent along its second size; zero otherwise;
 *   - `direction`: the unit emitting direction of a one-sided tube or a
 *     panel; zero for a light emitting all around;
 *   - `source_radius`: a point's sphere radius or a tube's thickness; zero
 *     for a panel.
 * A one-sided light's intensity is the intensity straight ahead; a tube
 * emits the total of a point light of its intensity. */
typedef struct AeronFlightLight {
	AeronFlightVec3       position;
	float                 color[3];
	float                 intensity;
	float                 range;
	uint32_t              component_index;
	AeronFlightLightShape shape;
	AeronFlightVec3       half_x;
	AeronFlightVec3       half_y;
	AeronFlightVec3       direction;
	float                 source_radius;
	/* Baked into the model's probes package: the game leaves the light out
	 * while the model's probes light the scene. */
	bool baked;
} AeronFlightLight;

/* Game-defined component tag: lower-case ASCII letters, digits, '.' and '_'. */
#define AERON_FLIGHT_TAG_MAX 64

typedef struct AeronFlightTag {
	char name[AERON_FLIGHT_TAG_MAX];
} AeronFlightTag;

/* Game-defined articulation: component `component` turns about its
 * `rotation` hinge, in a group the game drives. `component` indexes the
 * game's per-component state (0..254), so it may name a component of another
 * model the game draws with the same state, such as a cockpit. `group` and
 * `behavior` follow the tag syntax; `behavior` is empty when absent. `angle`
 * is in 1/256 turn, `speed` and `speed_back` in 1/256 turn per game step. */
typedef struct AeronFlightArticulation {
	char    group[AERON_FLIGHT_TAG_MAX];
	char    behavior[AERON_FLIGHT_TAG_MAX];
	uint8_t component;
	int16_t angle;
	uint8_t speed;
	uint8_t speed_back;
} AeronFlightArticulation;

/* Game-defined model articulation setting: a name with the tag syntax and an
 * integer value. */
typedef struct AeronFlightArticulationSetting {
	char    name[AERON_FLIGHT_TAG_MAX];
	int32_t value;
} AeronFlightArticulationSetting;

/* Game-defined named set of component ordinals (0..254). Like an
 * articulation's component, a member may be past this model's components:
 * it indexes the game's per-component state, which the game may also draw
 * another model with. `name` follows the tag syntax. */
typedef struct AeronFlightComponentSet {
	char     name[AERON_FLIGHT_TAG_MAX];
	uint32_t members[8]; /* bit n % 32 of word n / 32: component n */
} AeronFlightComponentSet;

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

/* ---- probes -------------------------------------------------------------
 *
 * A model's baked environment probes (`probeData` on the model root): its
 * reflection probes (a cube array, probe k in layer k) and its irradiance
 * grid, in the model frame (aeron axes), metres. The same layout is the
 * probes bake's output (bake.json) and its job's input, as JSON with the
 * fields of `probeData` (AERON_flight_model README, "Cooked data"). */

#define AERON_FLIGHT_MAX_PROBES      42 /* 6 cube layers each within 256 array layers */
#define AERON_FLIGHT_PROBE_GRID_MIN  2  /* grid points per axis */
#define AERON_FLIGHT_PROBE_GRID_MAX  64
#define AERON_FLIGHT_KEY_LIGHT_SHADOW 0.7f

typedef enum AeronFlightProbeShape {
	AERON_FLIGHT_PROBE_BOX,
	AERON_FLIGHT_PROBE_ELLIPSOID,
} AeronFlightProbeShape;

/* A reflection probe. `center` is where its cube was captured and the
 * centre of its influence; `size` the influence's full extent (an
 * ellipsoid's axes). Its weight falls from 1 to 0 over the outer `falloff`
 * fraction of the influence. Reflections are projected onto the parallax
 * volume around `parallax_center` (several probes of a room can share the
 * room's box). */
typedef struct AeronFlightProbe {
	float                 center[3];
	float                 size[3];
	AeronFlightProbeShape shape;
	float                 falloff; /* (0, 1] */
	float                 parallax_center[3];
	float                 parallax_size[3];
	AeronFlightProbeShape parallax_shape;
} AeronFlightProbe;

typedef struct AeronFlightProbeLayout {
	float intensity; /* scales the probes' light; with the strengths, 1 by default */
	float diffuse_strength;
	float specular_strength;
	/* The grid: an axis-aligned box of resolution points, sampled as
	 * Blender's volume probe (point i at (i + 1) / (resolution + 1) of the
	 * box). */
	float            grid_center[3];
	float            grid_size[3];
	uint32_t         grid_resolution[3];
	AeronFlightProbe probes[AERON_FLIGHT_MAX_PROBES];
	uint32_t         probe_count; /* 1 to AERON_FLIGHT_MAX_PROBES */
	/* When the scene's key light is in the bake (a hangar's ceiling light),
	 * the part of the probe light its shadow removes, 0-1. */
	float key_light_shadow;
	/* The floor that reflects what stands on it (a hangar's): a horizontal
	 * rectangle facing up, its centre and size along x and y; surfaces
	 * within `distance` of it take its reflection. */
	bool  has_reflection_plane;
	float reflection_plane_center[3];
	float reflection_plane_size[2];
	float reflection_plane_distance;
} AeronFlightProbeLayout;

/* The layout and owned copies of its two KTX2 images: the BC6H (or RGBA16F)
 * cube array and the RGBA16F grid (point (x, y, z) at column 6 x + f, row
 * y + ry z, for f = +x, -x, +y, -y, +z, -z). */
typedef struct AeronFlightProbes {
	AeronFlightProbeLayout layout;
	uint8_t*               cubes;
	size_t                 cubes_size;
	uint8_t*               grid;
	size_t                 grid_size;
} AeronFlightProbes;

/* Parses a probe layout from a JSON object's text; unknown fields are
 * ignored. On failure returns false with the reason in `error`. */
bool Aeron_FlightProbeLayoutParse(const char* json, size_t size, AeronFlightProbeLayout* out, char* error,
								  size_t error_size);
/* Formats a layout as a JSON object; returns the full length like snprintf. */
size_t Aeron_FlightProbeLayoutFormat(const AeronFlightProbeLayout* layout, char* out, size_t size);
/* Builds probes from a layout's JSON text and the two KTX2 images (copied),
 * checking that the images hold the layout's probes and grid. */
bool Aeron_FlightProbesBuild(const char* layout_json, size_t layout_size, const uint8_t* cubes,
							 size_t cubes_size, const uint8_t* grid, size_t grid_size, AeronFlightProbes* out,
							 char* error, size_t error_size);
void Aeron_FlightProbesFree(AeronFlightProbes* probes);

typedef struct AeronFlightModel {
	AeronGltfModel         render;
	uint32_t               component_count;
	AeronFlightComponent*  components;
	uint32_t               engine_glow_count;
	AeronFlightEngineGlow* engine_glows;
	uint32_t               light_count;
	AeronFlightLight*      lights;
	AeronFlightBounds      bounds;
	float                  max_extent;
	int32_t                bridge_component;
	uint32_t                        articulation_count;
	AeronFlightArticulation*        articulations;
	uint32_t                        articulation_setting_count;
	AeronFlightArticulationSetting* articulation_settings;
	uint32_t                        component_set_count;
	AeronFlightComponentSet*        component_sets;
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
	/* The cooked probes (`probeData` on the model root), or NULL. Filled by
	 * the render-including builds; freed with the render data. */
	AeronFlightProbes*     probes;
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
 * hardpoints, tags and cooked simulation data, the model's articulations,
 * articulation settings and component sets, and the cooked render data.
 * What AeronScene_MeshCreate reads stays: render data, render BVH,
 * component mesh types, rotations and bounds, engine glows, lights and model
 * bounds.
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
