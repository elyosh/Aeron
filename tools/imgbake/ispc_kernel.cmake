# The ISPC texture compressor kernel (third_party/ispc_texcomp/kernel.ispc):
# the ISPC options and the files it gives for a system and a processor.
#
# The imgbake build includes this file. Run as a script, it compiles the
# kernel into a folder, which AERON_ISPC_KERNEL_DIR can then give to a build
# so that it doesn't run ISPC. The kernel takes minutes to compile, and only
# changes with this file, kernel.ispc and the ISPC version, so the package
# builds keep the compiled kernel between runs:
#
#   cmake -DISPC=<ispc> -DSYSTEM=<Windows|Linux|Darwin> -DPROCESSOR=<x86_64|arm64>
#         -DEXTENSION=<.o|.obj> -DOUT=<folder> -P ispc_kernel.cmake
#
# EXTENSION is the C object extension of the build that uses the kernel
# (.obj with MinGW).

set(IMGBAKE_ISPC_KERNEL_SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ispc_texcomp/kernel.ispc)

# Sets `out_flags` to the ISPC options for `system` and `processor`, empty
# when ISPC has no target for the processor, and `out_files` to the names of
# the files that ISPC writes: the objects, then the header.
function(imgbake_ispc_kernel_settings system processor extension out_flags out_files)
    set(files kernel${extension})
    if(processor MATCHES "^(arm64|aarch64|ARM64)$")
        set(flags --arch=aarch64 --target=neon-i32x8)
    elseif(processor MATCHES "^(x86_64|AMD64|amd64)$")
        # Multi-target: ISPC adds an object per target and a dispatcher that
        # picks one at run time.
        set(flags --arch=x86-64 --target=sse4-i32x4,avx2-i32x8)
        list(APPEND files kernel_sse4${extension} kernel_avx2${extension})
    else()
        set(flags)
    endif()
    if(flags)
        if(system STREQUAL "Windows")
            # ISPC writes objects for the system it runs on unless told
            # otherwise. The Linux and macOS releases can't write Windows
            # objects, so a cross build runs the Windows release, under Wine
            # for example.
            list(APPEND flags --target-os=windows)
        else()
            list(APPEND flags --pic)
        endif()
        list(APPEND flags -O2 --woff)
    endif()
    list(APPEND files kernel_ispc.h)
    set(${out_flags} ${flags} PARENT_SCOPE)
    set(${out_files} ${files} PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    foreach(variable ISPC SYSTEM PROCESSOR EXTENSION OUT)
        if(NOT DEFINED ${variable})
            message(FATAL_ERROR "ispc_kernel.cmake: give -D${variable}=...")
        endif()
    endforeach()
    imgbake_ispc_kernel_settings(${SYSTEM} ${PROCESSOR} ${EXTENSION} flags files)
    if(NOT flags)
        message(FATAL_ERROR "ispc_kernel.cmake: ISPC has no target for processor '${PROCESSOR}'")
    endif()
    file(MAKE_DIRECTORY ${OUT})
    execute_process(
        COMMAND ${ISPC} ${flags} ${IMGBAKE_ISPC_KERNEL_SOURCE} -o ${OUT}/kernel${EXTENSION} -h ${OUT}/kernel_ispc.h
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "ispc_kernel.cmake: ISPC failed (${result})")
    endif()
endif()
