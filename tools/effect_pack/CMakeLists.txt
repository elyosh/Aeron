# effect_pack — effect image libraries (README.md): the library file reader, the library builder and the
# writer, shared by the tools that build libraries (aeron_gltf_cook --library) or show their images.

add_library(aeron_effect_pack_lib STATIC effect_pack.c library_file.c library_build.c)
target_include_directories(aeron_effect_pack_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xxhash)
target_link_libraries(aeron_effect_pack_lib PUBLIC aeron_gltf_cook_lib aeron_yaml_json aeron_cjson)
if(NOT MSVC)
    target_link_libraries(aeron_effect_pack_lib PRIVATE m)
endif()
set_target_properties(aeron_effect_pack_lib PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# The YAML reader of effect library and model animation files (aeron_gltf_cook --library and
# --animations).
add_library(aeron_yaml_json STATIC yaml_json.c)
target_include_directories(aeron_yaml_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_yaml_json PUBLIC aeron_yaml aeron_cjson)
