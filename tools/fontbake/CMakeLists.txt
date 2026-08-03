# fontbake — TTF → <basename>.png + <basename>.fnt (TFNT v2) atlas pairs
# for AeronFontAtlas_Load. Thin CLI over imgbake's font_atlas rasterizer.

if(AERON_BUILD_TOOLS)
    add_executable(fontbake fontbake.c)
    target_link_libraries(fontbake PRIVATE imgbake)
    set_target_properties(fontbake PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(MSVC)
        target_compile_options(fontbake PRIVATE /W4)
    else()
        target_compile_options(fontbake PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
