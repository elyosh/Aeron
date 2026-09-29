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
not the YAML file's folder. Capture resolution is independent of the host window;
closing that window cancels the run.

## Presets and settings

| Preset | Purpose |
|---|---|
| [motion-blur.yaml](presets/motion-blur.yaml) | Twelve cuboids with motion blur and a 64-sample temporal reference. |
| [models.yaml](presets/models.yaml) | Motion blur on OPT models; configure `asset_root` and model paths first. |
| [bloom.yaml](presets/bloom.yaml) | Static HDR chart for bloom threshold, color and spread measurements. |
| [bloom-motion.yaml](presets/bloom-motion.yaml) | Bright cuboids with both effects and a 16-sample motion-blur reference. |
| [benchmark.yaml](presets/benchmark.yaml) | Repeating 30-frame scene with both effects and timing settings. |

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
| `models`, `asset_root` | Optional OPT paths and their VFS root; replaces the cuboids. |
| `msaa_samples` | 1, 2, 4 or 8; default 2. |
| `emissive_scale` | Shape/material emission multiplier; default 1. Use 8 to raise the cuboids above the bloom threshold. |
| `bloom` | Optional mapping requiring `intensity` and `kernel` (1 or 4 taps). Omit to disable bloom captures. |
| `tonemap` | `agx` (default) or `aces`. |
| `output_dir` | Directory for images and reports. |

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
It measures the emitter plus halo, not the outer halo radius. Thresholding,
pixel coverage and texture precision can change energy between resolutions.
Zero-denominator ratios are `null`; zero-energy centroids/spreads are undefined
and reported as zero.

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
consistency for resolved bright emitters; near-threshold and subpixel emitters
also depend on coverage. There is no automatic pass threshold.

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

Compare matching builds, queue depths and workloads. One frame in flight
serializes CPU/GPU work; two allows overlap. Reports include device, compiler,
backend and validation state; Debug validation warns. All samples and queue
overhead are retained in the statistics.

Use benchmark report schema version 2. Version-1 measurements were inflated by
a fence-retirement problem and should be discarded.
