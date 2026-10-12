# Animated material example

A small model whose materials animate. It shows the authoring flow
([`../animated_materials.md`](../animated_materials.md)) and is captured by the `anim-example`
presets of `scene_capture`.

| File | Role |
|-|-|
| `panel.gltf`, `panel.bin`, `textures/` | The model: two panels. `Lights` has a dark base colour and a dim static emissive map; `Screen` a grey bezel and black glass. The root and component nodes carry the minimal `AERON_flight_model` roles the game needs. |
| `frames/` | The animation frames, PNGs in the materials' UV layout. |
| `examplepanel.yaml` | The effect library: its frames. |
| `panel.anim.yaml` | The model's animations: each material's flipbooks. |
| `build.sh` | Builds the library with `aeron_gltf_cook --library` and cooks the model with `--animations`. |

```sh
aeron/tools/gltf_cook/example/build.sh cmake-build-release captures/anim-example-assets
cp aeron/tools/scene_capture/presets/anim-example*.yaml .
cmake-build-release/scene_capture anim-example.yaml
cmake-build-release/scene_capture anim-example-target.yaml
```

What the captures show, at 0.25 s and 0.75 s:

- `Lights`: an `always` emissive flipbook with emission-only frames
  (`kind: emissive`, `emissive:` only), so the frames replace the glow and
  keep the panel's colour: lamps 0 and 2 amber, then 1 and 3 green, at
  `emissiveStrength` 3.
- `Screen`: a base colour flipbook on the `target` state. Without it the black
  glass shows; with it (`anim-example-target`) the glass is blue, then red.
  The frames' alpha covers the glass only, so the bezel keeps its colour.

In the game the model goes to `remaster/models/` and the library files to
`remaster/effects/`.
