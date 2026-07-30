#ifndef AERON_SCENE_MESH_TABLE_LAYOUT_H
#define AERON_SCENE_MESH_TABLE_LAYOUT_H

/*
 * Mesh-table layout shared between C (AeronSceneMeshTable, via
 * aeron/scene/mesh_common.h) and the shaders reading the mesh_tables
 * StructuredBuffer<float4>. Preprocessor arithmetic only. Lives under
 * shaders/ because shadercross accepts a single -I directory; the C
 * side includes it by relative path.
 *
 * AERON_MAX_MESH_SLOTS matches the original engine's per-craft
 * componentState / meshRotation array size (XWA_SNAP_MAX_MESH_SLOTS).
 *
 * Offsets are in float4 units: per-slot 3x4 affine rows, then packed
 * visibility / highlight / markings / emissive lanes holding 4 slots
 * per float4 (lane[slot >> 2][slot & 3]).
 */

#define AERON_MAX_MESH_SLOTS         50
#define AERON_MESH_PACKED_LANES      ((AERON_MAX_MESH_SLOTS + 3) / 4)

#define AERON_MESH_TABLE_ROWS_VEC4   (AERON_MAX_MESH_SLOTS * 3)
#define AERON_MESH_VISIBILITY_OFFSET (AERON_MESH_TABLE_ROWS_VEC4)
#define AERON_MESH_HIGHLIGHT_OFFSET  (AERON_MESH_VISIBILITY_OFFSET + AERON_MESH_PACKED_LANES)
#define AERON_MESH_MARKINGS_OFFSET   (AERON_MESH_HIGHLIGHT_OFFSET + AERON_MESH_PACKED_LANES)
#define AERON_MESH_EMISSIVE_OFFSET   (AERON_MESH_MARKINGS_OFFSET + AERON_MESH_PACKED_LANES)
#define AERON_MESH_TABLE_STRIDE_VEC4 (AERON_MESH_EMISSIVE_OFFSET + AERON_MESH_PACKED_LANES)

#endif /* AERON_SCENE_MESH_TABLE_LAYOUT_H */
