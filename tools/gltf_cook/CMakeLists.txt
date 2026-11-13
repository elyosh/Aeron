# aeron_gltf_cook — cook artist-authored glTF into Aeron's runtime GLB dialect.

add_library(aeron_gltf_cook_lib STATIC
    gltf_cook.c
    channel_atlas.c
    skin_layers.c)

target_include_directories(aeron_gltf_cook_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cgltf)

target_link_libraries(aeron_gltf_cook_lib PUBLIC imgbake aeron_cgltf PRIVATE aeron_cjson aeron_tool_log)
if(NOT MSVC)
    target_link_libraries(aeron_gltf_cook_lib PRIVATE m)
endif()
set_target_properties(aeron_gltf_cook_lib PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# The YAML reader of effect library and model animation files
# (animated_materials.md).
add_library(aeron_yaml_json STATIC yaml_json.c)
target_include_directories(aeron_yaml_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_yaml_json PUBLIC aeron_yaml aeron_cjson)

# The canonical written form of the effect library files (effect_files.h):
# the cooker and opt2gltf write them.
add_library(aeron_effect_files STATIC effect_files.c)
target_include_directories(aeron_effect_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_effect_files PUBLIC aeron_cjson PRIVATE aeron_tool_log)
if(NOT MSVC)
    target_link_libraries(aeron_effect_files PRIVATE m)
endif()
set_target_properties(aeron_effect_files PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# Effect libraries (animated_materials.md): reading a library file, building
# it and writing it (aeron_gltf_cook --library). Apart from
# aeron_gltf_cook_lib, which the game links too.
add_library(aeron_effect_library STATIC
    effect_library_read.c
    effect_library_build.c
    effect_library_write.c)
target_include_directories(aeron_effect_library
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xxhash)
target_link_libraries(aeron_effect_library PUBLIC aeron_gltf_cook_lib aeron_yaml_json aeron_effect_files
    aeron_cjson PRIVATE aeron_tool_log)
if(NOT MSVC)
    target_link_libraries(aeron_effect_library PRIVATE m)
endif()
set_target_properties(aeron_effect_library PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

if(AERON_BUILD_TOOLS)
    # The cook of one model with its flight data, and the material checks:
    # aeron_gltf_cook and the mod importer link it.
    add_library(aeron_gltf_cook_tool STATIC cook_model.c flight_data.c material_checks.c)
    target_include_directories(aeron_gltf_cook_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(aeron_gltf_cook_tool
        PUBLIC aeron_gltf_cook_lib
        PRIVATE aeron_effect_library aeron_flight_model aeron_collision aeron_cjson aeron_yaml_json aeron_tool_log
            aeron_lod_generate)
    set_target_properties(aeron_gltf_cook_tool PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)

    # Generated levels of detail, and aeron_lod_report, which measures them
    # on cooked models.
    add_library(aeron_lod_generate STATIC lod_generate.c)
    target_include_directories(aeron_lod_generate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(aeron_lod_generate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/bcdec)
    # aeron_scene's KTX2 reader and imgbake's sRGB table measure each
    # primitive's average appearance.
    target_link_libraries(aeron_lod_generate PUBLIC aeron_flight_model PRIVATE aeron_meshoptimizer aeron_scene imgbake)
    if(NOT MSVC)
        target_link_libraries(aeron_lod_generate PRIVATE m)
        set_source_files_properties(lod_generate.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    else()
        set_source_files_properties(lod_generate.c PROPERTIES COMPILE_OPTIONS "/fp:precise")
    endif()
    set_target_properties(aeron_lod_generate PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)
    add_executable(aeron_lod_report lod_report.c)
    target_link_libraries(aeron_lod_report PRIVATE aeron_lod_generate)
    set_target_properties(aeron_lod_report PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)

    add_executable(aeron_gltf_cook main.c project_build.c project_files.cpp)
    target_link_libraries(aeron_gltf_cook PRIVATE aeron_gltf_cook_tool aeron_effect_library)
    # Cooked simulation data must be bit-identical across platforms.
    if(MSVC)
        set_source_files_properties(flight_data.c PROPERTIES COMPILE_OPTIONS "/fp:precise")
    else()
        set_source_files_properties(flight_data.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    # The cooker calls no SDL function, but links aeron (through imgbake),
    # which links SDL: the unused library is dropped, so that the cooker runs
    # where SDL is not installed. (A Windows executable imports only the DLLs
    # it uses.)
    if(APPLE)
        target_link_options(aeron_gltf_cook PRIVATE LINKER:-dead_strip_dylibs)
    elseif(UNIX)
        target_link_options(aeron_gltf_cook PRIVATE LINKER:--as-needed)
    endif()
    # MinGW's C++ runtime is DLLs no Windows has. (Linux keeps the system's
    # libstdc++: a static one needs a newer glibc.)
    if(AERON_GLTF_COOK_STANDALONE AND WIN32 AND NOT MSVC)
        target_link_options(aeron_gltf_cook PRIVATE -static)
    endif()
    set_target_properties(aeron_gltf_cook PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
