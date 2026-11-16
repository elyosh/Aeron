# xr_demo — cubes drawn through the Aeron XR layer (see main.c).

add_executable(xr_demo main.c)
target_link_libraries(xr_demo PRIVATE aeron_scene)
set_target_properties(xr_demo PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # Aeron looks for shader_path next to the executable, so the program is
    # placed at the build root, like the game.
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The demo's own shaders are compiled into Aeron's shader folder.
aeron_compile_hlsl(XR_DEMO_SHADER_OUTPUTS ${AERON_SHADER_OUTPUT_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/xr_demo_cube.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/xr_demo_cube.frag.hlsl)
add_custom_target(xr_demo_shaders DEPENDS ${XR_DEMO_SHADER_OUTPUTS})
add_dependencies(xr_demo xr_demo_shaders aeron_shaders)

file(RELATIVE_PATH XR_DEMO_SHADER_RELATIVE_DIR
     "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(xr_demo PRIVATE
    XR_DEMO_SHADER_RELATIVE_DIR="${XR_DEMO_SHADER_RELATIVE_DIR}")

if(MSVC)
    target_compile_options(xr_demo PRIVATE /W4)
else()
    target_compile_options(xr_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
