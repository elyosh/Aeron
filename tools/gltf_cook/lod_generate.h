#ifndef AERON_GLTF_COOK_LOD_GENERATE_H
#define AERON_GLTF_COOK_LOD_GENERATE_H

/* Generated levels of detail of a flight model. A level is a simpler set of
 * triangles over level 0's vertices, with the same classes. */

#include "aeron/scene/gltf_mesh.h"

#include <stdbool.h>
#include <stdint.h>

#define AERON_LOD_MAX_GENERATED 3

typedef struct AeronLodGenerateOptions {
	/* The target error of each level, as a fraction of the model's radius
	 * (half its bounding box diagonal). */
	float relative_errors[AERON_LOD_MAX_GENERATED];
	int   level_count;
	/* A level is kept only if it removes at least this share of the
	 * previous level's triangles. */
	float min_gain;
	/* Models with fewer triangles get no levels. */
	uint32_t min_triangles;
	/* Collapse edges across UV and normal seams. Vertices on a material
	 * border are protected, so a triangle never mixes two materials. */
	bool permissive;
	/* Keep the open borders of each component in place, so that components
	 * don't crack apart where they meet. */
	bool lock_component_borders;
	/* Let the simplifier remove small disconnected parts. */
	bool prune;
	/* The first step of the error ladder (0 for the first) that crosses
	 * material borders and doesn't lock component borders; level_count or
	 * more for none. Such a level is meant for models a few pixels across: a
	 * triangle over several materials takes the material of most of its
	 * corners, and its other corners become new vertices with that
	 * material and the UV of the nearest vertex that has it. */
	int coarse_from_level;
	/* On coarse levels, how much a change of a primitive's average colour
	 * or emitted light counts as error: a change of 1 (in linear colour,
	 * with emission x mapped to x / (1 + x)) counts as a distance of this
	 * fraction of the model's radius. 0 measures only the geometry. */
	float attribute_weight;
} AeronLodGenerateOptions;

typedef struct AeronLodLevel {
	/* The largest distance from this level's surface to level 0's, in model
	 * units, over all components. */
	float     error;
	uint32_t* indices;
	uint32_t  index_count;
	/* Index counts of the six classes, in order: opaque, its screen part,
	 * mask, its screen part, blend, its screen part. Each class's screen
	 * part ends it, as in AeronGltfModel. */
	uint32_t ranges[6];
	/* On a level that keeps material borders: triangles the simplifier made
	 * across two materials anyway, which the level leaves out. This should
	 * stay 0. */
	uint32_t dropped_triangles;
	/* The vertices a coarse level adds, numbered after level 0's vertices
	 * and those of the previous levels, and the level 0 vertex each one
	 * copies. */
	AeronGltfVertex* vertices;
	uint32_t*        vertex_sources;
	uint32_t         vertex_count;
} AeronLodLevel;

/* The default options: the plan's error ladder (0.5%, 2% and 8% of the
 * radius), a 25% minimum gain, 300 triangles, permissive collapses, locked
 * component borders, pruning, only the third level coarse, and an
 * attribute weight of 1. */
AeronLodGenerateOptions aeron_lod_default_options(void);

/* Generates up to options->level_count levels of `model`, coarsest last.
 * Returns the number of levels kept; free each with aeron_lod_level_free.
 * Returns -1 when out of memory. */
int aeron_lod_generate(const AeronGltfModel* model, const AeronLodGenerateOptions* options,
					   AeronLodLevel levels[AERON_LOD_MAX_GENERATED]);

void aeron_lod_level_free(AeronLodLevel* level);

#endif
