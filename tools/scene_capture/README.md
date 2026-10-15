# Scene capture and benchmarking

`scene_capture` runs deterministic scenes through Aeron's production renderer.
It captures motion blur and bloom, compares bloom measurements across resolutions,
and benchmarks rendering without image readback or file output in the timed loop.

## Build and run

From the OpenXWA root, use a Release build for meaningful performance results:

```sh
cmake --build cmake-build-release --target scene_capture
./cmake-build-release/scene_capture aeron/tools/scene_capture/presets/bloom.yaml
./cmake-build-release/scene_capture benchmark aeron/tools/scene_capture/presets/benchmark.yaml
```

For standalone Aeron:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERON_BUILD_SCENE_CAPTURE=ON -DAERON_BUILD_TOOLS=OFF
cmake --build build --target scene_capture
./build/scene_capture tools/scene_capture/presets/bloom.yaml
```

`AERON_BUILD_TOOLS` also enables the tool. Multi-configuration generators need
`--config Release`; the executable and shaders are placed at the build root.

Copy a preset and choose a fresh `output_dir`: existing capture manifests are
never overwritten. Input YAML paths must be relative to the working directory.
Relative `asset_root` and `output_dir` paths also use the working directory,
not the YAML file's folder. The tool renders offscreen: its window stays hidden
and, on macOS, it takes no Dock icon or focus. Interrupt it (Ctrl+C) to cancel a
run.

## Presets and settings

| Preset | Purpose |
|---|---|
| [motion-blur.yaml](presets/motion-blur.yaml) | Twelve cuboids with motion blur and a 64-sample temporal reference. |
| [models.yaml](presets/models.yaml) | Motion blur on OPT models; configure `asset_root` and model paths first. |
| [bloom.yaml](presets/bloom.yaml) | Static HDR chart for bloom proportionality, color and spread measurements. |
| [bloom-motion.yaml](presets/bloom-motion.yaml) | Bright cuboids with both effects and a 16-sample motion-blur reference. |
| [benchmark.yaml](presets/benchmark.yaml) | Repeating 30-frame scene with both effects and timing settings. |
| [flight-benchmark.yaml](presets/flight-benchmark.yaml) | A game-like flight scene (`game_scene`): cockpit view with SSAO, shadows, point lights and converted XWAU models; configure `asset_root` first. |
| [hangar-lights.yaml](presets/hangar-lights.yaml) | An indoor `game_scene`: a converted XWAU hangar with fifteen parked fighters, lit only by point lights under its ceiling panels, for comparing local-light models; configure `asset_root` first. |
| [hangar-tubes.yaml](presets/hangar-tubes.yaml) | `hangar-lights.yaml` with one tube light per ceiling row in place of its point lights. |
| [hangar-one-sided.yaml](presets/hangar-one-sided.yaml) | `hangar-tubes.yaml` with one-sided tubes emitting downward, as the ceiling strips. |
| [hangar-probes.yaml](presets/hangar-probes.yaml) | `hangar-lights.yaml` with environment probes: the hangar owns four flat-coloured probes and a grid lit from one side, over a dim environment. Build the test packages first with `probe_packages.py --baker <build dir>/environment_bake --out captures/probe-assets`. |
| [flight-probes.yaml](presets/flight-probes.yaml) | A turned Star Destroyer owning four flat-coloured probes and a grid, fighters above its hull, no environment; same test packages. |
| [anim-example.yaml](presets/anim-example.yaml), [anim-example-target.yaml](presets/anim-example-target.yaml) | The authored animated-material example of `tools/gltf_cook/example` at two times, without and with the `target` state. Build its assets first with `tools/gltf_cook/example/build.sh <build dir> captures/anim-example-assets`. |

Start from a preset; unknown keys are rejected. The main settings are:

| Setting | Meaning |
|---|---|
| `width`, `height` | Render dimensions in pixels. |
| `frames`, `fps` | Number of scene poses and their simulation timestep. |
| `pan_degrees_per_second`, `pan_direction_degrees` | Camera speed and direction; direction 0 is yaw, speed 0 is stationary. |
| `quality` | Motion blur: 0/off, 1/Low, 2/High. |
| `shutter` | Exposure is `shutter * 32 ms`, independent of `fps`; range 0–1. |
| `reference_samples` | Temporal reference samples; 0 disables it, maximum 256. |
| `fixture` | `shapes` (default) or `bloom_chart`. |
| `models`, `asset_root` | Optional OPT or cooked `.glb` paths and their VFS root; replaces the cuboids. |
| `effect_dir`, `time` | Optional material animation: the effect libraries' directory under `asset_root` (`remaster/effects`) and the animation time of frame 0 in seconds (default 0), which advances with the frames. Without `effect_dir`, animated materials show their static look. |
| `anim` | Optional `{ damage_stage, energy, state_mask, seed, energy_start, onset: [...] }`: the material animation state of every instance (`AeronSceneMaterialAnim`), instance `i` taking seed `seed + i`, so their damage alternatives and placements differ. Needs `effect_dir`. |
| `blast_mark` | Optional `{ image: "3050-1", center: [x, y, z], size }`: draws that `effects.yaml` image's frame at the animation time as an animated blast mark on the first model's first instance, as the game does: the classic mark square, centred at `center` (model metres) in the model's XY plane and `size` metres wide, shows the image's crop, and the image spreads around it over a square three times as wide (not with `game_scene`). Needs `effect_dir`. |
| `msaa_samples` | 1, 2, 4 or 8; default 2. |
| `cull` | `none` (default, two-sided), `front` or `back`: the instances' cull mode. `front` culls what the game's `back` culls, as this camera's handedness is the opposite. |
| `backface_fill` | `true` (default) or `false`: the dark fill behind cracks in single-sided instances (`AeronScene_SetBackfaceFill`). |
| `screen_test` | Optional mapping: `eye: [x, y, z]` (model metres), `yaw_degrees`, `pitch_degrees` (positive looks down). Renders the first model alone from that point, unscaled, with test screen inputs: a gradient checker as graphics and a white "F" with a 32-pixel grid as text, every source covering the whole texture. Checks screen surfaces (cockpit displays, holograms) for placement, orientation and mirroring. |
| `emissive_scale` | Shape/material emission multiplier; default 1. Use 8 to make the cuboids glow strongly. |
| `bloom` | Optional mapping requiring `intensity` (the fraction of light scattered, 0 to 1) and `kernel` (1 or 4 taps). Omit to disable bloom captures. |
| `tonemap` | `agx` (default) or `aces`. |
| `output_dir` | Directory for images and reports. |
| `game_scene` | Optional mapping: renders with the game's Scene3D features instead of the plain fixture (see below). |

The bloom chart requires motion blur, camera speed and reference samples to be
zero, `msaa_samples: 1`, `emissive_scale: 1`, and no models. Its emitters use
pixel-area coverage and sizes proportional to image height. In row order:

| Row | Column 1 | Column 2 | Column 3 | Column 4 |
|---|---|---|---|---|
| 1 | White 0.8 | White 1.0 | White 1.15 | White 1.3 |
| 2 | Small white 2 | Small white 4 | Medium white 8 | Large white 16 |
| 3 | Red 8 | Green 8 | Blue 8 | Thin white line 8 |

The line is one pixel wide at 1080p. Use landscape captures at 720p or above;
broad halos can cross region boundaries, so inspect images alongside the metrics.
FSR, SSAO and shadows are disabled; presentation previews use SDR tonemapping.

### Game scene

`game_scene` renders the models as the game does, so benchmarks include the
features it always enables. The scene is in game world units (OPT units,
`view_space_to_meters` and a one-unit near plane as the game sets them); preset
positions stay in metres.

| Setting | Meaning |
|---|---|
| `defaults` | Required: the Scene3D defaults file the game loads (`aeron/config/scene3d_defaults.yaml` from the OpenXWA root). Its SSAO and directional shadows apply; the capture keeps its own tonemap. |
| `ssao`, `shadows` | `true` (default) or `false`, to turn either off. |
| `temporal_upscaling` | Optional `{ mode, sharpness }` as the game's settings: `mode` is `off` (default), `native_aa`, `quality`, `balanced` or `performance`; `sharpness` 0–1 enables RCAS above 0. Benchmarks only, since temporal output depends on frame history, and needs `msaa_samples: 1` as in the game. The history resets when the pose sequence restarts. |
| `model_light_intensity` | Scene illuminance per lux of the lights authored in the models (in candela), as the game's `point_lights.model_light_intensity`: default 1/683, 0 to 1, 0 turns them off. Every drawn instance submits its model's lights. |
| `environment` | Optional `{ folder, manifest, package }`: `manifest` holds `packages:` as the game's environment manifest does, in `folder` (default: `asset_root`) with the files its packages name; `package` (optional) is the environment the scene uses. Instances name the `probes` packages they own. |
| `cluster_debug` | `false` (default) or `true`: the clustered lights' occupancy view (lights per cluster as colours) in place of the lit scene, as the game's Point Lights debug tool. |
| `cockpit_follows_camera` | `false` (default) or `true`. With a panning camera, `false` keeps the cockpit and exterior in place (the pilot looks around); `true` turns them with the camera (the craft turns), as the game carries its cockpit. |
| `key_probe_shadow` | Optional, 0 to 1: the sun is in the probe sets' bake, as the game's hangar ceiling light is. Inside a probe grid it then lights nothing, and its shadow removes this share of the probe light. Off by default. |
| `key_shadow_all_sides` | `true` (default) or `false`. With `key_probe_shadow`, the sun's shadow takes the probe light from its side whichever way surfaces face, as the game's Hangar Lighting "Shadow from all sides" does; `false` takes it only as surfaces face the sun. |
| `probe_occlusion` | `false` (default) or `true`: the instances inside the first probe set's grid, other than its owner, take its light from what they cover (diffuse light and reflections), as the game's Hangar Lighting "Ship occlusion" does. It replaces the sun shadow's cut of `key_probe_shadow`. |
| `sun_direction`, `sun_color` | Surface-to-sun direction in camera axes (+X right, +Y down, +Z forward) and linear RGB illuminance; default white 2.513, the game's white classic sun of intensity 1 at its default `lighting.sun_illuminance_scale`. |
| `ambient` | Linear ambient fill, default 0.08. |
| `point_lights` | Up to 256 `{ position, intensity, half_axis, direction }` lights: position in metres and linear RGB intensity, the illuminance 1 m away. Drawn as the game's point lights: 0.25 m inverse-square spheres through clustered lighting. At the game's default `point_lights.match_distance_m` (27.5), its classic intensity `I` is about `1.05 × I` here. An optional `half_axis` (metres, camera axes) makes a tube along `position ± half_axis` with the same total intensity, as the game's laser streaks (a fighter laser is 17 m long). An optional `direction` (camera axes) makes the light one-sided, like a ceiling panel: `intensity × cos θ` around that direction and nothing behind it; a tube's direction is made perpendicular to its axis (a flat strip). |
| `instances` | Required, up to 1024 `{ model, role, position, rotation_degrees, markings, probes }` entries; `model` is one of `models`. `probes` (optional) names a `probes` package of `environment`'s manifest the instance owns; other instances whose bounds reach it use it too. `markings` (0-255, optional) shows the look the model's markings list gives that markings value: the model's mesh then has one variant index per markings value up to the highest given, a given value showing its list entry and loading only that variant's textures. A model no instance gives markings shows its whole markings list. `role: ship` (default) places it at `position` with `[yaw, pitch, roll]`. `hangar` places it the same way, receiving shadows without casting them, as the game draws its hangar under the ceiling light. `cockpit` places it at `screen_test`'s eye with the game cockpit's flags (its own shadow map, which it casts into with both faces). `exterior` places it there too but only casts shadows, like the player's hidden exterior in cockpit view. |

Models are placed mirrored from their left-handed OPT frames, as the game places
them, so `cull: back` matches the game's culling (the default fixture's `front`
does not apply). Captures stay deterministic: with temporal upscaling off, SSAO
and shadow sampling patterns do not vary between runs.

## Capture outputs

`capture.yaml` records the resolved settings, backend and fixture version.
Images have both PFM (float32 RGB) and PNG (8-bit sRGB) versions:

| Output | Contents |
|---|---|
| `frame_NNN_sharp`, `frame_NNN_blur` | Unblurred and motion-blurred 3D scene, in linear HDR. |
| `frame_NNN_scene` | Linear HDR input from the bloom chart. |
| `frame_NNN_reference` | Temporal reference, when requested. |
| `frame_NNN_bloom` | Native-size linear bloom before intensity and final presentation filtering. |
| `frame_NNN_present` | Scene plus bloom through the production tonemapper; its PFM is display-linear. |
| `frame_NNN_metrics.yaml` | Motion-blur reference error. |
| `frame_NNN_bloom_metrics.yaml` | Bloom energy, peak, centroid and spread, globally and per chart region. |

PNG previews clamp to [0,1]; use `present` to judge bloom visually because raw
HDR previews clip bright cores. At zero bloom intensity, capture mode still
records the raw chain and produces a presentation without its contribution.

The motion-blur reference averages independently rendered subframes across a
centered box shutter. It excludes bloom. RMSE, MAE, signed mean and maximum error
use pixels where either image exceeds 0.02 in any channel. Differences include
the reconstruction shader's weighting, occlusion and streak-limit approximations;
increase `reference_samples` to check convergence.

Bloom measurements have no dim-pixel cutoff. RGB integrals divide sums by image
pixel count; spread is RGB-weighted standard deviation in image-height units.
It measures the emitter plus halo, not the outer halo radius. Bloom has no
brightness threshold, so energy scales with emitter brightness; pixel coverage
and texture precision can change energy between resolutions.
Zero-denominator ratios are `null`; zero-energy centroids/spreads are undefined
and reported as zero.

## Compare with a Cycles reference

Three scripts render a game scene with `scene_capture` and the same scene in
Blender's Cycles, then compare them term by term. They run in Blender
(tested with 5.1), whose Python brings numpy and OpenImageIO. The scene is a
.blend holding each model under its own root object (as the glTF importer
leaves it), a camera, and the lights and emission the hangar's probes were
baked from. The .blend is never saved.

```sh
blender -b scene.blend --python cycles_match.py -- --out DIR \
    --asset-root GAME_DATA --hangar OPT_HANGAR \
    --model OPT_HANGAR=remaster/models/hangar.glb --model OPT_SHIP=remaster/models/ship.glb \
    --package BAKE_ROOT/flight/environments/models/hangar/package.yaml \
    --bake-root BAKE_ROOT --defaults aeron/config/scene3d_defaults.yaml
(cd DIR && for c in full diffuse reflect; do SCENE_CAPTURE $c.yaml; done)
blender -b scene.blend --python cycles_render.py -- DIR
blender -b --python cycles_compare.py -- DIR [--box NAME X0 Y0 X1 Y1]
```

- `cycles_match.py` places the models as in the .blend, seen from its camera,
  and writes three configs: the whole scene, the probes' diffuse light only,
  and their reflections only. A model whose parts are posed differently in
  the .blend (landing gear, a ramp) is copied with them posed as there. The
  hangar's ceiling light points up the hangar's axis, and `key_probe_shadow`
  is the package's `key_light_shadow`.
- `cycles_render.py` renders the reference with its light passes: no light
  from outside the model, as the probes bake, and `scene_capture`'s 60 degree
  vertical field of view. 1920 x 1080 at 1024 samples takes about two minutes
  on an M4 Pro GPU.
- `cycles_compare.py` writes `DIR/compare`: both renders per term at one
  exposure, the game-to-Cycles ratio per term, an edge overlay to check the
  views line up (yellow where they do), and `regions.txt`, the mean of each
  term on the floor (open, under the models, in each `--box`) and on the
  models' surfaces.

Captures never overwrite: remove `DIR/captures` before running them again.

## Compare bloom across resolutions

Copy the bloom preset for 720p, 1080p, 1440p and 4K, using distinct output
directories. Keep aspect ratio, scene and effect settings matched. An odd size
such as 1279×719 also tests rounding and fractional sampling.

```sh
./cmake-build-release/scene_capture compare \
  captures/bloom-1080p/frame_000_bloom_metrics.yaml \
  captures/bloom-2160p/frame_000_bloom_metrics.yaml > captures/bloom-comparison.yaml
```

The command requires no GPU and reports candidate/baseline ratios for normalized
spread and bloom energy, plus RGB integral deltas. Ratios near 1 indicate
consistency for resolved bright emitters; subpixel emitters also depend on
coverage. There is no automatic pass threshold.

View `present` images at the same displayed size to compare appearance, and at
native resolution to inspect sampling artifacts. Keep the Aeron revision with
your results. Repeated captures should be byte-identical on the same backend;
that is not guaranteed across GPUs.

## Benchmarking

Use `scene_capture benchmark <preset.yaml>` with any scene preset and a fresh
output directory. It writes settings to `capture.yaml` and timing results to
`benchmark.yaml`, without images or temporal references. Optional defaults:

```yaml
benchmark:
  warmup_seconds: 1
  sample_seconds: 1
  samples: 7
  min_frames: 120
  frames_in_flight: 2
```

The tool primes one complete scene sequence, then warms up and measures repeated
windows. Each window meets both its duration and minimum frame count and ends
on a complete sequence. `frames` and `fps` define the repeated camera poses,
not a frame-rate limit; a longer run cannot pan beyond the configured scene.
Allowed ranges are 0.1–60 seconds, 3–31 samples, 1–100000 minimum frames and 1–4
frames in flight. The benchmark preset uses a two-second warm-up.

Timing includes scene preparation/uploads, rendering, enabled effects,
tonemapping, submission, event handling and completion waits. All submitted GPU
work finishes before a window ends. Setup, priming, warm-up, reference rendering,
diagnostic conversion, readback, file output and swapchain/VSync presentation
are excluded. Tonemapping always runs; zero bloom intensity skips the bloom chain.

| Report field | Interpretation |
|---|---|
| `mean_frame_ms` | Wall time divided by completed frames: rendering throughput including CPU and GPU work, not GPU-only time or displayed-frame latency. |
| `throughput_fps` | Reciprocal throughput; not a prediction of full-game FPS. |
| `mean_cpu_record_submit_ms` | CPU preparation/recording/submission, including possible driver stalls. CPU and GPU overlap, so subtracting this from frame time does not yield GPU time. |
| `sample_mean_frame_ms` | Median, min, max and coefficient of variation of window averages; not individual-frame percentiles. |
| `sample_variation_within_5_percent` | Repeatability check. Noisy runs warn; increase warm-up/window duration and rerun under steady load. |
| `gpu_execution_ms` | `null`: SDL's public GPU API does not expose GPU timestamps. Use a backend profiler for GPU-only timings. |

Light scenes do not saturate the GPU. On Apple GPUs especially, clock scaling then
skews wall-clock results: removing work can make frames slower and noisier, so
differences of about 0.1 ms between light configurations are not reliable. Use
a heavier scene or a backend profiler for those.

Compare matching builds, fixture versions, queue depths and workloads. One frame
in flight serializes CPU/GPU work; two allows overlap. Reports record the device,
backend and validation state.
