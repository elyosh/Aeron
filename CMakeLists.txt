cmake_minimum_required(VERSION 3.20)

project(aeron C CXX)

# Aeron — SDL3-based portable engine (window, GPU HAL, layer composition,
# input snapshot, audio mixer, VFS, time, log, config). Consumed either
# standalone or via add_subdirectory() from a game repository.

option(AERON_DEBUG_UI "Build the Dear ImGui debug overlay into aeron" OFF)
option(AERON_HOST_TOOLS_ONLY "Build only tools that must run on the build host" OFF)

find_package(SDL3 3.4 REQUIRED CONFIG COMPONENTS SDL3-shared)

set(AERON_YAML_SOURCES
    third_party/libyaml/src/api.c
    third_party/libyaml/src/dumper.c
    third_party/libyaml/src/emitter.c
    third_party/libyaml/src/loader.c
    third_party/libyaml/src/parser.c
    third_party/libyaml/src/reader.c
    third_party/libyaml/src/scanner.c
    third_party/libyaml/src/writer.c
)

add_library(aeron_yaml STATIC ${AERON_YAML_SOURCES})

target_include_directories(aeron_yaml
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyaml/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyaml/src
)

target_compile_definitions(aeron_yaml
    PUBLIC
        YAML_DECLARE_STATIC
    PRIVATE
        YAML_VERSION_MAJOR=0
        YAML_VERSION_MINOR=2
        YAML_VERSION_PATCH=5
        YAML_VERSION_STRING="0.2.5"
)

set_target_properties(aeron_yaml PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

function(aeron_add_fsr3_shadergen)
    add_executable(aeron_fsr3_shadergen
        tools/fsr3_shadergen/fsr3_shadergen.cpp
    )
    target_compile_features(aeron_fsr3_shadergen PRIVATE cxx_std_17)
    target_link_libraries(aeron_fsr3_shadergen PRIVATE aeron_yaml SDL3::SDL3)
    if(MSVC)
        target_compile_options(aeron_fsr3_shadergen PRIVATE /W4)
    else()
        target_compile_options(aeron_fsr3_shadergen PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(AERON_HOST_TOOLS_ONLY)
    aeron_add_fsr3_shadergen()
    install(TARGETS aeron_fsr3_shadergen RUNTIME DESTINATION bin)
    return()
endif()

# libzstd — KTX2 supercompressionScheme=2 path in aeron_scene's reader.
find_package(zstd REQUIRED CONFIG)

# SDL 3.4 can request optional Vulkan feature chains during GPU-device
# creation. Keep Vulkan headers optional so Metal- and D3D12-only builds do
# not acquire a Vulkan SDK dependency.
find_path(AERON_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h)

# find_program skips its search whenever the cache entry already holds a value,
# so the entry must never be seeded ahead of it. An empty user-supplied value
# carries no path, so drop it and let the PATH lookup run.
if(DEFINED AERON_SHADERCROSS_EXECUTABLE AND AERON_SHADERCROSS_EXECUTABLE STREQUAL "")
    unset(AERON_SHADERCROSS_EXECUTABLE CACHE)
endif()
find_program(AERON_SHADERCROSS_EXECUTABLE NAMES shadercross
    DOC "Path to the host-native SDL_shadercross command line compiler")

if(NOT EXISTS "${AERON_SHADERCROSS_EXECUTABLE}")
    message(FATAL_ERROR
        "SDL_shadercross was not found; set AERON_SHADERCROSS_EXECUTABLE or add shadercross to PATH")
endif()

if(NOT DEFINED AERON_SHADER_FORMATS)
    if(APPLE)
        set(AERON_SHADER_FORMATS MSL)
    elseif(WIN32)
        set(AERON_SHADER_FORMATS DXIL SPIRV)
    else()
        set(AERON_SHADER_FORMATS SPIRV)
    endif()
endif()
set(AERON_SHADER_FORMATS "${AERON_SHADER_FORMATS}" CACHE STRING
    "Runtime shader formats to build: MSL, SPIRV, and/or DXIL")

set(aeron_validated_shader_formats)
foreach(format IN LISTS AERON_SHADER_FORMATS)
    string(TOUPPER "${format}" format)
    if(NOT format STREQUAL "MSL" AND NOT format STREQUAL "SPIRV" AND NOT format STREQUAL "DXIL")
        message(FATAL_ERROR
            "Unsupported Aeron shader format '${format}'; expected MSL, SPIRV, or DXIL")
    endif()
    list(APPEND aeron_validated_shader_formats "${format}")
endforeach()
list(REMOVE_DUPLICATES aeron_validated_shader_formats)
if(NOT aeron_validated_shader_formats)
    message(FATAL_ERROR "AERON_SHADER_FORMATS must contain at least one shader format")
endif()
set(AERON_SHADER_FORMATS ${aeron_validated_shader_formats})
list(JOIN AERON_SHADER_FORMATS ", " aeron_shader_formats_display)
list(JOIN AERON_SHADER_FORMATS "," AERON_SHADER_FORMATS_CSV)
message(STATUS "Aeron shader formats: ${aeron_shader_formats_display}")

# Compiles each .vert.hlsl / .frag.hlsl / .comp.hlsl file to the configured
# runtime formats in output_dir, appending the generated paths to the
# parent-scope variable named by out_files_var. Consumers call this and hang a
# custom target off the output list.
function(aeron_compile_hlsl out_files_var output_dir)
    set(generated)
    foreach(hlsl IN LISTS ARGN)
        get_filename_component(base ${hlsl} NAME)
        if(base MATCHES "\\.vert\\.hlsl$")
            set(stage vertex)
        elseif(base MATCHES "\\.frag\\.hlsl$")
            set(stage fragment)
        elseif(base MATCHES "\\.comp\\.hlsl$")
            set(stage compute)
        else()
            message(FATAL_ERROR "Cannot infer shader stage from ${hlsl}")
        endif()
        string(REGEX REPLACE "\\.hlsl$" "" stem ${base})
        # Includes are shared building blocks; depend on all of them so
        # an .hlsli edit rebuilds every shader (a stale consumer of a
        # changed cbuffer LAYOUT reads garbage — cheap to over-rebuild).
        file(GLOB shader_includes ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli)
        set(outputs)
        set(commands)
        foreach(format IN LISTS AERON_SHADER_FORMATS)
            if(format STREQUAL "MSL")
                set(extension msl)
            elseif(format STREQUAL "SPIRV")
                set(extension spv)
            else()
                set(extension dxil)
            endif()
            list(APPEND outputs ${output_dir}/${stem}.${extension})
            list(APPEND commands
                COMMAND ${AERON_SHADERCROSS_EXECUTABLE} ${hlsl}
                    --source HLSL --dest ${format} --stage ${stage} --entrypoint main
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                    --output ${output_dir}/${stem}.${extension}
            )
        endforeach()
        add_custom_command(
            OUTPUT ${outputs}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
            ${commands}
            DEPENDS ${hlsl} ${shader_includes}
            VERBATIM
            COMMENT "shadercross HLSL: ${base}"
        )
        list(APPEND generated ${outputs})
    endforeach()
    set(${out_files_var} ${generated} PARENT_SCOPE)
endfunction()

# Bakes a TTF into the <basename>.fnt + <basename>.png atlas pair consumed by
# AeronFontAtlas_Load, using the fontbake tool at build time. Sets the
# parent-scope variable named by out_files_var to the two generated paths so
# consumers can hang a custom target off them. Requires AERON_BUILD_TOOLS.
function(aeron_bake_font out_files_var output_dir ttf basename cell_h)
    set(outputs
        ${output_dir}/${basename}.fnt
        ${output_dir}/${basename}.png)
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND $<TARGET_FILE:fontbake> ${ttf} ${output_dir}/${basename}
            --cell-h ${cell_h}
        DEPENDS fontbake ${ttf}
        VERBATIM
        COMMENT "fontbake TTF: ${basename}"
    )
    set(${out_files_var} ${outputs} PARENT_SCOPE)
endfunction()

set(AERON_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)

set(AERON_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.frag.hlsl
    # aeron_scene "pbr" material class (C2b) — forward, prepass, and
    # alpha-tested velocity-stamp variants for cooked glTF meshes.
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_mesh.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_mesh.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_mesh_mask.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass_mask.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass_mask.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass_stamp.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_prepass_stamp.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_clustered_lights.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_directional_shadow.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_directional_shadow.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_directional_shadow_mask.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_directional_shadow_mask.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_directional_shadow_debug.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mesh_overlay.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mesh_overlay.frag.hlsl
    # aeron_scene sky cube (AeronScene_SetSkyCube) — fullscreen-triangle
    # cube-map sky under all meshes/billboards, promoted from the TIE
    # shell (flight_skybox.*).
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_sky_cube.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_sky_cube.frag.hlsl
    # aeron_scene bloom chain + the shared fullscreen-quad VS.
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_fullscreen_quad.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_bloom_brightpass.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_bloom_downsample.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_bloom_upsample.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_tonemap.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_tonemap_hdr.frag.hlsl
    # aeron_scene SSAO + motion-blur chains (scene-internal post stack).
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ssao.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ssao_blur.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ssao_blur_lq.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_camera_fill.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_velocity_viz.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_temporal_velocity.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_temporal_tilemax.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_fsr_tilemax.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_tilemax.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_reconstruct.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_mb_neighbormax.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_temporal_copy.frag.hlsl
    # aeron_scene 2D blit layer (C3a) — batched instanced rect quads +
    # the 4-corner projective variant, moved from the TIE shell
    # (gpu_blit.vert/.frag, gpu_blit4.vert).
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_blit.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_blit.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d_vel.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d_vel.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d_lens.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_billboard3d_lens.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_blit4.vert.hlsl
    # aeron_scene UI layer — analytic rounded-rect surfaces (fills,
    # gradients, borders, bevels, focus outlines, drop shadows).
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ui_rect.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ui_rect.frag.hlsl
    # Compute HAL validation and fallback storage clear building block.
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_fill.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_fill_buffer.comp.hlsl
)

if(AERON_DEBUG_UI)
    list(APPEND AERON_SHADER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_mesh_debug.frag.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_pbr_mesh_mask_debug.frag.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_ssao_debug.frag.hlsl
    )
endif()

aeron_compile_hlsl(AERON_SHADER_OUTPUTS ${AERON_SHADER_OUTPUT_DIR} ${AERON_SHADER_SOURCES})
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AERON_SHADER_OUTPUTS ${AERON_SHADER_OUTPUTS} PARENT_SCOPE)
endif()

set(AERON_FSR3_SHADERGEN_EXECUTABLE "" CACHE FILEPATH
    "Optional host-native Aeron FSR shader generator for cross-compiling")
if(AERON_FSR3_SHADERGEN_EXECUTABLE)
    if(NOT EXISTS "${AERON_FSR3_SHADERGEN_EXECUTABLE}")
        message(FATAL_ERROR
            "AERON_FSR3_SHADERGEN_EXECUTABLE does not exist: ${AERON_FSR3_SHADERGEN_EXECUTABLE}")
    endif()
    set(AERON_FSR3_SHADERGEN_COMMAND ${AERON_FSR3_SHADERGEN_EXECUTABLE})
    set(AERON_FSR3_SHADERGEN_DEPENDS ${AERON_FSR3_SHADERGEN_EXECUTABLE})
else()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "Cross-compiling requires a host-native AERON_FSR3_SHADERGEN_EXECUTABLE")
    endif()
    aeron_add_fsr3_shadergen()
    set(AERON_FSR3_SHADERGEN_COMMAND $<TARGET_FILE:aeron_fsr3_shadergen>)
    set(AERON_FSR3_SHADERGEN_DEPENDS aeron_fsr3_shadergen)
endif()

set(AERON_FSR3_MANIFEST
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr3/fsr3_shaders.yaml)
set(AERON_FSR3_GPU_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr3/include/FidelityFX/gpu)
set(AERON_FSR3_GENERATED_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/fsr3)
file(GLOB_RECURSE AERON_FSR3_GPU_HEADERS
    ${AERON_FSR3_GPU_INCLUDE}/*.h)

add_custom_command(
    OUTPUT ${AERON_FSR3_GENERATED_DIR}/fsr3_shader_bundle.stamp
    COMMAND ${AERON_FSR3_SHADERGEN_COMMAND}
        --manifest ${AERON_FSR3_MANIFEST}
        --gpu-include ${AERON_FSR3_GPU_INCLUDE}
        --generated-dir ${AERON_FSR3_GENERATED_DIR}
        --shader-output-dir ${AERON_SHADER_OUTPUT_DIR}
        --shadercross ${AERON_SHADERCROSS_EXECUTABLE}
        --formats ${AERON_SHADER_FORMATS_CSV}
    BYPRODUCTS
        ${AERON_FSR3_GENERATED_DIR}/include/aeron_fsr3_shader_manifest.generated.h
        ${AERON_FSR3_GENERATED_DIR}/fsr3_shader_inventory.yaml
    DEPENDS
        ${AERON_FSR3_SHADERGEN_DEPENDS}
        ${AERON_FSR3_MANIFEST}
        ${AERON_FSR3_GPU_HEADERS}
        ${AERON_SHADERCROSS_EXECUTABLE}
    VERBATIM
    COMMENT "Generating and validating FSR 3.1.4 shader variants"
)
add_custom_target(aeron_fsr3_shaders
    DEPENDS ${AERON_FSR3_GENERATED_DIR}/fsr3_shader_bundle.stamp)

add_custom_target(aeron_shaders DEPENDS ${AERON_SHADER_OUTPUTS})
add_dependencies(aeron_shaders aeron_fsr3_shaders)

set(AERON_SOURCES
    src/aeron.c
    src/atlas_pack.c
    src/audio.c
    src/config_file.c
    src/dialog.c
    src/events.c
    src/gamepad.c
    src/image.c
    src/input.c
    src/log.c
    src/mesh_normals.c
    src/numeric.c
    src/paths.c
    src/render_backend.c
    src/surface.c
    src/sync.c
    src/time.c
    src/vfs.c
    src/window.c
)

add_library(aeron ${AERON_SOURCES})

set_target_properties(aeron PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_include_directories(aeron
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

foreach(format IN LISTS AERON_SHADER_FORMATS)
    target_compile_definitions(aeron PRIVATE AERON_SHADER_FORMAT_${format}=1)
endforeach()

if(SDL3_VERSION VERSION_GREATER_EQUAL "3.4.0" AND AERON_VULKAN_INCLUDE_DIR)
    target_include_directories(aeron PRIVATE ${AERON_VULKAN_INCLUDE_DIR})
    target_compile_definitions(aeron PRIVATE AERON_HAS_VULKAN_FP16_DEVICE_OPTIONS=1)
endif()

# SDL_GPU capture annotations default to the compiler's debug/release policy
# (enabled without NDEBUG, disabled with it). Profiling optimized builds can
# force them on without changing source.
set(AERON_GPU_DEBUG_LABELS "AUTO" CACHE STRING
    "SDL_GPU debug labels: AUTO, ON, or OFF")
set_property(CACHE AERON_GPU_DEBUG_LABELS PROPERTY STRINGS AUTO ON OFF)
if(AERON_GPU_DEBUG_LABELS STREQUAL "ON")
    target_compile_definitions(aeron PRIVATE AERON_GPU_DEBUG_LABELS=1)
elseif(AERON_GPU_DEBUG_LABELS STREQUAL "OFF")
    target_compile_definitions(aeron PRIVATE AERON_GPU_DEBUG_LABELS=0)
elseif(NOT AERON_GPU_DEBUG_LABELS STREQUAL "AUTO")
    message(FATAL_ERROR "AERON_GPU_DEBUG_LABELS must be AUTO, ON, or OFF")
endif()

target_link_libraries(aeron
    PUBLIC
        SDL3::SDL3
    PRIVATE
        aeron_yaml
)

add_dependencies(aeron aeron_shaders)

option(AERON_ENABLE_VIDEO "Build the FFmpeg-backed Aeron video player" ON)
if(AERON_ENABLE_VIDEO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample
    )

    add_library(aeron_video
        src/video.c
        src/video_decode.c
    )
    set_target_properties(aeron_video PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_include_directories(aeron_video
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(aeron_video
        PUBLIC
            aeron
        PRIVATE
            PkgConfig::FFMPEG
            SDL3::SDL3
    )
    if(MSVC)
        target_compile_options(aeron_video PRIVATE /W4)
    else()
        target_compile_options(aeron_video PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# AMD FidelityFX FSR 3.1.4 host implementation. The public Aeron API remains C;
# this private target contains AMD's C++ host scheduler and object management.
add_library(aeron_fsr3_host STATIC
    third_party/fidelityfx-fsr3/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    third_party/fidelityfx-fsr3/src/shared/ffx_assert.cpp
    third_party/fidelityfx-fsr3/src/shared/ffx_message.cpp
    third_party/fidelityfx-fsr3/src/shared/ffx_object_management.cpp
)
target_include_directories(aeron_fsr3_host
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr3/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr3/src/shared
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr3/src/components
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fsr3
)
target_compile_features(aeron_fsr3_host PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(aeron_fsr3_host PRIVATE /W3)
else()
    target_compile_options(aeron_fsr3_host PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

# Reusable SDL_GPU backend for the FSR 3.1.4 host scheduler. The public
# temporal-upscaler facade is added to this target below.
add_library(aeron_fsr3 STATIC
    src/fsr3/backend.cpp
    src/fsr3/temporal.cpp
)
target_include_directories(aeron_fsr3
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fsr3
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr3/include
        ${AERON_FSR3_GENERATED_DIR}/include
)
target_compile_features(aeron_fsr3 PUBLIC cxx_std_17)
target_link_libraries(aeron_fsr3 PUBLIC aeron PRIVATE aeron_fsr3_host)
add_dependencies(aeron_fsr3 aeron_fsr3_shaders)
if(MSVC)
    target_compile_options(aeron_fsr3 PRIVATE /W4)
else()
    target_compile_options(aeron_fsr3 PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-braces)
endif()

if(MSVC)
    target_compile_options(aeron PRIVATE /W4)
else()
    target_compile_options(aeron PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
endif()

# ---------- Debug UI (optional Dear ImGui overlay) ----------
#
# Compiles the ImGui context host + SDL3/SDL_GPU backends into the aeron
# library and defines AERON_DEBUG_UI on its PUBLIC interface (the
# aeron/debug.h stubs vs. real API switch on it, so consumers must
# see the same definition state). Games register tool windows through
# Aeron_DebugRegisterTool; tool code includes <imgui.h>, exported below.
if(AERON_DEBUG_UI)
    enable_language(CXX)

    target_sources(aeron PRIVATE
        src/debug_ui.cpp
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl3.cpp
        third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
    )

    # PUBLIC imgui root so game-side tool code resolves <imgui.h> (and
    # ImGui backends stay an aeron implementation detail under backends/).
    target_include_directories(aeron
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
    )

    target_compile_definitions(aeron PUBLIC AERON_DEBUG_UI=1)
    target_compile_features(aeron PUBLIC cxx_std_17)

    # ImGui doesn't compile clean under the strict warning set; relax the
    # noisiest categories for the C++ TUs only.
    set_source_files_properties(
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl3.cpp
        third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
        PROPERTIES COMPILE_OPTIONS
        "-Wno-pedantic;-Wno-unused-but-set-variable;-Wno-unused-result"
    )
endif()

# ---------- aeron_scene — shared mid-level scene / asset layer ----------
#
# Phase C of the shared-architecture migration: game-agnostic asset and
# scene modules extracted from the TIE remaster shell. Games translate
# their own snapshots into these primitives; game-specific shading and
# formats stay game-side. Grows toward AeronScene3D / AeronDrawList2D;
# current contents are the C1 asset layer.
add_library(aeron_cgltf STATIC
    tools/gltf_cook/cgltf_impl.c
)
target_include_directories(aeron_cgltf PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
)
set_target_properties(aeron_cgltf PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

add_library(lucas_opt STATIC
    tools/opt2gltf/opt.c
)
target_include_directories(lucas_opt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/opt2gltf
)
set_target_properties(lucas_opt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_library(opt2gltf_lib STATIC
    tools/opt2gltf/opt2gltf.c
)
target_include_directories(opt2gltf_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/opt2gltf
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
target_link_libraries(opt2gltf_lib PUBLIC lucas_opt aeron_cgltf aeron)
if(NOT MSVC)
    target_link_libraries(opt2gltf_lib PUBLIC m)
endif()
set_target_properties(opt2gltf_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_library(aeron_scene STATIC
    src/scene/ktx2_reader.c
    src/scene/image_cache.c
    src/scene/sprite_atlas.c
    src/scene/blend_ramp.c
    src/scene/gltf_mesh.c
    src/scene/scene3d.c
    src/scene/clustered_lights.c
    src/scene/billboards3d.c
    src/scene/mesh.c
    src/scene/mesh_overlay.c
    src/scene/directional_shadow.c
    src/scene/pbr.c
    src/scene/bloom.c
    src/scene/util.c
    src/scene/present.c
    src/scene/post.c
    src/scene/temporal.c
    src/scene/draw_list2d.c
    src/scene/font_atlas.c
    src/scene/rgba_upload.c
    src/scene/runtime_atlas.c
    src/scene/text2d.c
    src/scene/ui/containers.c
    src/scene/ui/context.c
    src/scene/ui/draw.c
    src/scene/ui/focus.c
    src/scene/ui/layout.c
    src/scene/ui/rebind.c
    src/scene/ui/submit.c
    src/scene/ui/theme.c
    src/scene/ui/widgets.c
    src/scene/ui/window.c
)

set_target_properties(aeron_scene PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_include_directories(aeron_scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_link_libraries(aeron_scene
    PUBLIC
        aeron
    PRIVATE
        aeron_cgltf
        aeron_yaml
        aeron_fsr3
        zstd::libzstd
)

if(MSVC)
    target_compile_options(aeron_scene PRIVATE /W4)
else()
    target_compile_options(aeron_scene PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
endif()

# opt2gltf — 1998 LucasArts OPT ship models -> glTF 2.0 + .bin + .png,
# preserving the engine-specific data the scene renderer reads
# (mesh_type, rotation axes/pivots, hardpoints, paint-scheme variants
# via KHR_materials_variants). Moved from the TIE repo (plan E1) so
# both game asset pipelines share one converter. The host tool uses Aeron's
# CPU-only normal builder and owns its copy of the OPT parser.
option(AERON_BUILD_TOOLS "Build Aeron asset tools" ${PROJECT_IS_TOP_LEVEL})
add_subdirectory(tools/imgbake)
add_subdirectory(tools/gltf_cook)
add_subdirectory(tools/fontbake)

add_library(aeron_asset STATIC
    src/asset/opt_model.c
)
target_include_directories(aeron_asset
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/opt2gltf
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gltf_cook
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
)
target_link_libraries(aeron_asset
    PUBLIC aeron_scene
    PRIVATE opt2gltf_lib aeron_gltf_cook_lib
)
set_target_properties(aeron_asset PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

if(AERON_BUILD_TOOLS)
    add_executable(opt2gltf
        tools/opt2gltf/main.c)
    target_include_directories(opt2gltf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/opt2gltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(opt2gltf PRIVATE opt2gltf_lib)
    set_target_properties(opt2gltf PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# Example apps — small windowed programs demonstrating engine subsystems.
option(AERON_BUILD_EXAMPLES "Build Aeron example apps" OFF)
if(AERON_BUILD_EXAMPLES)
    add_subdirectory(examples/ui_demo)
endif()

# Formatting helpers (standalone use; game repos have their own format targets).
set(AERON_FORMAT_FILES
    ${AERON_SOURCES}
    src/gpu_debug.h
    src/internal.h
    src/video.c
    src/video_decode.c
    src/video_internal.h
    include/aeron/aeron.h
    include/aeron/audio.h
    include/aeron/config_file.h
    include/aeron/dialog.h
    include/aeron/input.h
    include/aeron/log.h
    include/aeron/main.h
    include/aeron/numeric.h
    include/aeron/paths.h
    include/aeron/render.h
    include/aeron/surface.h
    include/aeron/time.h
    include/aeron/video.h
    include/aeron/vfs.h
    shaders/fullscreen.frag.hlsl
    shaders/compute_fill.comp.hlsl
    shaders/compute_fill_buffer.comp.hlsl
    shaders/fullscreen.vert.hlsl
)

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT AND PROJECT_IS_TOP_LEVEL)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} --Wno-error=unknown -i ${AERON_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting Aeron sources with clang-format"
    )
endif()
