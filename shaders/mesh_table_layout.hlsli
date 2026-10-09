#ifndef AERON_SCENE_MESH_TABLE_LAYOUT_H
#define AERON_SCENE_MESH_TABLE_LAYOUT_H

/*
 * Mesh-table layout shared between C (AeronSceneMeshTable, via
 * aeron/scene/mesh_common.h) and the shaders reading the mesh_tables
 * StructuredBuffer<float4>. Preprocessor arithmetic only. Lives under
 * shaders/ because shadercross accepts a single -I directory; the C
 * side includes it by relative path.
 *
 * AERON_MAX_MESH_COMPONENTS is the per-model component capacity: the 255
 * meshes the XWAU hooks configure, against the original engine's 50
 * (XWA_SNAP_MAX_MESH_SLOTS).
 *
 * A table holds `n` entries (its model's component count) and is packed at
 * that count, in float4 units from the table's base:
 *   - rows:  3 per component c, its 3x4 affine, at base + 3 * c;
 *   - lanes: visibility, highlight, markings, emissive, each
 *     AERON_MESH_LANE_VEC4(n) float4 holding 4 components per float4, at
 *     base + 3 * n + lane * AERON_MESH_LANE_VEC4(n) + (c >> 2),
 *     element (c & 3).
 */

#define AERON_MAX_MESH_COMPONENTS    255
#define AERON_MESH_PACKED_LANES      ((AERON_MAX_MESH_COMPONENTS + 3) / 4)

#define AERON_MESH_LANE_VISIBILITY   0
#define AERON_MESH_LANE_HIGHLIGHT    1
#define AERON_MESH_LANE_MARKINGS     2
#define AERON_MESH_LANE_EMISSIVE     3
#define AERON_MESH_LANE_COUNT        4

#define AERON_MESH_LANE_VEC4(n)      (((n) + 3) / 4)
#define AERON_MESH_TABLE_VEC4(n)     (3 * (n) + AERON_MESH_LANE_COUNT * AERON_MESH_LANE_VEC4(n))

#endif /* AERON_SCENE_MESH_TABLE_LAYOUT_H */
