# environment_bake — offline HDR environment baker. It writes radiance,
# diffuse irradiance and GGX specular cubes, sun sprites, environment
# entries, and probe cube arrays and irradiance grids with their layout.

# The bake modes, which environment_bake and the mod importer run.
add_library(aeron_environment_bake_lib STATIC env_bake.c xwau_faces.c)
target_include_directories(aeron_environment_bake_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aeron_environment_bake_lib
    PUBLIC imgbake_bc6h
    PRIVATE aeron_scene aeron_flight_model aeron_cjson Threads::Threads)
set_target_properties(aeron_environment_bake_lib PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(aeron_environment_bake_lib PRIVATE /W4)
else()
    target_compile_options(aeron_environment_bake_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(environment_bake main.c)
target_link_libraries(environment_bake PRIVATE aeron_environment_bake_lib)
set_target_properties(environment_bake PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
    target_compile_options(environment_bake PRIVATE /W4)
else()
    target_compile_options(environment_bake PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Checks the XWAU face remap (xwau_faces.h), on generated faces or on the
# cube folders given as arguments.
add_executable(environment_bake_faces_test tests/xwau_faces_test.c xwau_faces.c)
target_include_directories(environment_bake_faces_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(environment_bake_faces_test PRIVATE imgbake Threads::Threads)
set_target_properties(environment_bake_faces_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
    target_compile_options(environment_bake_faces_test PRIVATE /W4)
else()
    target_compile_options(environment_bake_faces_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
