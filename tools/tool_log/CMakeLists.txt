# aeron_tool_log — the log of the tool libraries. The game links it too,
# through the libraries that convert and cook models.

add_library(aeron_tool_log STATIC tool_log.c)
target_include_directories(aeron_tool_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(aeron_tool_log PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
