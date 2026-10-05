# AERON_flight_model

`AERON_flight_model` describes the simulation-facing hierarchy of an Aeron
flight asset. It is a node extension listed in `extensionsUsed` and is not
listed in `extensionsRequired`.

The default scene contains one model-role root. Its ordered direct children
are component-role mesh nodes. A component's ordered children are hardpoint-
or engine-glow-role empty nodes, plus at most one collision-role mesh node.
Child order defines component, hardpoint and flattened engine-glow ordinals;
collision nodes do not take part in any ordinal.

Files use standard glTF right-handed coordinates and meters. Omitted TRS
members use the glTF defaults: zero translation, identity rotation and unit
scale. Model and component transforms are TRS with positive uniform scale.
Hardpoints permit translation only; explicitly serialized rotation and scale
must be identity. Engine glows permit translation, quaternion rotation and
scale: local +X is right, +Y is up and +Z is look; X/Y scale are positive full
sizes and Z is signed depth.

## Roles

- `model`: `{ "role": "model" }`
- `component`: requires `meshType` in `0..31`; optionally carries
  `explosionFlags`, `targetId`, `target`, descriptor geometry, and `rotation`
- `hardpoint`: requires raw hardpoint `type` in `0..39`
- `engineGlow`: requires `coreColor` and `outerColor`; `enabled` defaults to
  true
- `collision`: `{ "role": "collision" }` on a mesh node that is a direct child
  of a component. At most one per component. It must have a triangle mesh, no
  children, no skin, no morph targets and a TRS transform with positive
  uniform scale. When present, its mesh is the component's simulation
  topology (collision) instead of the component's render mesh. It is never
  rendered, and its materials are ignored.

Component `rotation` requires `pivot`, `rotationAxis`, `directionAxis`, and
`upAxis`. These vectors are retained independently and are not normalized or
orthogonalized. `target` is required when `targetId` is nonzero.

Descriptor geometry consists of `span`, `center`, `boundsMin`, and `boundsMax`.
The four fields must be present together. `span` is kept signed as authored,
because stock OPT descriptors include negative span components that the games
read as stored; under a rotated node transform its components combine through
the absolute rotation coefficients. They preserve authored simulation
metadata independently from bounds calculated from rendered vertices. Assets
that omit them remain valid and use calculated geometry as a compatibility
fallback. That fallback is computed from the simulation topology: the
collision child's mesh when present, otherwise the render mesh.

A component whose simulation triangles are all degenerate has no collision
faces: nothing collides with it, and its positions still give its bounds.
Such a component carries no `simulationData`.

## Component tags

A component may carry `tags`, an array of game-defined strings. Each tag is 1
to 63 characters of lower-case ASCII letters, digits, `.` and `_`; malformed
tags are invalid and duplicates within one component are ignored. A tag binds
a component to a role that game code identifies by name instead of by
ordinal, for example `crew.side`. Aeron stores tags without interpreting them;
each game documents the tags it reads and which must be unique in a model.
Games ignore tags they do not read.

## Cooked data

`aeron_gltf_cook` adds precomputed acceleration data. Loaders verify it in
linear time and build it themselves, with a warning, when it is absent.
Malformed or stale data fails the load.

- Component `simulationData`: `{ "version": 1, "bufferView": N,
  "topologyHash": "<16 lower-case hex digits>" }`. It covers the
  component's simulation topology (the collision child's mesh, otherwise the
  render mesh) as the loader builds it: positions in metres in the Aeron
  frame, degenerate triangles dropped, triangle order preserved.
- Model-root `renderData`: `{ "version": 1, "bufferView": N, "meshHash":
  "<16 lower-case hex digits>" }`. It covers the merged render triangles in
  final index order (opaque, mask, blend).

`topologyHash` is XXH64 (seed 0) of these little-endian values:
`position_count` (u32), every position (3 x f32), `triangle_count` (u32),
then every triangle's three position indices (3 x u32). `meshHash` hashes
the render vertex positions the same way, with the index count (u32) in place
of the triangle count.

The buffer view holds, little-endian and exactly this size:

```text
u32 version = 1, u32 face_count, u32 unique_vertex_count, u32 node_count, u32 sample_count
node[node_count]: f32 min[3], f32 max[3], f32 max_inv_dominant, u32 first_or_child, u32 count
u32 face_order[face_count]
u32 sample[sample_count]
```

Nodes form a bounding volume hierarchy over the triangles. Node 0 is the
root. An internal node (`count == 0`) has its children at `first_or_child`
and `first_or_child + 1`, both after it. A leaf lists `count` triangles of
`face_order` starting at `first_or_child`. Bounds are the raw triangle
bounds. `max_inv_dominant` is the largest `1 / max(|nx|, |ny|, |nz|)` of the
node's triangles: games expand nodes by their own plane tolerance times this
value. Render data stores 0 there, `sample_count = 0`, and the render vertex
count as `unique_vertex_count`.

For simulation data, unique vertices are the topology positions
deduplicated by bitwise equality, first occurrence kept. The sample is up to
256 unique-vertex indices in farthest-point order: the seed is the
lexicographically smallest position, and each step takes the largest minimum
squared distance, lowest index on ties. A game needing fewer samples takes a
prefix.

Unknown fields are ignored. Unknown roles are invalid. Runtime semantics are
not read from node `extras`.

The normative field constraints are in [schema.json](schema.json).
