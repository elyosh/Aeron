# imgbake — shared encoder backend for replacement assets.
#
# Provides HD upscaling, atlas packing, KTX2 (BC7/BC6H/BC5/RGBA16F + zstd)
# encoding, PNG, JPEG and Radiance HDR I/O, environment-cube filtering, palette
# helpers, and TTF/glyph font-atlas baking.
#
# Image containers (Image8/AnimImage) come from delt.h/anim.h as TYPES
# only; the DELT/ANIM decoders stay game-side (each baker fills the
# containers from its own source format). Host build only.

enable_language(CXX)

set(IMGBAKE_TP ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

# BC6H uses the ISPC Texture Compressor (third_party/ispc_texcomp), whose
# kernel is compiled by the ISPC compiler for the host CPU; it is
# imgbake_bc6h, apart, so that the libraries without it need no ISPC. It is
# host tooling only: package builds (tools off) configure without ISPC and
# never build it, nor does the add-on's cooker (AERON_GLTF_COOK_STANDALONE).
if(DEFINED AERON_ISPC_EXECUTABLE AND AERON_ISPC_EXECUTABLE STREQUAL "")
    unset(AERON_ISPC_EXECUTABLE CACHE)
endif()
find_program(AERON_ISPC_EXECUTABLE NAMES ispc
    DOC "Path to the ISPC compiler (1.30 or later) used for imgbake's BC6H encoder")
set(IMGBAKE_ISPC_DIR ${CMAKE_CURRENT_BINARY_DIR}/ispc)
set(IMGBAKE_ISPC_OBJ ${IMGBAKE_ISPC_DIR}/kernel${CMAKE_C_OUTPUT_EXTENSION})
set(IMGBAKE_ISPC_HEADER ${IMGBAKE_ISPC_DIR}/kernel_ispc.h)
set(IMGBAKE_ISPC_OUTPUTS ${IMGBAKE_ISPC_OBJ})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(IMGBAKE_ISPC_FLAGS --arch=aarch64 --target=neon-i32x8)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # Multi-target: ISPC adds per-target objects and a runtime dispatcher.
    set(IMGBAKE_ISPC_FLAGS --arch=x86-64 --target=sse4-i32x4,avx2-i32x8)
    list(APPEND IMGBAKE_ISPC_OUTPUTS
        ${IMGBAKE_ISPC_DIR}/kernel_sse4${CMAKE_C_OUTPUT_EXTENSION}
        ${IMGBAKE_ISPC_DIR}/kernel_avx2${CMAKE_C_OUTPUT_EXTENSION})
else()
    set(IMGBAKE_ISPC_FLAGS)
endif()
if(NOT WIN32)
    list(APPEND IMGBAKE_ISPC_FLAGS --pic)
endif()
if(AERON_ISPC_EXECUTABLE AND IMGBAKE_ISPC_FLAGS)
    add_custom_command(
        OUTPUT ${IMGBAKE_ISPC_OUTPUTS} ${IMGBAKE_ISPC_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${IMGBAKE_ISPC_DIR}
        COMMAND ${AERON_ISPC_EXECUTABLE} ${IMGBAKE_ISPC_FLAGS} -O2 --woff
                ${IMGBAKE_TP}/ispc_texcomp/kernel.ispc
                -o ${IMGBAKE_ISPC_OBJ} -h ${IMGBAKE_ISPC_HEADER}
        DEPENDS ${IMGBAKE_TP}/ispc_texcomp/kernel.ispc
        COMMENT "ISPC texture compressor kernel"
        VERBATIM)
else()
    set(imgbake_ispc_problem "the ISPC compiler was not found (brew install ispc, the distribution's ispc package, or a release from https://github.com/ispc/ispc/releases; or set AERON_ISPC_EXECUTABLE)")
    if(AERON_ISPC_EXECUTABLE)
        set(imgbake_ispc_problem "ISPC has no target for processor '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
    if(AERON_BUILD_TOOLS AND NOT AERON_GLTF_COOK_STANDALONE)
        message(FATAL_ERROR "imgbake: ${imgbake_ispc_problem}")
    endif()
    message(STATUS "imgbake: ${imgbake_ispc_problem}; imgbake_bc6h cannot be built")
    add_custom_command(
        OUTPUT ${IMGBAKE_ISPC_OUTPUTS} ${IMGBAKE_ISPC_HEADER}
        COMMAND ${CMAKE_COMMAND} -E echo "imgbake: ${imgbake_ispc_problem}"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()

add_library(imgbake STATIC
    # C: agnostic image/atlas/font backend.
    upscale.c
    atlas_pack.c
    ktx2_writer.c
    png_write.c
    png_write_atomic.c
    png_read.c
    ldr_read.c
    hdr_read.c
    envmap.c
    palette.c
    font_atlas.c
    font_atlas_load.c
    # C++: BC codec facades over vendored encoders.
    bc7_codec.cpp
    bc5_codec.cpp
    ${IMGBAKE_TP}/bc7enc/bc7enc.cpp   # BC7 (bc7_codec)
    ${IMGBAKE_TP}/bc7enc/rgbcx.cpp)   # BC5  (bc5_codec)

target_include_directories(imgbake
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..    # third_party/bc7enc/...
        ${IMGBAKE_TP}/bc7enc                 # rgbcx.h, bc7enc.h
        ${IMGBAKE_TP}/stb)                   # stb_image*/truetype

# zstd — KTX2 supercompressionScheme=2 (aeron already requires it).
# Threads — the BC7 encoder splits large images across threads.
find_package(Threads REQUIRED)
target_link_libraries(imgbake PUBLIC aeron ${AERON_ZSTD} PRIVATE Threads::Threads)

set_target_properties(imgbake PROPERTIES C_STANDARD 11)
target_compile_features(imgbake PUBLIC cxx_std_17)

# The BC6H encoder and the BC6H cubemap writer (environment_bake).
add_library(imgbake_bc6h STATIC
    ktx2_writer_bc6h.c
    bc6h_codec.cpp
    ${IMGBAKE_TP}/ispc_texcomp/ispc_texcomp.cpp  # BC6H (bc6h_codec)
    ${IMGBAKE_ISPC_HEADER}
    ${IMGBAKE_ISPC_OUTPUTS})
target_include_directories(imgbake_bc6h PRIVATE
    ${IMGBAKE_TP}/ispc_texcomp           # ispc_texcomp.h
    ${IMGBAKE_ISPC_DIR})                 # kernel_ispc.h (generated)
target_link_libraries(imgbake_bc6h PUBLIC imgbake PRIVATE Threads::Threads)
set_target_properties(imgbake_bc6h PROPERTIES C_STANDARD 11)

# Vendored encoders trip the project's strict warnings (sign compares,
# unused locals, MSVC pragmas); they are correct upstream — suppress
# rather than patch. The encoders are tight numeric loops: force -O3 (this
# is build-host tooling). The BC6H kernel itself is compiled by ISPC.
set_source_files_properties(
    bc7_codec.cpp bc5_codec.cpp bc6h_codec.cpp
    ${IMGBAKE_TP}/bc7enc/bc7enc.cpp
    ${IMGBAKE_TP}/bc7enc/rgbcx.cpp
    ${IMGBAKE_TP}/ispc_texcomp/ispc_texcomp.cpp
    PROPERTIES COMPILE_FLAGS "-w -O3")

# Environment filtering integrates millions of samples per cube; keep it
# optimized in debug builds too (host tooling).
if(NOT MSVC)
    set_source_files_properties(envmap.c PROPERTIES COMPILE_OPTIONS "-O3")
endif()
