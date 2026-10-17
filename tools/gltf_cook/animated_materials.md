# Animated materials

A model's materials can play animated frames: flipbooks driven by game
states, hull damage and energy effects. Two places describe them:

- an **effect library** (`<library>.yaml`): a set of frames, shared by any
  models. `aeron_gltf_cook --library` builds it.
- the **model's materials**: each material's `AERON_flight_model` object
  says which frames of which libraries it plays, and when
  ([the extension](../extensions/AERON_flight_model/README.md#materials)).

```sh
aeron_gltf_cook --library mylibrary.yaml --out <mod folder>/flight/effects
aeron_gltf_cook model.gltf --out <mod folder>/flight/models/model.glb \
    --effects-dir <mod folder>/flight/effects
```

Build the libraries first: with `--effects-dir`, the cooker checks that
every frame a model names is in its library there, as an image of the
right kind. With `--project <dir>` as well, the cook first builds each
library the model names from `<dir>/effects/<library>/<library>.yaml`, when
that file exists: a project folder holds models (`models/<key>/`) and
their libraries (`effects/<library>/`).

[`example/`](example) is a working model with its library and animations.

## Flow

1. **Model.** Export glTF with ordinary PBR materials. Give each animated
   surface its own material and UV island: frames cover the material's whole
   UV space.
2. **Frames.** Paint each frame's images as sRGB PNGs with the material
   texture's UV layout.
3. **Effect library.** List the frames and their images.
4. **Model animations.** Say which frames each material plays, in its
   `AERON_flight_model` object (the Blender add-on edits them).
5. **Build** with the commands above. The game loads models from
   `flight/models/` in its mod folder and the libraries they name from
   `flight/effects/`.

The material's own textures show when no flipbook plays, and in any glTF
viewer: make them the animation's rest look.

## Effect libraries

```yaml
version: 1
library: mycockpit
images:
  lights_0: { kind: emissive, emissive: frames/lights_0.png }
  lights_1: { kind: emissive, emissive: frames/lights_1.png }
  screen_a: { kind: baseColor, baseColor: frames/screen_a.png }
  dmg_75:   { kind: damage, factor: frames/dmg_75.png }
```

- `library` is the library's name: `a-z`, `0-9`, `_` and `-`.
- Image keys are free names, unique within the library.
- Paths are relative to the file.

Each image changes one or both of the material's PBR channels, and its
inputs are named after them. All are sRGB PNGs in the material texture's UV
layout.

| Kind | Inputs | What it does |
|-|-|-|
| `baseColor` | `baseColor` (RGBA) | Paints over the material's base colour, alpha being the coverage |
| `emissive` | `emissive` (RGB); optional `baseColor` (RGBA) | Replaces the material's emissive; with `baseColor`, also paints over its base colour |
| `damage` | `factor` (RGB; alpha: gloss) | Hull damage: multiplies the base colour; alpha below 1 dulls the surface |
| `mark` | `baseColor` (RGBA) | Blast marks, painted over the hull |
| `energy` | `emissive` (RGBA; alpha: intensity) | Energy effects (shields, tractor, jamming), added as emission |

`mark` and `energy` images are the game's own effects, which
`flight/effects/effects.yaml` in the mod folder names; materials do not
reference them.

All inputs of one image have the same size. Images are capped at 1024
texels on their longer side, or `maxSize`. `crop: [x0, y0, x1, y1]` (blast
marks) records the part of the image that the classic texture's `[0, 1]`
maps to.

`--library` writes `<library>.yaml` (the index: each image's kind and
place), and `<library>.layer.ktx2` and `<library>.emissive.ktx2` (the
frames, as needed). The index records a hash of the inputs (the library
file, its images and the baking version, `EFFECT_LIBRARY_BAKE_VERSION` in
`effect_library.h`); a library whose index already records the same is
left as it is. A change to how libraries are baked or encoded raises that
version, so every library is rebuilt once.

Models share libraries, and name their frames by key. A rebuild that
replaces an index reports the keys it no longer has and the images whose
kind changed, one line each (`removed "<key>"`, `"<key>" changed kind from
<old> to <new>`): the models that name them lose those frames.

## Model animations

```json
"materials": [
  { "name": "PanelLights", "extensions": { "AERON_flight_model": { "flipbooks": [
    { "state": "always", "layer": "emissive", "mode": "loop", "library": "mycockpit",
      "emissiveStrength": 2, "frames": [["lights_0", 0.5], ["lights_1", 0.5]] } ] } } },
  { "name": "Screen", "extensions": { "AERON_flight_model": { "flipbooks": [
    { "state": "target", "layer": "baseColor", "mode": "loop", "library": "mycockpit",
      "frames": [["screen_a", 1]] } ] } } }
]
```

A material's object gives any of:

**`flipbooks`**: an ordered list. Per layer, the first flipbook whose state
is active plays, so a base colour and an emissive flipbook can play
together.

| Field | Meaning |
|-|-|
| `state` | `always`, or a game state (below) |
| `layer` | `baseColor` or `emissive`: the kind of its frames |
| `mode` | `loop`, or `once` (holds the last frame) |
| `library` | The library of its frames |
| `frames` | `[key, seconds]` pairs: images of the layer's kind. An emissive frame can also have a strength, `[key, seconds, strength]`, that multiplies `emissiveStrength` (default 1) |
| `emissiveStrength` | Optional, default 1: brightness of emissive frames |
| `unlit` | Optional, 0 to 1, default 0, base colour flipbooks: the part of the frames drawn unlit, as emission in place of the material's (for screens whose material is unlit) |

A flipbook plays from the moment its state becomes active; `always` plays
from the start.

States: `target`, `laser_lock`, `warhead.locking`, `warhead.locked`,
`threat.laser`, `threat.turret`, `threat.beam`, `threat.missile_locking`,
`threat.missile_locked`, `hull.75`, `hull.50`, `hull.25`, `cannon.1` …
`cannon.8`, `broken.cmd`, `broken.laser_ion`, `broken.beam_weapon`,
`broken.shields`, `broken.throttle`, `broken.sensors`,
`broken.laser_recharge`, `broken.engine_power`, `broken.shield_recharge`,
`broken.beam_recharge`, `hangar`. They are the player's, shown on the
player's craft and cockpit.

**`damage`**: hull damage, as images of kind `damage`.

```json
"damage": { "library": "mycockpit", "choice": 0, "placement": 0, "alternatives": [
  { "stages": ["dmg_75", "dmg_50", "dmg_25"], "scale": [0.2, 0.5], "offset": "inside" },
  { "stages": [null, null, null] } ] }
```

- Each object picks one alternative; materials with the same `choice`
  pick the same one.
- `stages` are the images at 75, 50 and 25% hull; `null` shows none.
- `scale` (optional) is the `[min, max]` side of a square placed at
  random, as a fraction of the texture; without it, the whole texture.
- `offset` (optional) moves that square: `inside` anywhere within the
  texture; `{ "min": [x, y], "max": [x, y] }` by `min + (max − min)·r` per
  axis, `r` random in [0, 1); without it, centred. Materials with the same
  `placement` share the random placement.

**`energy`**: the energy effects the material shows, any of `shields`,
`tractor` and `jamming`; `[]` for none.

The model root node's `AERON_flight_model` `energy` is the default of every
material without its own: energy effects are usually the whole model's,
with a few exceptions (a cockpit's screens). Without it, such a material
shows none.

## Canonical form

A library file may be written by hand, in any YAML. The tools write it in
one canonical form, so that the same values are always written the same
way: the file's leading comment lines, then one image per line in flow
style (`lights_0: { kind: emissive, emissive: frames/lights_0.png }`),
images in key order, strings quoted only when they need it, and each number
as the shortest decimal that reads back as the same 32-bit float. A tool
rewriting a file keeps its leading comments only. `aeron_gltf_cook
--library-json` prints a library file as JSON; `--library-from-json` checks
such JSON and writes the file in this form, keeping the leading comments of
the file it replaces.

## How a frame changes the material

While frames play, the material is drawn as any PBR material, with its
base colour and emissive changed:

1. Base colour: the material's base colour texture, then:
   - a `baseColor` frame painted over it: `lerp(base, frame.rgb, frame.a)`;
   - hull damage: times the damage factor;
   - an `emissive` frame's `baseColor` painted over the result.
2. The surface is lit with that base colour and the material's own normal,
   metallic and roughness (hull damage dulls the last two).
3. Emissive: while an `emissive` frame plays, its `emissive` image
   **replaces** the material's emissive over the whole material, black
   meaning no emission. Otherwise, while a `baseColor` frame plays from a
   flipbook with `unlit` u above 0, the base colour c of step 1 is split:
   c·(1 − u) is lit and c·u replaces the material's emissive. Otherwise
   the material's own emissive shows.
4. Screen (a material with a `screen`, whose display is drawn live): a
   `baseColor` frame covers the display by its coverage where the cover's
   window shows it, and replaces the display of a screen without a cover.

Emission brightness: the `emissive` image's colour (0 to 1 per channel,
from sRGB) × the flipbook's `emissiveStrength` (default 1) × the game's
emissive scale, which also applies to the material's own emissive. Paint
the image as the emitted colour at full brightness where it should be
brightest; raise `emissiveStrength` to go beyond white.
