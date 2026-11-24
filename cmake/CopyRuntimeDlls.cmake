# gdm_bench_copy_runtime_dlls(<target>)
#
# Windows only: after <target> is built, copy the DLLs it needs at run time next to it, so
# it starts without relying on PATH:
#   - DLLs of shared CMake targets it links ($<TARGET_RUNTIME_DLLS>, e.g. a shared cpptrace)
#   - MinGW's runtime DLLs from the compiler's bin dir (libstdc++, libgcc, winpthread)
# A missing DLL otherwise kills the process before main() with exit code 0xC0000135 and,
# in a console, usually no message at all.
function(gdm_bench_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    set(_mingw_dlls "")
    if(MINGW)
        get_filename_component(_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_dll libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libwinpthread-1.dll)
            if(EXISTS "${_bin}/${_dll}")
                list(APPEND _mingw_dlls "${_bin}/${_dll}")
            endif()
        endforeach()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>;${_mingw_dlls}"
            "-DDST=$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDllsScript.cmake"
        VERBATIM
    )
endfunction()
