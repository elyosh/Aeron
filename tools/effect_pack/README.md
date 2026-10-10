# Animated materials

A model's materials can play animated frames: flipbooks driven by game
states, hull damage and energy effects. The frames live in **effect
libraries** shared by models; the model's materials say which frames play
and when. Both are built from an **animation file**:

```sh
effect_pack --out <asset root>/remaster/effects model.anim.yaml
aeron_gltf_cook --animations model.anim.yaml --effects-dir <asset root>/remaster/effects \
    model.gltf <asset root>/remaster/models/model.glb
```

- `effect_pack` builds the library from the file's `images`.
- `aeron_gltf_cook --animations` writes the file's `materials` into the
  model. With `--effects-dir`, every frame must be in its library there.

[`example/`](example) is a working model with its animation file.

## Flow

1. **Model.** Export glTF with ordinary PBR materials. Give each animated
   surface its own material and UV island: frames cover the material's whole
   UV space (or the part `uv` selects).
2. **Frames.** Paint each frame as an sRGB PNG with the material texture's
   UV layout.
3. **Animation file.** Describe the images and the materials' animations.
4. **Build** with the commands above, and install the model in
   `remaster/models/` and the library in `remaster/effects/`.

The material's own textures show when no flipbook plays, and in any glTF
viewer: make them the animation's rest look.

## Animation file

```yaml
version: 1
library: mycockpit              # default library of the materials' frames
images:
  lights_0: { kind: illumination, emissive: frames/lights_0.png }
  lights_1: { kind: illumination, emissive: frames/lights_1.png }
  screen_a: { kind: colour, image: frames/screen_a.png }
materials:                      # glTF material names
  PanelLights:
    flipbooks:
      - { state: always, layer: illumination, mode: loop, emissiveStrength: 2,
          frames: [[lights_0, 0.5], [lights_1, 0.5]] }
  Screen:
    flipbooks:
      - { state: target, layer: colour, mode: loop, frames: [[screen_a, 1]] }
```

Paths are relative to the file. Image keys are free names, unique within
the library. Library names use `a-z`, `0-9`, `_` and `-`; one library per
model avoids clashes.

### Image kinds

| Kind | Inputs | Use |
|-|-|-|
| `colour` | `image` (RGBA, alpha = coverage) | Paints over the material's colour |
| `illumination` | `emissive` (RGB); optional `colour` (RGBA, alpha = coverage) | Replaces the material's glow; with `colour`, also paints over its colour |
| `multiply` | `image` (RGB factor, alpha = gloss factor) | Hull damage |
| `mark` | `image` (RGBA) | Blast marks |
| `energy` | `image` (RGBA, alpha = intensity) | Energy effects |

`mark` and `energy` images are the game's own effects, which
`remaster/effects/effects.yaml` names; materials do not reference them.

All inputs of one image have the same size. Images are capped at 1024
texels on their longer side.

### Materials

A material lists any of:

**`flipbooks`**: an ordered list. Per layer, the first flipbook whose state
is active plays, so a colour and an illumination flipbook can play together.

| Field | Meaning |
|-|-|
| `state` | `always`, or a game state (below) |
| `layer` | `colour` or `illumination` |
| `mode` | `loop`, or `once` (holds the last frame) |
| `frames` | `[key, seconds]` pairs |
| `library` | Optional, default the file's |
| `uv` | Optional `{ scale: [sx, sy], offset: [ox, oy], clamp: true }`: frames are sampled at `uv·scale + offset` |
| `emissiveStrength` | Optional, default 1: brightness of illumination frames |

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

**`damage`**: hull damage, as `multiply` images.

```yaml
damage: { choice: 0, placement: 0, alternatives: [
  { stages: [dmg_75, dmg_50, dmg_25], scale: [0.2, 0.5], offset: inside },
  { stages: [null, null, null] } ] }
```

- Each object picks one alternative; materials with the same `choice`
  pick the same one.
- `stages` are the images at 75, 50 and 25% hull; `null` shows none.
- `scale` (optional) is the `[min, max]` side of a square placed at
  random, as a fraction of the texture; without it, the whole texture.
- `offset` (optional) moves that square: `inside` anywhere within the
  texture; `[x0, y0, x1, y1]` by `(x1 − x0)·r − x0` per axis, `r` random
  in [0, 1); without it, centred. Materials with the same `placement` share the random placement.

**`energy`**: the energy effects the material shows, any of `shields`,
`tractor` and `jamming`.

**`animBaseColor`**: optional PNG, the colour illumination frames apply
over when the material's base colour is not it (for example a base colour
darkened where the texture glows).

## Libraries

`effect_pack` writes `<library>.yaml` (the index), and `<library>.layer.ktx2`
and `<library>.emissive.ktx2` (the frames, as needed). Models load the
libraries they name from `remaster/effects/`.
