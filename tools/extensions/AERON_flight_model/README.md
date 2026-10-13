# AERON_flight_model

`AERON_flight_model` describes an Aeron flight asset: the simulation-facing
hierarchy of its nodes, what its materials animate or show live, and, in
cooked files, where its textures are. It extends nodes, materials and
textures. A source file lists it in `extensionsUsed` only, so that any glTF
viewer shows the model as a standard glTF. A cooked file, which
`aeron_gltf_cook` writes, lists it in `extensionsRequired` too: its textures
are found only through it.

Each object has its schema, with a description of every field:
[node](node.AERON_flight_model.schema.json),
[material](material.AERON_flight_model.schema.json) and
[texture](texture.AERON_flight_model.schema.json).

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
  `articulationSettings`, `componentSets`, `markings`, `energy`,
  `skinVariants` and `environmentProbe`
- `component`: requires `meshType` in `0..31`; optionally carries
  `explosionFlags`, `targetId`, `target`, descriptor geometry, `rotation`
  and `tags`
- `hardpoint`: requires raw hardpoint `type` in `0..39`
- `engineGlow`: requires `coreColor` and `outerColor`, RGBA as 8-bit sRGB
  values over 255 (the game draws the glow with them, alpha included);
  `enabled` defaults to true
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
the variant that markings value m shows (a `KHR_materials_variants` variant
or a skin variant, see Skins), or `null` for the primitives' own
materials. A value past the end shows the last entry; without the array,
every value shows the own materials. Every name must be one of the file's
variants.

```json
"markings": [null, "Red", "Blue"]
```

Here markings 0 shows the model as modelled, 1 shows `Red`, and 2 and above
`Blue`. A game may let a mission choose another variant, or `null`, for a
markings value. Aeron loads a texture that only variants use when a
markings value in use shows one of them.

## Environment probe

A hangar model's root may carry `environmentProbe`, the point from which
an authoring tool bakes the hangar's environment package (its lighting and
reflections). The game ignores it.

```json
"environmentProbe": { "position": [0.0, 1.2, -4.5], "samples": 256 }
```

- `position`: required, in the model's frame, in metres;
- `samples`: render samples per pixel.

## Skins

A skin is a set of images painted over materials. A material's
`AERON_flight_model` object may carry `skins`, keyed by skin name (1 to 63
of `a-z`, `0-9`, `.`, `_`, `-`); each entry is a layer of that skin on this
material:

- `baseColorTexture`: required. Its RGB is the layer's base colour and its
  alpha the layer's coverage, for every channel of the layer;
- `emissiveTexture`, `metallicRoughnessTexture`: optional. Without one, the
  layer leaves that channel as it is;
- `emissiveStrength`: optional, the strength of the layer's emissive.
  Default: the material's `KHR_materials_emissive_strength`, or 1.

A layer's textures are ordinary glTF textures of the file. They use the
texture coordinates of the material's base colour texture (its `texCoord`
and `KHR_texture_transform`); their own sampler is ignored. Each must have
the size of the material's own texture of the same channel; when the
material has none, all layers of that channel must have the same size.

The model root may carry `skinVariants`, an array of variants made of skins:

```json
"skinVariants": [
  { "name": "Red", "layers": [{ "skin": "red" }] },
  { "name": "Red, worn", "base": "State 1", "layers": [{ "skin": "red" }, { "skin": "dirt", "opacity": 60 }] }
]
```

- `name`: required, unique among the skin variants. The name of a
  `KHR_materials_variants` variant adds the layers to that variant;
- `base`: optional, another `KHR_materials_variants` variant the skin
  variant starts from;
- `layers`: the skins in order, each with an optional `opacity` (0 to 100,
  default 100) that scales its coverage. Every skin must be on at least one
  material. It may be empty or absent only with a `base` or the name of a
  `KHR_materials_variants` variant.

On a primitive whose material has at least one of the variant's skins, the
variant shows a composite of that material: each channel that a layer has
is the material's own image (or, without one, its factor as a uniform
image, the composite then using factor 1) with the layers painted over it
in order, as `dst · (1 − a) + layer · a` on the stored values, where `a` is
the coverage times the opacity. The composite keeps the material's alpha.
Emissive layers whose strength differs from the composite's (the
material's, else the first emissive layer's) are scaled in linear light
first. Any other primitive shows the material that the
`KHR_materials_variants` variant of the same name gives it, else the `base`
variant's, else its own. Layers always paint the primitive's own material,
not a variant's.

The cooker turns skins into `KHR_materials_variants`, with one material per
distinct (material, layers) composite. A skin variant with the same name as
a file variant changes that variant. The others are added after the file's
variants. A cooked file has no `skins` or
`skinVariants`. A viewer without this extension shows the materials
without skins.

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

## Materials

A material's `AERON_flight_model` object gives what the game draws on it
beyond its glTF definition. It may hold:

- `flipbooks`: frames the material plays on game states;
- `damage`: the hull damage it shows as the hull weakens;
- `energy`: the energy effects it shows, any of `shields`, `tractor` and
  `jamming` (`[]` for none). The model root's `energy` is the default of
  every material without its own; without either, a material shows none;
- `screen`: displays the game draws live on it (cockpit screens);
- `skins`: its skin layers (see Skins).

Frames and damage stages are image keys of effect libraries, files the
models share and the game loads from its `remaster/effects/`.
[animated_materials.md](../../gltf_cook/animated_materials.md) describes the
libraries, the game states and how a frame changes the material.

```json
"AERON_flight_model": {
  "flipbooks": [
    { "state": "always", "layer": "emissive", "mode": "loop", "library": "mycockpit",
      "frames": [["lights_0", 0.5], ["lights_1", 0.5]], "emissiveStrength": 2 }
  ],
  "energy": []
}
```

**Flipbooks** form an ordered array. Per layer, the first flipbook whose
state is active plays, so a base colour and an emissive flipbook can play
together. A flipbook has:

- `state`: `always`, or a game state;
- `layer`: `baseColor` (frames painted over the base colour) or `emissive`
  (frames replacing the emissive);
- `mode`: `loop`, or `once` (holds its last frame);
- `library`: the effect library of its frames, required with frames;
- `frames`: `[key, seconds]` pairs, images of the layer's kind;
- `emissiveStrength`: optional, default 1, the brightness of emissive
  frames;
- `unlit`: optional, 0 to 1, default 0: the part of base colour frames drawn
  unlit, as emission.

A flipbook plays from the moment its state becomes active.

**Hull damage** is `{ "library", "choice", "placement", "alternatives" }`.
Each object picks one of the `alternatives`, materials with the same
`choice` (0 to 4095) picking the same. An alternative has:

- `stages`: three image keys of kind `damage`, or `null`, shown at 75, 50
  and 25% hull;
- `scale`: optional `[min, max]`: the image covers a square whose side is
  random between them, as a fraction of the texture, centred; without it,
  the whole texture;
- `offset`: optional, moves that square: `"inside"` anywhere within the
  texture, or `{ "min": [x, y], "max": [x, y] }` by `min + (max − min)·r`
  per axis, `r` random in [0, 1). Without it, the square stays centred.

The random values are drawn once per object, shared by the materials with
the same `placement` (0 to 4095). Outside its square, a stage shows
nothing; the square is clipped at the texture's edges.

**Screens.** `screen` makes the material show displays the game draws live,
through rectangles of its `TEXCOORD_0` space:

- `cover`: the base colour's alpha below 1 shows the screen through the
  material;
- `transparent`: drawn unlit, its alpha from the screen's luminance;
- `alwaysVisible`: a transparent screen kept while the game draws its 2D
  HUD;
- `hologram`: drawn as a hologram; `noisy` adds scanlines;
- `hide`: required, a bitmask of the 2D HUD regions the game stops drawing
  while the screen shows (game-defined);
- `rects`: required, up to 16 rectangles.

The flags are booleans, false when absent. A rectangle has:

- `source`: the game's display it shows, 0 to 63 (game-defined);
- `dst`: `[u0, v0, u1, v1]` in `TEXCOORD_0`; swapped corners mirror it;
- `bg`, `highlight`, `lock`: linear RGB backgrounds in the display's normal,
  highlighted and locked states;
- `gain`: >= 0, the content's brightness;
- `layers`: a bitmask, 1 graphics, 2 text, 4 a fill bar (no texture);
- `bar`: the linear RGB of the fill bar, required with layer 4.

## Textures and images

In a cooked file, each glTF image is a KTX2 array of atlas pages in a
buffer view, for one channel (base colour, normal, metallic-roughness or
emissive: the material slot that references it). A texture names its image
and page through its `AERON_flight_model` object, and has no `source` of
its own:

- `source`: the image;
- `layer`: the page of it, 0 to 4095;
- `maxLod`: optional, the mip levels the texture may sample (0 to 14): a
  texture packed on a shared page stops there, before its neighbours bleed
  into it. Without it, no limit.

`KHR_texture_transform` on the material's texture info places the texture
within its page.

A model loads an image when a primitive's own material references it, or
when a material variant it shows does (through `KHR_materials_variants`,
for the markings values in use). In a channel no primitive's own material
uses, every image is loaded. The cooker packs the pages that only some
variants use into images of their own, one per set of variants, so that a
model loads only the skins it shows.

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

Unknown fields are ignored. Unknown roles are invalid. Nothing is read from
`extras`.
