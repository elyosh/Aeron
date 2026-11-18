# Packaged dependencies

The versions and patches of the dependencies that aeron needs when a game
builds them from source for its packages. Each folder holds one dependency:
its `version` (a release, or a commit for SDL and SDL_shadercross) and the
patches that aeron relies on, applied in name order by `apply-patches.sh`.

| Folder | Version | Source |
|---|---|---|
| `sdl` | SDL3 commit or release | `https://github.com/libsdl-org/SDL`, a commit of `main` or tag `release-<version>` |
| `shadercross` | SDL_shadercross commit | `https://github.com/libsdl-org/SDL_shadercross` |
| `zstd` | zstd release | `https://github.com/facebook/zstd`, tag `v<version>` |
| `ffmpeg` | FFmpeg release | `https://github.com/FFmpeg/FFmpeg`, tag `n<version>` |

## SDL

Every build of aeron uses the same SDL: packages, CI and local builds, on
Windows, Linux and macOS. `sdl/version` pins a commit of SDL's `main` branch,
because the OpenXR API that aeron's VR support needs is not in an SDL release
yet. When a release has it, `sdl/version` will name that release.

`build-sdl.sh` builds this SDL. It clones SDL at the pinned version, applies
the patches, and builds and installs a shared library:

```sh
aeron/packaging/dependencies/build-sdl.sh <install prefix> [CMake options...]
cmake -S . -B build -DCMAKE_PREFIX_PATH=<install prefix>
```

Extra options go to SDL's CMake configure step, for example
`-DCMAKE_TOOLCHAIN_FILE=...` for a cross build. `--source <dir>` and
`--build <dir>` choose where the checkout and the build tree are kept. Don't
use a system SDL, such as Homebrew's or a distribution package.

aeron's CMake stops when the SDL it finds is not the pinned one. For a pinned
commit, it looks for the commit in `SDL_REVISION` of SDL's
`SDL3/SDL_revision.h`. `build-sdl.sh` writes the full commit there. SDL's own
build, from a shallow clone, would only write the first 7 characters. For a
pinned release, the check compares the version number.
`-DAERON_ALLOW_OTHER_SDL=ON` turns the error into a warning. It is for
distribution packagers who must use their own SDL. Such an SDL needs the
OpenXR API, or `-DAERON_XR=OFF`.

## OpenXR

`AERON_XR` builds aeron's VR support. It is on by default on Windows and
Linux, and off on macOS, where SDL has no OpenXR support. aeron vendors the
OpenXR headers in `third_party/openxr` and does not link the OpenXR loader:
SDL loads it by name at run time, `openxr_loader.dll` on Windows and
`libopenxr_loader.so.1` on Linux. A Windows package ships the loader from the
Khronos `OpenXR.Loader` NuGet package of the same OpenXR SDK release as the
vendored headers. A Linux package uses the system's loader.

## Patches

| Patch | Fixes |
|---|---|
| `sdl/01-gpu-d3d12-hdr.patch` | Lets the D3D12 SDL_GPU backend report and enable HDR swapchain compositions. |
| `sdl/02-gpu-d3d12-descriptor-heaps.patch` | Stops the D3D12 SDL_GPU backend from writing past a full descriptor heap, and from leaving bound tables in the old heap after a switch. Both make shaders read the wrong descriptors: blinking textures and GPU hangs under many draws (libsdl-org/SDL#16653). |
| `sdl/03-gpu-vulkan-xr-swapchain-levels.patch` | Sets the level and layer counts of the Vulkan textures that wrap OpenXR swapchain images. Without them, the layout change that SDL records when it creates the swapchain covers zero levels and layers, which is not valid. |
| `sdl/04-gpu-d3d12-xr-swapchain-destroy.patch` | Makes the D3D12 SDL_GPU backend destroy the images of an OpenXR swapchain as the texture containers they are. It treated each container as a texture, so destroying a swapchain read wrong memory, and the application crashed when it closed a VR session. |
| `shadercross/01-fsr3.patch` | FSR3 shader generation support. |
| `shadercross/02-msl-bindings.patch` | Numbers Metal storage buffers after the resources a graphics shader declares, not after the textures it uses, so a shader that leaves a texture unused reads its storage buffers from the right slots. aeron's split screen PBR shaders rely on it. |

The first two SDL patches change only the D3D12 backend, which builds for
Windows targets, and the third only the Vulkan backend's OpenXR code. They
can be applied to every SDL build.

## Using them

For SDL, run `build-sdl.sh`. For another dependency, copy its folder into the
build, read its `version`, check out the source at that version and apply the
patches:

```sh
commit="$(cat aeron/packaging/dependencies/shadercross/version)"
git clone https://github.com/libsdl-org/SDL_shadercross.git SDL_shadercross
git -C SDL_shadercross checkout "${commit}"
aeron/packaging/dependencies/apply-patches.sh aeron/packaging/dependencies/shadercross SDL_shadercross
```

In a Dockerfile, copy only the scripts and the folder of the dependency that a
stage builds, so that a new version or patch rebuilds only that stage:

```dockerfile
COPY aeron/packaging/dependencies/apply-patches.sh /tmp/aeron/apply-patches.sh
COPY aeron/packaging/dependencies/build-sdl.sh /tmp/aeron/build-sdl.sh
COPY aeron/packaging/dependencies/sdl /tmp/aeron/sdl
RUN /tmp/aeron/build-sdl.sh --source /deps/SDL --build /build/sdl /opt/prefix
```

Builds that cache built dependencies should include these files in their
cache key.
