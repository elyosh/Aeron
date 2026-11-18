#ifndef AERON_ASSET_PLAIN_GLTF_MODEL_H
#define AERON_ASSET_PLAIN_GLTF_MODEL_H

/*
 * Builds a flight model from a plain glTF 2.0 binary file: one that isn't
 * written for the game, such as the model of a VR controller that the
 * headset's runtime gives.
 *
 * The game's loader wants a model in the AERON_flight_model layout: one root
 * node and its parts under it. The file is rewritten in that layout in
 * memory: each node that has a mesh becomes a part, placed where the node is
 * at rest, and the rest of the node tree is dropped from the file. The
 * result is cooked and loaded as any other model.
 *
 * The node tree is kept aside (AeronPlainGltfNodes), so that the parts can
 * follow nodes that something moves afterwards: AeronPlainGltf_Pose fills a
 * mesh table from the nodes' poses.
 *
 * Limits: the file must be a GLB with one buffer and embedded PNG or JPEG
 * images, no required glTF extension, and no node with a mesh under a scale
 * that isn't uniform and positive. Skins, animations, morph targets and
 * material extensions are dropped. A material that gives no metallic value
 * and no metallic texture is taken as not metal, against the glTF default:
 * files converted from formats without such a value leave it out.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/asset/flight_model.h"
#include "aeron/scene/scene3d.h"
#include "aeron/scene/view_rig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AERON_PLAIN_GLTF_MAX_NODES 256
#define AERON_PLAIN_GLTF_NAME_MAX 64

typedef struct AeronPlainGltfNode {
	char name[AERON_PLAIN_GLTF_NAME_MAX];
	int  parent; /* an index in the nodes, or -1 */
	/* The node's transform at rest, from its space to its parent's: a 3x4
	 * matrix, row-major, in the file's axes and metres, and the uniform
	 * scale that it holds. */
	float local[12];
	float scale;
	/* The mesh table row of the node's mesh, or -1. */
	int component;
} AeronPlainGltfNode;

typedef struct AeronPlainGltfNodes {
	int                count;
	int                component_count;
	AeronPlainGltfNode nodes[AERON_PLAIN_GLTF_MAX_NODES];
	/* Each node's transform at rest from its space to the model's, inverted. */
	float rest_inverse[AERON_PLAIN_GLTF_MAX_NODES][12];
} AeronPlainGltfNodes;

/* Builds `out` from the bytes of a GLB file, and fills `nodes`. `label`
 * names the model in messages. Returns false with a message in `error` when
 * the file can't be used. Free the model with Aeron_FlightModelFree. */
bool Aeron_FlightModelBuildPlainGlb(const uint8_t* glb, size_t size, const char* label, AeronFlightModel* out,
									AeronPlainGltfNodes* nodes, char* error, size_t error_size);

/* The index of the node named `name`, or -1. */
int AeronPlainGltf_FindNode(const AeronPlainGltfNodes* nodes, const char* name);

/* Fills the mesh table of the model for nodes that moved. `poses[i]`, when
 * it isn't NULL, replaces the place and the rotation of node i in its
 * parent's space; the node keeps its scale. `hidden[i]` hides node i and
 * the nodes under it. Both arrays have `nodes->count` entries, and either
 * can be NULL. */
void AeronPlainGltf_Pose(const AeronPlainGltfNodes* nodes, const AeronViewPose* const* poses,
						 const uint8_t* hidden, AeronSceneMeshTable* table);

#ifdef __cplusplus
}
#endif

#endif
