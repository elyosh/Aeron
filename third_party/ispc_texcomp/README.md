# ISPC Texture Compressor

BC6H encoder used by `tools/imgbake/bc6h_codec.cpp`.

- Source: https://github.com/Traverse-Research/ISPCTextureCompressor
  (maintained fork of Intel's ISPC Texture Compressor)
- Commit: `d80d4a7af5f63940f479bc0ebe6469cdb32150cc`
- Files: `kernel.ispc`, `ispc_texcomp.cpp`, `ispc_texcomp.h`, unmodified.
  The ASTC files (`kernel_astc.ispc`, `ispc_texcomp_astc.cpp`) are not
  vendored; `ispc_texcomp.h` still declares their entry points.
- License: MIT (`LICENSE`).

`kernel.ispc` is compiled by the ISPC compiler (1.30 or later) at build
time: NEON on arm64, SSE4 and AVX2 with runtime dispatch on x86-64. See
`tools/imgbake/CMakeLists.txt`.
