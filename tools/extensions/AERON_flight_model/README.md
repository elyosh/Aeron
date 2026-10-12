# AERON_flight_model

`AERON_flight_model` describes the simulation-facing hierarchy of an Aeron
flight asset. It is a node extension listed in `extensionsUsed` and is not
listed in `extensionsRequired`.

The default scene contains one model-role root. Its ordered direct children
are 1 to 255 component-role mesh nodes. A component's ordered children are hardpoint-,
engine-glow- or light-role empty nodes, plus at most one collision-role mesh node.
Child order defines component, hardpoint and flattened engine-glow ordinals;
collision and light nodes do not take part in any ordinal.

Files use standard glTF right-handed coordinates and meters. Omitted TRS
members use the glTF defaults: zero translation, identity rotation and unit
scale. Model and component transforms are TRS with positive uniform scale.
Hardpoints permit translation only; explicitly serialized rotation and scale
must be identity. Engine glows permit translation, quaternion rotation and
scale: local +X is right, +Y is up and +Z is look; X/Y scale are positive full
sizes and Z is signed depth. Lights permit translation, quaternion rotation
and positive uniform scale.

## Roles

- `model`: `{ "role": "model" }`; optionally carries `articulations`,
  `articulationSettings`, `componentSets` and `markings`
- `component`: requires `meshType` in `0..31`; optionally carries
  `explosionFlags`, `targetId`, `target`, descriptor geometry, `rotation`
  and `tags`
- `hardpoint`: requires raw hardpoint `type` in `0..39`
- `engineGlow`: requires `coreColor` and `outerColor`; `enabled` defaults to
  true
- `collision`: `{ "role": "collision" }` on a mesh node that is a direct child
  of a component. At most one per component. It must have a triangle mesh, no
  children, no skin, no morph targets and a TRS transform with positive
  uniform scale. When present, its mesh is the component's simulation
  topology (collision) instead of the component's render mesh. It is never
  rendered, and its materials are ignored.
- `light`: on an empty node with a `KHR_lights_punctual` light, a direct
  child of a component; see [Lights](#lights)

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
ordinal, for example `hangarcrane.arm`. Aeron stores tags without interpreting them;
each game documents the tags it reads and which must be unique in a model.
Games ignore tags they do not read.

## Articulations

The model root may carry `articulations`, an array of objects that let a
game turn components about their `rotation` hinge:

- `component`: required integer in `0..254`, the component ordinal. It
  indexes the game's per-component state, so it may be past this model's
  components when the game draws another model, such as a cockpit, with the
  same state;
- `group`: required, a game-defined name with the tag syntax;
- `angle`: required integer in `-128..255`, in 1/256 turn;
- `speed`: required integer in `0..255`, in 1/256 turn per game step;
- `speedBack`: optional integer in `0..255`, defaults to `speed`;
- `behavior`: optional game-defined name with the tag syntax.

The model root may also carry `articulationSettings`, an object whose keys are
game-defined names with the tag syntax and whose values are integers. Aeron
stores articulations and settings without interpreting them; each game
documents the groups, behaviors and settings it reads, and ignores the
others.

## Component sets

The model root may carry `componentSets`, an object whose keys are
game-defined names with the tag syntax and whose values are arrays of
component ordinals in `0..254`; repeats are ignored. Like an articulation's
`component`, a member may be past this model's components, so a set can name
components of another model the game draws with the same per-component
state. Aeron stores sets without interpreting them.

## Markings

The game gives each object a markings value (0 to 255). The model root may
carry `markings`, an array of at most 256 entries: entry m is the name of
the `KHR_materials_variants` variant that markings value m shows, or `null`
for the primitives' own materials. A value past the end shows the last
entry; without the array, every value shows the own materials. Every name
must be one of the file's variants.

```json
"markings": [null, "Red", "Blue"]
```

Here markings 0 shows the model as modelled, 1 shows `Red`, and 2 and above
`Blue`. A game may let a mission choose another variant, or `null`, for a
markings value. Aeron loads a texture that only variants use when a
markings value in use shows one of them.

## Lights

A light-role node carries a `KHR_lights_punctual` point light, which gives
its colour, its intensity in candela and its range in metres (no range: the
game ends the light where it becomes negligible). Spot and directional
lights are ignored with a warning. The node's `AERON_flight_model` object
gives the shape:

- `shape`: `point` (default), `tube` or `panel`;
- `length`: required for a tube, its length in metres along the node's
  local +X;
- `size`: required for a panel, `[x, y]` in metres along the node's local X
  and Y;
- `oneSided`: optional for a tube (default false); a panel is always
  one-sided. A one-sided light emits only toward the node's local -Z, as a
  glTF spot light points, with a Lambertian (cosine) falloff;
- `radius`: optional for a point or a tube, its sphere radius or thickness
  in metres (default 0.1).

Fields that do not apply to the shape are invalid. Lengths take the node's
scale and its parents'. The intensity is the light straight ahead for a
one-sided light; a tube emits the total of a point light of its intensity,
spread along its length. A light lights the scene while its component is
shown, and follows the component's articulation. Games map candela to their
own lighting units.

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
