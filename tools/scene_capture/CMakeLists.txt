add_executable(scene_capture main.c config.c fixture.c capture.c runner.c bloom.c bloom_chart.c bloom_metrics.c compare.c benchmark.c)
target_link_libraries(scene_capture PRIVATE aeron_asset)
target_include_directories(scene_capture PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(scene_capture PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(scene_capture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR})
endforeach()
file(RELATIVE_PATH CAPTURE_SHADER_DIR "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(scene_capture PRIVATE SCENE_CAPTURE_SHADER_DIR="${CAPTURE_SHADER_DIR}"
    CAPTURE_BUILD_TYPE="$<CONFIG>" CAPTURE_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
if(MSVC)
    target_compile_options(scene_capture PRIVATE /W4)
else()
    target_compile_options(scene_capture PRIVATE -Wall -Wextra -Wpedantic)
endif()
