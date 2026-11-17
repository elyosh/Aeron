# planes_demo — a 2D draw list drawn on planes in 3D (see main.c for usage).

add_executable(planes_demo main.c)
target_link_libraries(planes_demo PRIVATE aeron_scene)
set_target_properties(planes_demo PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # Aeron resolves shader_path relative to the executable; placing the
    # binary at the build root keeps that path identical to game shells.
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

file(RELATIVE_PATH PLANES_DEMO_SHADER_RELATIVE_DIR
     "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(planes_demo PRIVATE
    PLANES_DEMO_SHADER_RELATIVE_DIR="${PLANES_DEMO_SHADER_RELATIVE_DIR}")

if(MSVC)
    target_compile_options(planes_demo PRIVATE /W4)
else()
    target_compile_options(planes_demo PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(planes_demo PRIVATE m)
endif()
