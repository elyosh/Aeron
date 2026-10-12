# effect_pack — effect image libraries (README.md): the library file reader and the library writer, shared by
# the tools that build libraries or show their images.

add_library(aeron_effect_pack_lib STATIC effect_pack.c library_file.c)
target_include_directories(aeron_effect_pack_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_effect_pack_lib PUBLIC aeron_gltf_cook_lib aeron_yaml_json aeron_cjson)
set_target_properties(aeron_effect_pack_lib PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# The YAML reader of effect library and model animation files (effect_pack,
# gltf_cook --animations).
add_library(aeron_yaml_json STATIC yaml_json.c)
target_include_directories(aeron_yaml_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_yaml_json PUBLIC aeron_yaml aeron_cjson)

if(AERON_BUILD_TOOLS)
    add_executable(effect_pack main.c)
    target_include_directories(effect_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xxhash)
    target_link_libraries(effect_pack PRIVATE aeron_effect_pack_lib aeron_yaml_json)
    if(NOT MSVC)
        target_link_libraries(effect_pack PRIVATE m)
    endif()
    set_target_properties(effect_pack PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
