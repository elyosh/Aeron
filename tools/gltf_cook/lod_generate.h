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
	/* Collapse edges across normal seams. Vertices on a material border or
	 * a UV seam are protected, so a triangle never mixes two materials or
	 * two UV mappings. */
	bool permissive;
	/* Keep the open borders of each component in place, so that components
	 * don't crack apart where they meet. */
	bool lock_component_borders;
	/* Let the simplifier remove small disconnected parts. */
	bool prune;
	/* The first step of the error ladder (0 for the first) that crosses
	 * material borders and UV seams and doesn't lock component borders;
	 * level_count or more for none. Such a level is meant for models a few
	 * pixels across. Each of its triangles gets three vertices of its own,
	 * with the material of most of its corners, its own normal, and the UVs
	 * of the nearest original triangle of that material, extended to its
	 * corners. */
	int coarse_from_level;
	/* On coarse levels, how much a change of a primitive's average colour
	 * or emitted light counts as error: a change of 1 (in linear colour,
	 * with emission x mapped to x / (1 + x)) counts as a distance of this
	 * fraction of the model's radius. 0 measures only the geometry. */
	float attribute_weight;
	/* On coarse levels, components at least this many times the target
	 * error across (their bounding box's longest side) keep their open
	 * borders, so that large parts such as a TIE fighter's wing panels don't
	 * pull apart. 0 frees every border. */
	float lock_border_size;
	/* On coarse levels, a component that keeps less than this share of its
	 * surface area is simplified again at half the error, up to 4 times, so
	 * that tubes and boxes don't flatten into strips. 0 for no check. */
	float min_area_share;
	/* On coarse levels, the brightest emitting parts are simplified on their
	 * own, so that engines and lights smaller than the error don't vanish.
	 * They are taken by emitted light, brightest first, until they hold
	 * emitter_share of the model's light or there are emitter_count of
	 * them (at most 255). Each keeps at least emitter_min_triangles, or all
	 * of them when it has fewer. 0 for emitter_count turns this off. */
	float emitter_share;
	int   emitter_count;
	int   emitter_min_triangles;
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
	/* The vertices a coarse level adds, three for each of its triangles,
	 * numbered after level 0's vertices and those of the previous levels,
	 * and the level 0 vertex whose position each one has. */
	AeronGltfVertex* vertices;
	uint32_t*        vertex_sources;
	uint32_t         vertex_count;
} AeronLodLevel;

/* The default options: the plan's error ladder (0.5%, 2% and 8% of the
 * radius), a 20% minimum gain, 300 triangles, permissive collapses, locked
 * component borders, pruning, only the third level coarse, an attribute
 * weight of 4, locked borders on components 8 times the error across, a 50%
 * minimum area, and up to 64 emitters for 90% of the light, with at least
 * 4 triangles each. */
AeronLodGenerateOptions aeron_lod_default_options(void);

/* Generates up to options->level_count levels of `model`, coarsest last.
 * Returns the number of levels kept; free each with aeron_lod_level_free.
 * Returns -1 when out of memory. */
int aeron_lod_generate(const AeronGltfModel* model, const AeronLodGenerateOptions* options,
					   AeronLodLevel levels[AERON_LOD_MAX_GENERATED]);

void aeron_lod_level_free(AeronLodLevel* level);

/* How a level compares with level 0, for aeron_lod_report. */
typedef struct AeronLodMeasure {
	/* The level's emitted light (area times the luminance of the emitted
	 * light, sampled across each triangle) as a share of level 0's; -1 when
	 * level 0 emits nothing. */
	float light;
	/* The share of level 0's covered area that the level leaves open, over
	 * 13 orthographic views. Pixels closer than the level's error to level
	 * 0's outline don't count, since the outline may move that much. */
	float holes;
} AeronLodMeasure;

/* Measures `count` levels generated for `model`. False when out of memory. */
bool aeron_lod_measure(const AeronGltfModel* model, const AeronLodLevel* levels, int count,
					   AeronLodMeasure* out);

#endif
