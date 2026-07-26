# FidelityFX FSR 3.1.4

This directory contains the minimal host and shader sources needed by Aeron's
FSR 3 upscaler integration.

- Upstream: AMD FidelityFX SDK
- Tag: `v1.1.4`
- Commit: `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- Component version: FSR 3 upscaler 3.1.4
- License: MIT; see `LICENSE.txt`

The import contains the FSR 3 upscaler component, its shared host support,
required GPU headers, and the eleven HLSL upscaler passes. It intentionally omits
Cauldron, frame generation, optical flow, the public DLL API, precompiled
shader blobs, and AMD's D3D12/Vulkan backends.

Portability modifications are limited to the backend boundary:

- SRV, UAV, and constant-buffer declarations use HLSL register spaces 0, 1,
  and 2 to match SDL_GPU.
- Manifest-generated wrappers use compact per-resource-class register ranges.
- Reconstructed-depth and SPD atomics use storage buffers. Metal has no
  portable storage-texture atomic equivalent, while buffer atomics are
  available on every Aeron backend.
- SPD builds its native-wave implementation and retains the non-wave
  implementation as a whole-profile fallback on Metal, Vulkan, and D3D12.
- The luma-pyramid callback permits a compact native-wave binding containing
  only mip 5. AMD's luma algorithm does not access mips 0 through 4, and
  omitting those dead declarations keeps SDL_GPU resource slots compact.
- The callback does not define `FFX_PREFER_WAVE64`; subgroup variants use the
  device's supported native width.
- The Lanczos LUT has its own linear sampler and is declared before storage
  SRVs. SDL_GPU requires one sampler per sampled texture, and SDL_shadercross
  classifies HLSL separate images by declaration order.

The FSR filtering, reprojection, accumulation, and sharpening math is unchanged.
The shipping accumulate shader selects AMD's LUT reprojection permutation.

The manifest builds native-wave and scalar-SPD profiles in both FP16 and FP32.
The configured SDL_shadercross must expose DXC native 16-bit types and a
Vulkan 1.1 SPIR-V target so those profiles compile consistently for MSL,
SPIR-V, and DXIL.
