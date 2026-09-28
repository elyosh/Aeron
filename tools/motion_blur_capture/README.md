# Deterministic motion-blur capture

This standalone diagnostic tool renders twelve untextured, emissive cuboids through
`AeronScene3D`, including its normal mesh prepass, camera/object velocity path,
TileMax/NeighborMax and motion-blur reconstruction. It needs no game assets.
A broad face, narrower rotated objects and a thin rod expose different artifact
scales against a dark background. Eight scattered small cuboids provide additional
thin-feature targets at varied depths, orientations and brightnesses. Their fixed
placements preserve repeatability. This arrangement is recorded as fixture version 2;
version 1 contained only the four larger objects.

The camera rotates at a fixed angular speed. Poses are computed from frame
indices and the YAML `fps`; wall-clock speed and capture/readback stalls cannot
change the images. `pan_direction_degrees` changes the rotation axis between
yaw and pitch. Frame zero has the same central camera pose for every pan speed
and direction; its previous pose is one fixed timestep earlier.

## Build and run

From the Aeron repository root:

```sh
cmake -S . -B build -DAERON_BUILD_MOTION_BLUR_CAPTURE=ON -DAERON_BUILD_TOOLS=OFF
cmake --build build --target motion_blur_capture
./build/motion_blur_capture tools/motion_blur_capture/capture.yaml
```

From OpenXvT's root, using its Aeron checkout:

```sh
cmake -S . -B cmake-build-debug -DAERON_BUILD_MOTION_BLUR_CAPTURE=ON
cmake --build cmake-build-debug --target motion_blur_capture
./cmake-build-debug/motion_blur_capture aeron/tools/motion_blur_capture/capture.yaml
```

The tool is also included by `AERON_BUILD_TOOLS`. The dedicated option enables it
without requiring the other tools or example applications.

Multi-configuration generators also place the executable at the build root,
beside its shader directory. Shader formats and graphics backends use the same
Aeron/SDL GPU selection as other applications. This is not a Metal-specific fixture.

The configuration path is relative to the working directory. Copy the supplied
YAML to make variants. Use a new `output_dir` for every run: existing capture
manifests are not overwritten. The small host window is only for GPU device
initialization; rendering and capture use exactly the YAML pixel dimensions,
independent of desktop scaling. Closing the window cancels the capture.

## Configuration

All keys in `capture.yaml` are required and unknown keys are rejected:

- `width`, `height`: output dimensions (64–8192 each).
- `frames`: number of successive frames starting at time zero (1–120).
- `fps`: simulation/camera timestep, independent of actual throughput.
- `pan_degrees_per_second`: signed angular speed, including zero for a control.
- `pan_direction_degrees`: axis angle; zero is yaw, 90 is pitch.
- `quality`: motion blur 0/off, 1/Low, 2/High.
- `shutter`: OpenXvT's exposure convention, `shutter * 32 ms` (0–1).
- `reference_samples`: 0 disables the reference; otherwise 1–256 subframes.
- `output_dir`: destination for captures and reports.

MSAA is 1x. FSR, SSAO, shadows, bloom, tonemapping and output dithering are off.
The fixture uses the current motion-blur shader unchanged. When comparing shader
revisions, preserve the configuration and record the Aeron commit/diff alongside
the captures. For resolution comparisons change width and height together to
preserve aspect ratio and field of view.

## Outputs

Each frame produces `frame_NNN_sharp` and `frame_NNN_blur`:

- `.pfm`: RGB float32 linear data read from the RGBA16F render target. The PFM
  sign records endianness; file rows are bottom-to-top. No display conversion,
  exposure or normalization is applied.
- `.png`: lossless 8-bit sRGB preview, clamped to [0,1], without dithering.
  Compare at 100% pixel scale; viewer resizing can conceal patterns.

When enabled, `frame_NNN_reference` averages unblurred renders at midpoint
subframes across the **centered box shutter**. Camera poses and actual visibility
are rendered anew for each subframe; this does not use the reconstruction shader
or its velocity clamp. It is a temporal reference, not a claim of identical
filtering: our blur shader has cone weighting, approximate occlusion and a
maximum streak length. Reference error includes those model differences as well
as sampling artifacts. Increase the reference count to check convergence.

`frame_NNN_metrics.yaml` reports linear RGB RMSE, MAE, mean signed error and
maximum error on pixels where either image has a channel above 0.02. Excluding
the empty background avoids diluting the result. These are comparison metrics,
not a pure checkerboard/noise score. `capture.yaml` records the resolved settings,
backend, fixture version, exposure and image conventions.

A 1080p frame with reference produces about 75 MB of PFM data; 2160p is four times
larger. Reference capture is deliberately slow and GPU-fence synchronized. Its
wall-clock time is not a motion-blur performance benchmark.

## Suggested checks

1. Repeat the identical capture and compare PFM files byte-for-byte on the same
   backend. Cross-GPU floating-point differences need not be bit-identical.
2. Set pan speed to zero: sharp, blurred and reference images should agree.
3. Capture 1080p and 2160p with the same aspect ratio and camera settings.
4. Sweep pan direction and speed while keeping frame zero fixed.
5. Compare shader changes using identical settings and enlarged image crops.

The raw readback API rejects undersized destinations and unsupported formats.
Readback preserves RGBA8/BGRA8 or RGBA16F bytes and blocks on a fence; it is intended
for diagnostics, not normal frame presentation.
