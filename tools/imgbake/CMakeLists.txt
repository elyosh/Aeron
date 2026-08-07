# imgbake — shared remaster-asset encoder backend.
#
# Provides HD upscaling, atlas packing, KTX2 (BC7/BC6H/BC5 + zstd)
# encoding, PNG I/O, palette helpers, and TTF/glyph font-atlas baking.
#
# Image containers (Image8/AnimImage) come from delt.h/anim.h as TYPES
# only; the DELT/ANIM decoders stay game-side (each baker fills the
# containers from its own source format). Host build only.

enable_language(CXX)

set(IMGBAKE_TP ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

add_library(imgbake STATIC
    # C: agnostic image/atlas/font backend.
    upscale.c
    atlas_pack.c
    ktx2_writer.c
    png_write.c
    png_read.c
    palette.c
    font_atlas.c
    font_atlas_load.c
    # C++: BC codec facades over vendored encoders.
    bc7_codec.cpp
    bc5_codec.cpp
    bc6h_codec.cpp
    ${IMGBAKE_TP}/bc7enc/bc7enc.cpp   # BC7 (bc7_codec)
    ${IMGBAKE_TP}/bc7enc/rgbcx.cpp    # BC5  (bc5_codec)
    ${IMGBAKE_TP}/dxtex/BC6HBC7.cpp)  # BC6H (bc6h_codec, DirectXTex)

target_include_directories(imgbake
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..    # third_party/{bc7enc,dxtex}/...
        ${IMGBAKE_TP}/bc7enc                 # rgbcx.h, bc7enc.h
        ${IMGBAKE_TP}/dxtex                  # BC.h, dxtex_compat.h
        ${IMGBAKE_TP}/dxmath                 # DirectXMath.h + sal.h shim
        ${IMGBAKE_TP}/stb)                   # stb_image*/truetype

# zstd — KTX2 supercompressionScheme=2 (aeron already requires it).
target_link_libraries(imgbake PUBLIC aeron zstd::libzstd)

set_target_properties(imgbake PROPERTIES C_STANDARD 11)
target_compile_features(imgbake PUBLIC cxx_std_17)

# Vendored encoders trip the project's strict warnings (sign compares,
# unused locals, MSVC pragmas); they are correct upstream — suppress
# rather than patch. BC6HBC7 is a tight numeric loop: force -O3 (it is
# ~100x slower unoptimized, and this is build-host tooling).
set_source_files_properties(
    bc7_codec.cpp bc5_codec.cpp bc6h_codec.cpp
    ${IMGBAKE_TP}/bc7enc/bc7enc.cpp
    ${IMGBAKE_TP}/bc7enc/rgbcx.cpp
    ${IMGBAKE_TP}/dxtex/BC6HBC7.cpp
    PROPERTIES COMPILE_FLAGS "-w -O3")
