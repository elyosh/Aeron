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
The four fields must be present together. They preserve authored simulation
metadata independently from bounds calculated from rendered vertices. Assets
that omit them remain valid and use calculated geometry as a compatibility
fallback. That fallback is computed from the simulation topology: the
collision child's mesh when present, otherwise the render mesh.

Simulation topology must keep at least one non-degenerate triangle per
component.

## Component tags

A component may carry `tags`, an array of game-defined strings. Each tag is 1
to 63 characters of lower-case ASCII letters, digits, `.` and `_`; malformed
tags are invalid and duplicates within one component are ignored. A tag binds
a component to a role that game code identifies by name instead of by
ordinal, for example `crew.side`. Aeron stores tags without interpreting them;
each game documents the tags it reads and which must be unique in a model.
Games ignore tags they do not read.

Unknown fields are ignored. Unknown roles are invalid. Runtime semantics are
not read from node `extras`.

The normative field constraints are in [schema.json](schema.json).
