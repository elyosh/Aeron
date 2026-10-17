# AERON_flight_model

`AERON_flight_model` describes an Aeron flight asset as the artist authors
it: the node hierarchy that the simulation uses, and what the materials
animate or display live. It extends nodes and materials. Files list it in
`extensionsUsed` only, so any glTF viewer can show the model as standard
glTF. The data that the cooker adds for the game is described in
[AERON_flight_model_cooked](../AERON_flight_model_cooked/README.md) (see
[Cooked files](#cooked-files)).

Each object has a schema that describes every field:
[node](node.AERON_flight_model.schema.json) and
[material](material.AERON_flight_model.schema.json).

The default scene has one root node with the `model` role. Its direct
children, in order, are 1 to 255 mesh nodes with the `component` role. A
component's children, in order, are empty nodes with the `hardpoint`,
`engineGlow` or `light` role, plus at most one mesh node with the
`collision` role. The order of the children gives the component numbers,
the hardpoint numbers (per component) and the engine glow numbers (across
the model). Collision and light nodes are not numbered.

Files use standard glTF coordinates (right-handed, +Y up) and metres. An
omitted translation, rotation or scale takes the glTF default. Node
transforms:

- model and component: translation, rotation and scale (not a matrix), with
  a positive uniform scale;
- hardpoint: translation only. A rotation or scale written in the file must
  be the identity;
- engine glow: translation, rotation and scale. Local +X is right, +Y is up
  and +Z is the direction the glow looks. The X and Y scale are its full
  width and height, and must be positive. The Z scale is its depth and can
  be negative;
- light: translation, rotation and a positive uniform scale.

## Roles

- `model`: `{ "role": "model" }`. Optional fields: `articulations`,
  `articulationSettings`, `componentSets`, `markings`, `energy`,
  `skinVariants`, `reflectionProbes`, `lightGrid` and `reflectionPlane`.
- `component`: requires `meshType`, 0 to 31. Optional fields:
  `explosionFlags`, `targetId`, `target`, the descriptor geometry,
  `rotation` and `tags`.
- `hardpoint`: requires `type`, the game's hardpoint type, 0 to 39.
- `engineGlow`: requires `coreColor` and `outerColor`, RGBA as 8-bit sRGB
  values divided by 255. The game draws the glow with them, alpha
  included. `enabled` is optional and defaults to true.
- `collision`: `{ "role": "collision" }` on a mesh node that is a direct
  child of a component, at most one per component. Its mesh is made of
  triangles. The node has no children, no skin and no morph targets, and a
  translation, rotation and positive uniform scale. Its mesh replaces the
  render mesh as the component's simulation topology (collision). It is
  never rendered, and its materials are ignored.
- `light`: on an empty node with a `KHR_lights_punctual` light, a direct
  child of a component. See [Lights](#lights).

A component's `rotation` needs `pivot`, `rotationAxis`, `directionAxis` and
`upAxis`. The vectors are kept as written: they are not normalized or made
orthogonal. `target` is required when `targetId` is not 0.

The descriptor geometry is `span`, `center`, `boundsMin` and `boundsMax`:
the part's size, centre and bounds as the original model gives them. The
four fields go together. `span` keeps its sign, because stock OPT
descriptors have negative span values that the games read as they are.
When the node is rotated, the span is transformed with the absolute values
of the rotation's entries. The descriptor is kept separate from the bounds
of the rendered vertices. Without it, the loader computes it from the
simulation topology: the collision child's mesh when there is one,
otherwise the render mesh.

A component whose simulation triangles all have zero area has no collision
faces: nothing collides with it, but its positions still give its bounds.
Such a component has no cooked `simulationData`.

## Component tags

A component may carry `tags`, an array of game-defined names. A tag is 1 to
63 lower-case ASCII letters, digits, `.` and `_`. A malformed tag is
invalid, and a tag repeated in one component counts once. A tag gives a
component a role that the game finds by name instead of by number, such as
`hangarcrane.arm`. Aeron stores tags as they are. Each game documents the
tags it reads, and which ones must be unique in a model. Games ignore the
tags they don't read.

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

## Probes

The model root may carry reflection probes and a light grid. An authoring
tool uses them to bake the model's probes, which hold the light and
reflections of a hangar or of a capital ship's bays. The game uses the
probes to light the model and the objects inside it. The cooker stores the
bake in the cooked file as `probeData`
([AERON_flight_model_cooked](../AERON_flight_model_cooked/README.md)), and
the game reads that. Cooked files do not contain `reflectionProbes` or
`lightGrid`.

The model root may also carry a reflection plane, a floor that reflects
what stands on it. Cooked files keep it, and the game reads it from there.
The game uses it only with the model's probes, and ignores it in a model
without probes.

```json
"reflectionProbes": [
  { "position": [0.0, 1.2, -4.5], "size": [20.0, 8.0, 30.0], "shape": "box", "falloff": 0.2,
    "roomParallax": true, "filled": true, "clipStart": 0.8, "clipEnd": 40.0 }
],
"lightGrid": { "position": [0.0, 4.0, -10.0], "size": [40.0, 12.0, 60.0], "spacing": 4.0,
               "resolution": [9, 2, 14], "keyLightShadow": 0.7 },
"reflectionPlane": { "position": [0.0, 0.0, -10.0], "size": [40.0, 60.0], "distance": 0.1 }
```

Positions and sizes are in the model's frame, in metres, and boxes are
aligned with its axes.

A reflection probe has:

- `position` and `size`: the centre and full extent of its influence (an
  ellipsoid's axes);
- `shape`: `box` or `ellipsoid`;
- `falloff`: the outer fraction of the influence over which its weight
  falls to 0;
- `parallaxSize` and `parallaxShape`: optional, the volume its reflections
  are projected on. Default: the influence;
- `roomParallax`: optional. Its reflections are projected on the light
  grid's box instead, which several probes of one room can share;
- `clipStart` and `clipEnd`: the near and far distances of its render;
- `filled`: optional, set on probes that the authoring tool's fill made.
  The next fill replaces them.

The light grid has:

- `position` and `size`: the centre and full extent of the box it lights
  fully. Its light fades out within one point spacing outside the box;
- `spacing`: the approximate distance between its points;
- `resolution`: the number of points along each axis, which follows from
  `size` and `spacing`. Point i is at (i + 1) / (resolution + 1) of the box;
- `keyLightShadow`: optional, 0 to 1, default 0.7. When the scene's key
  light, such as a hangar's ceiling light, is in the bake, its shadow
  removes this share of the probes' light.

The reflection plane is a hangar's floor. It faces +y and has:

- `position`: its centre;
- `size`: its full extent along x and z;
- `distance`: optional, 0 to 10, default 0.1. Surfaces of the model within
  this distance of the plane show the reflection.

The game shows in the plane the objects inside the probes' grid.

Lights inside the grid's box are baked into the probes and have `baked`
set (see [Lights](#lights)).

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
variant shows a composite of that material. For each channel that a layer
has, the composite starts from the material's own image. When the material
has no image for that channel, it starts from the material's factor as a
uniform image, and the composite uses a factor of 1. The layers are painted
over it in order, as `dst · (1 − a) + layer · a` on the stored values, where
`a` is the coverage times the opacity. The composite keeps the material's
alpha. The composite's emissive strength is the material's, or else the
first emissive layer's. An emissive layer with another strength is scaled
in linear light before it is painted.

On any other primitive, the variant shows the material that the
`KHR_materials_variants` variant of the same name gives it, else the
`base` variant's material, else its own. Layers always paint the
primitive's own material, not a variant's.

The cooker turns skins into `KHR_materials_variants`, with one material per
distinct composite of a material and its layers. A skin variant with the
same name as a file variant changes that variant. The others are added
after the file's variants. A cooked file has no `skins` or `skinVariants`.
A viewer without this extension shows the materials without skins.

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
  in metres (default 0.1);
- `baked`: optional, default false. True when the light is baked into the
  model's probes. The authoring tool sets it on the lights inside the
  model's light grid. Games skip the light while those probes light the
  scene, so it is not counted twice.

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

## Cooked files

`aeron_gltf_cook` turns a source file into a cooked file for the game. The
source-only fields (`skins`, `skinVariants`, `reflectionProbes`,
`lightGrid`) are not copied to the cooked file. The
cooker adds the data it computes: texture atlas pages, simulation data,
render data and probe data. That data is described in
[AERON_flight_model_cooked](../AERON_flight_model_cooked/README.md).

Unknown fields are ignored. Unknown roles are invalid. Nothing is read from
`extras`.
