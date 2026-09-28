add_executable(motion_blur_capture main.c config.c fixture.c capture.c)
target_link_libraries(motion_blur_capture PRIVATE aeron_scene)
target_include_directories(motion_blur_capture PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(motion_blur_capture PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(motion_blur_capture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR})
endforeach()
file(RELATIVE_PATH MB_CAPTURE_SHADER_DIR "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(motion_blur_capture PRIVATE MB_CAPTURE_SHADER_DIR="${MB_CAPTURE_SHADER_DIR}")
if(MSVC)
    target_compile_options(motion_blur_capture PRIVATE /W4)
else()
    target_compile_options(motion_blur_capture PRIVATE -Wall -Wextra -Wpedantic)
endif()
