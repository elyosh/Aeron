# ui_demo — windowed AeronUi showcase (see main.c for usage).

add_executable(ui_demo main.c)
target_link_libraries(ui_demo PRIVATE aeron_scene)
set_target_properties(ui_demo PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # Aeron resolves shader_path relative to the executable; placing the
    # binary at the build root keeps that path identical to game shells.
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Compiled-shader root (AERON_SHADER_OUTPUT_DIR) expressed relative to
# the executable directory, as Aeron_Init consumes it.
file(RELATIVE_PATH UI_DEMO_SHADER_RELATIVE_DIR
     "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(ui_demo PRIVATE
    UI_DEMO_SHADER_RELATIVE_DIR="${UI_DEMO_SHADER_RELATIVE_DIR}")

if(MSVC)
    target_compile_options(ui_demo PRIVATE /W4)
else()
    target_compile_options(ui_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
