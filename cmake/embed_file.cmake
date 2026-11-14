# Writes a C source file that holds the bytes of a file as an array.
#   cmake -DINPUT=<file> -DOUTPUT=<file.c> -DSYMBOL=<name> -P embed_file.cmake
# The source defines `const unsigned char <name>[]` and
# `const unsigned long <name>_size`.

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
# 16 bytes per line, each as 0xNN.
string(REGEX REPLACE "([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
    "/* Generated from ${name} by cmake/embed_file.cmake. Do not edit. */\n"
    "const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
    "const unsigned long ${SYMBOL}_size = ${size}ul;\n")
