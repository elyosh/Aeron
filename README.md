# Aeron

Aeron is a portable engine foundation for games that need to combine classic
software-rendered content with a modern GPU renderer. It provides the common
runtime, rendering, media, input, and asset-processing services used by
[OpenXWA](https://github.com/elyosh/openxwa) and
[OpenTIE](https://github.com/elyosh/opentie).

## What Aeron offers

### Portable runtime

Aeron uses SDL3 to provide a native application runtime across current desktop
operating systems. It manages the window, timing, input, audio, logging, YAML
configuration, platform integration, and a virtual file system for application
resources and game data. Keyboard, mouse, text input, gamepads, and controller
rumble are exposed through a consistent per-frame input snapshot.

### Classic and modern presentation

Aeron can combine CPU-generated frames, GPU textures, and direct GPU drawing as
ordered layers in one final image. This allows a game to preserve its original
framebuffer, interface, and cockpit composition while adding GPU-rendered
elements where appropriate. It handles legacy indexed and true-color frames,
palette and color-key behavior, resolution-independent scaling, resizable
windows, and SDR or HDR output.

### GPU and scene rendering

Aeron provides a portable C graphics layer over SDL_GPU. Shared HLSL shaders
are compiled to Metal Shading Language, SPIR-V, and DXIL for the supported
graphics backends.

The shared scene renderer provides:

- glTF meshes, KTX2 textures, physically based materials, material variants,
  and articulated mesh parts;
- point lights, cascaded directional shadows, sky cubes, billboards, lens
  effects, and 2D sprite and text drawing;
- SSAO, bloom, motion blur, SDR/HDR tone mapping, and AMD FidelityFX FSR 3.1.4
  native AA and temporal upscaling.

Games remain responsible for translating their authoritative state into
Aeron's game-neutral scene and can extend its rendering passes where
game-specific behavior is required.

### Audio and video

The software audio mixer supports sound effects, positional audio, and streamed
speech or music.

The optional FFmpeg-backed video player decodes audio and video asynchronously,
reads through the virtual file system, and presents decoded frames through the
same layer compositor as the rest of the application.

### Asset pipeline

Aeron includes shared tools and libraries for preparing original and remastered
assets:

- `opt2gltf` converts LucasArts OPT models to glTF while preserving engine
  metadata such as articulated parts, hardpoints, and material variants;
- `aeron_gltf_cook` converts artist-authored glTF assets into the runtime GLB
  representation and builds material channel atlases;
- `imgbake` handles image scaling, texture and font atlases, and compressed
  KTX2 texture generation.

### Diagnostics

Optional Dear ImGui tools can be registered by a game for live diagnostics.
Aeron also names GPU resources and annotates render and compute work so frames
are easier to inspect in tools such as RenderDoc and Xcode's Metal debugger.

## Current state

Aeron is under active development alongside OpenXWA and OpenTIE. Its runtime,
renderer, media systems, and asset tools are already used by both projects.
It is developed as a shared source dependency rather than as a separately
versioned SDK, and its interfaces continue to evolve.

## Supported platforms

Aeron targets the same desktop platforms and SDL_GPU backends as the game
projects:

| Platform | Graphics backend |
|---|---|
| Windows | Direct3D 12 or Vulkan |
| macOS | Metal |
| Linux | Vulkan |

Shader sources are shared across all three platforms and compiled into the
native format required by each backend.

## Building from source

Aeron is normally included in a game repository as a Git submodule and added
with CMake's `add_subdirectory()`.

Building requires CMake 3.20 or later, a C99/C++17 toolchain, SDL3 3.4, zstd,
and SDL_shadercross. FFmpeg and pkg-config are also required when video support
is enabled.
