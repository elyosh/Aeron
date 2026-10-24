# scene_capture

Renders fixed scenes through Aeron's renderer to capture images (motion blur,
bloom, game scenes), compare them with references, and benchmark rendering.

## Build and run

From the OpenXWA root, with a Release build:

```sh
cmake --build cmake-build-release --target scene_capture
./cmake-build-release/scene_capture aeron/tools/scene_capture/presets/bloom.yaml
./cmake-build-release/scene_capture benchmark aeron/tools/scene_capture/presets/benchmark.yaml
```

Standalone Aeron: configure with `-DAERON_BUILD_SCENE_CAPTURE=ON`, then build
the `scene_capture` target.

- The YAML path, `asset_root` and `output_dir` are relative to the working
  directory, not to the YAML file.
- Captures never overwrite: use a new `output_dir` for each run.
- Unknown keys are rejected. Start from a preset.

## Presets

| Preset | Scene |
|---|---|
| `motion-blur.yaml` | Moving cuboids, with a motion-blur reference. |
| `bloom-motion.yaml` | Bright moving cuboids, with bloom and a motion-blur reference. |
| `bloom.yaml` | Static HDR chart for bloom measurements. |
| `models.yaml` | Motion blur on OPT models. |
| `benchmark.yaml` | Repeating 30-frame scene with timing settings. |
| `flight-benchmark.yaml` | Cockpit view of a game flight scene. |
| `hangar-lights.yaml` | Hangar lit by point lights; `hangar-tubes.yaml` and `hangar-one-sided.yaml` use tube lights. |
| `hangar-probes.yaml`, `flight-probes.yaml` | Scenes with environment probes. Build the test probes first with `probe_bakes.py --baker <build dir>/environment_bake --out captures/probe-assets`. |
| `anim-example.yaml`, `anim-example-target.yaml` | The animated-material example. Build its assets first with `tools/gltf_cook/example/build.sh <build dir> captures/anim-example-assets`. |

Presets that use game models need `asset_root` set to your game data.

## Settings

| Setting | Meaning |
|---|---|
| `width`, `height` | Image size in pixels. |
| `frames`, `fps` | Number of camera poses and the time between them. |
| `pan_degrees_per_second`, `pan_direction_degrees` | Camera turn speed and direction (0 = yaw). |
| `quality` | Motion blur: 0 off, 1 Low, 2 High. |
| `shutter` | Exposure time, `shutter × 32 ms` (0 to 1). |
| `reference_samples` | Sub-frames averaged for the motion-blur reference (0 = none, max 256). |
| `msaa_samples` | 1, 2, 4 or 8 (default 2). |
| `fixture` | `shapes` (default) or `bloom_chart`. |
| `models`, `asset_root` | OPT or `.glb` models replacing the cuboids, and the folder they are read from. |
| `effect_dir`, `time` | Material animation: the effects folder under `asset_root` (`mod/flight/effects`) and the start time in seconds. |
| `anim` | Material animation state of every instance: `{ damage_stage, energy, state_mask, seed, energy_start, onset }`. Needs `effect_dir`. |
| `blast_mark` | `{ image, center, size }`: a blast mark on the first model (not with `game_scene`). Needs `effect_dir`. |
| `cull` | `none` (default), `front` or `back`. Use `back` for game models. |
| `backface_fill` | `true` (default) or `false`. |
| `screen_test` | `{ eye, yaw_degrees, pitch_degrees }`: shows the first model from that point with test images on its screens. |
| `emissive_scale` | Emission multiplier (default 1). |
| `bloom` | `{ intensity, kernel }` (kernel 1 or 4). Omit to disable bloom. |
| `tonemap` | `agx` (default) or `aces`. |
| `agx_highlight` | `{ saturation, ev_start, ev_end, hue }`: the AgX saturation of very bright pixels, reached at `ev_end` EV of scene luminance (0 = white), from the punchy saturation at `ev_start`; `hue` (0 to 1, default 0) is how much of them keeps its hue over the same ramp. Omit it to keep the renderer's default. |
| `output_dir` | Where images and reports go. |
| `game_scene` | Renders like the game; see below. |

The bloom chart needs `quality`, `pan_degrees_per_second` and
`reference_samples` at 0, `msaa_samples: 1`, `emissive_scale: 1` and no models.

### game_scene

Renders the models with the game's renderer settings (SSAO, shadows, point
lights). Positions are in metres, in camera axes (+X right, +Y down, +Z
forward).

| Setting | Meaning |
|---|---|
| `defaults` | Required: `aeron/config/scene3d_defaults.yaml`. |
| `instances` | Required, up to 1024; see below. |
| `ssao`, `shadows` | `true` (default) or `false`. |
| `temporal_upscaling` | `{ mode, sharpness }`, mode `off`, `native_aa`, `quality`, `balanced` or `performance`. Benchmarks only, with `msaa_samples: 1`. |
| `sun_direction`, `sun_color` | Direction toward the sun and its linear RGB illuminance. |
| `ambient` | Ambient light (default 0.08). |
| `point_lights` | Up to 256 `{ position, intensity, half_axis, direction }`: intensity is the linear RGB illuminance 1 m away; `half_axis` makes a tube, `direction` makes it one-sided. |
| `model_light_intensity` | Brightness of the lights built into the models (default 1/683, 0 turns them off). |
| `environment` | `{ folder, manifest, name }`: the environment manifest and the environment to use. |
| `cockpit_follows_camera` | `true` turns the cockpit with the camera; `false` (default) keeps it in place. |
| `cluster_debug` | `true` shows the number of lights per cluster instead of the lit scene. |
| `key_probe_shadow`, `key_shadow_all_sides`, `probe_occlusion`, `planar_reflection` | Hangar lighting options, as in the game's Hangar Lighting tool. |

Each instance is `{ model, role, position, rotation_degrees, velocity, markings, probes }`:

- `model`: one of `models`.
- `role`: `ship` (default) or `hangar`, placed at `position` with
  `rotation_degrees: [yaw, pitch, roll]`; `cockpit` or `exterior`, placed at
  `screen_test`'s eye.
- `velocity`: metres per second; the instance moves, in the motion blur and
  its reference too.
- `markings`: a markings value (0–255).
- `probes`: a probes bake folder to use instead of the model's own.

## Outputs

Each image is written as PFM (float32, linear) and PNG (8-bit sRGB, clipped to
1). PFM files store rows from the bottom up.

| File | Contents |
|---|---|
| `frame_NNN_sharp`, `frame_NNN_blur` | Scene without and with motion blur. |
| `frame_NNN_reference` | Motion-blur reference, when `reference_samples` is set. |
| `frame_NNN_present` | Final image with bloom and tonemapping. Use it to judge bloom. |
| `frame_NNN_bloom` | Bloom alone. |
| `frame_NNN_scene` | The bloom chart's input. |
| `frame_NNN_metrics.yaml` | Motion-blur error against the reference (RMSE, MAE, max), over pixels brighter than 0.02. |
| `frame_NNN_bloom_metrics.yaml` | Bloom energy, peak, centre and spread. |
| `capture.yaml` | The settings used. |

Captures are deterministic on one machine, except with temporal upscaling.

## Compare bloom across resolutions

Capture the bloom preset at several sizes with the same aspect ratio, then:

```sh
./cmake-build-release/scene_capture compare \
  captures/bloom-1080p/frame_000_bloom_metrics.yaml \
  captures/bloom-2160p/frame_000_bloom_metrics.yaml > captures/bloom-comparison.yaml
```

It reports the ratios of spread and energy between the two. Ratios near 1 mean
bloom looks the same at both sizes.

## Compare with Cycles

Renders a game scene with `scene_capture` and in Blender's Cycles (5.1), then
compares them per light term. The .blend holds each model under its own root
object, a camera, and the lights the probes were baked from.

```sh
blender -b scene.blend --python cycles_match.py -- --out DIR \
    --asset-root GAME_DATA --hangar OPT_HANGAR \
    --model OPT_HANGAR=mod/flight/models/hangar.glb \
    --model OPT_SHIP=mod/flight/models/ship.glb \
    --probes PROJECT/hangar/bake/probes --defaults aeron/config/scene3d_defaults.yaml
(cd DIR && for c in full diffuse reflect; do SCENE_CAPTURE $c.yaml; done)
blender -b scene.blend --python cycles_render.py -- DIR
blender -b --python cycles_compare.py -- DIR [--box NAME X0 Y0 X1 Y1]
```

- `cycles_match.py` writes three configs: the whole scene, the probes' diffuse
  light only and their reflections only.
- `cycles_render.py` renders the Cycles reference (about two minutes at 1080p).
- `cycles_compare.py` writes `DIR/compare`: both renders, their ratio per term,
  an edge overlay to check the views line up, and `regions.txt` with mean
  values per region.

Remove `DIR/captures` before running the captures again.

## Benchmarking

```sh
./cmake-build-release/scene_capture benchmark <preset.yaml>
```

It renders the scene repeatedly without writing images and saves the timings
to `benchmark.yaml` in `output_dir`. Optional settings, with their defaults:

```yaml
benchmark:
  warmup_seconds: 1
  sample_seconds: 1
  samples: 7
  min_frames: 120
  frames_in_flight: 2
```

- `mean_frame_ms` and `sample_mean_frame_ms` (median, min, max, variation)
  are wall-clock time per frame, CPU and GPU together. There is no GPU-only
  time; use a GPU profiler for that.
- Compare builds by alternating them (A B B A) over several runs; a long run
  heats the GPU and slows later runs.
- Light scenes don't keep the GPU busy, and clock changes then make
  differences of about 0.1 ms unreliable.
