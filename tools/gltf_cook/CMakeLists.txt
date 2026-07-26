# aeron_gltf_cook — cook artist-authored glTF into Aeron's runtime GLB dialect.

add_library(aeron_gltf_cook_lib STATIC
    gltf_cook.c
    channel_atlas.c)

target_include_directories(aeron_gltf_cook_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cgltf)

target_link_libraries(aeron_gltf_cook_lib PUBLIC imgbake aeron_cgltf)
set_target_properties(aeron_gltf_cook_lib PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

if(AERON_BUILD_TOOLS)
    add_executable(aeron_gltf_cook main.c)
    target_link_libraries(aeron_gltf_cook PRIVATE aeron_gltf_cook_lib)
    set_target_properties(aeron_gltf_cook PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
