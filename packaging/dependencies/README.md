# Packaged dependencies

The versions and patches of the dependencies that aeron needs when a game
builds them from source for its packages. Each folder holds one dependency:
its `version` (a release, or a commit for SDL_shadercross) and the patches
that aeron relies on, applied in name order by `apply-patches.sh`.

| Folder | Version | Source |
|---|---|---|
| `sdl` | SDL3 release | `https://github.com/libsdl-org/SDL`, tag `release-<version>` |
| `shadercross` | SDL_shadercross commit | `https://github.com/libsdl-org/SDL_shadercross` |
| `zstd` | zstd release | `https://github.com/facebook/zstd`, tag `v<version>` |
| `ffmpeg` | FFmpeg release | `https://github.com/FFmpeg/FFmpeg`, tag `n<version>` |

aeron's CMake requires SDL3 3.4.16 or later.

## Patches

| Patch | Fixes |
|---|---|
| `sdl/01-gpu-d3d12-hdr.patch` | Lets the D3D12 SDL_GPU backend report and enable HDR swapchain compositions. |
| `sdl/02-gpu-d3d12-descriptor-heaps.patch` | Stops the D3D12 SDL_GPU backend from writing past a full descriptor heap, and from leaving bound tables in the old heap after a switch. Both make shaders read the wrong descriptors: blinking textures and GPU hangs under many draws (libsdl-org/SDL#16653). |
| `shadercross/01-fsr3.patch` | FSR3 shader generation support. |
| `shadercross/02-msl-bindings.patch` | Numbers Metal storage buffers after the resources a graphics shader declares, not after the textures it uses, so a shader that leaves a texture unused reads its storage buffers from the right slots. aeron's split screen PBR shaders rely on it. |

The SDL patches change only the D3D12 backend, which builds for Windows
targets, so they can be applied to every SDL build.

## Using them

Copy a dependency's folder into the build, read its `version`, check out the
source at that version and apply the patches:

```sh
version="$(cat aeron/packaging/dependencies/sdl/version)"
git clone --branch "release-${version}" --depth 1 https://github.com/libsdl-org/SDL.git SDL
aeron/packaging/dependencies/apply-patches.sh aeron/packaging/dependencies/sdl SDL
```

In a Dockerfile, copy only the folder of the dependency that a stage builds,
so that a new version or patch rebuilds only that stage:

```dockerfile
COPY aeron/packaging/dependencies/apply-patches.sh /tmp/aeron/apply-patches.sh
COPY aeron/packaging/dependencies/sdl /tmp/aeron/sdl
```

Builds that cache built dependencies should include these files in their
cache key.
