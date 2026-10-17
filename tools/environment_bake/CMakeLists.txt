# environment_bake — offline HDR environment package baker (radiance,
# diffuse irradiance and GGX specular cubes, sun sprites, package entry;
# probe cube arrays and irradiance grids with their layout).

add_executable(environment_bake main.c)
target_link_libraries(environment_bake PRIVATE imgbake_bc6h aeron_scene aeron_flight_model aeron_cjson)
set_target_properties(environment_bake PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
    target_compile_options(environment_bake PRIVATE /W4)
else()
    target_compile_options(environment_bake PRIVATE -Wall -Wextra -Wpedantic)
endif()
