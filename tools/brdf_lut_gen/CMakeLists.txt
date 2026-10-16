# brdf_lut_gen — writes src/scene/brdf_lut.inc, the split-sum BRDF table the
# PBR shaders sample. The output is checked in; run the tool by hand after
# changing the table.
#
# Fused multiply-add changes a few table values, so the tool is built without
# it. This way every machine and compiler produces the same file.

if(AERON_BUILD_TOOLS)
    add_executable(brdf_lut_gen brdf_lut_gen.c)
    set_target_properties(brdf_lut_gen PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(MSVC)
        target_compile_options(brdf_lut_gen PRIVATE /W4 /fp:precise)
    else()
        target_compile_options(brdf_lut_gen PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
        target_link_libraries(brdf_lut_gen PRIVATE m)
    endif()
endif()
