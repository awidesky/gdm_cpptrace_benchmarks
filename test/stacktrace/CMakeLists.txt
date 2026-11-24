# glutil stack-frame skipping benchmark (old hardcoded skip vs new skip-by-name).
#
# Compiles the REAL glutil sources and glad DEBUG gl.c of the gdm subproject (added by the
# root CMakeLists.txt), with only src/debug_stacktrace.cpp replaced by bench_stacktrace.cpp.
# One executable per optimization level, all with -g. Everything else (definitions such as
# GDM_DEBUG, include paths, glfw, glm, cpptrace) is taken from gdm's own targets.

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_multi_config)
    # Visual Studio (and other multi-config) generators: the configuration is chosen at build
    # time; build with --config Debug or --config RelWithDebInfo (gdm's debug code is compiled
    # only there, through $<CONFIG:...> generator expressions).
    message(STATUS "[stacktrace bench] multi-config generator: build with --config Debug or RelWithDebInfo")
elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    message(FATAL_ERROR "The stacktrace benchmark needs CMAKE_BUILD_TYPE Debug or RelWithDebInfo "
                        "(gdm compiles glutil's debug code only there), got '${CMAKE_BUILD_TYPE}'.")
endif()

if(MSVC)
    # The per-level optimization below must be able to override the configuration's: /RTC
    # (MSVC Debug default) refuses to combine with /O1 /O2 (D8016), so it is dropped for the
    # benchmark executables (this directory only).
    foreach(_v CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${_v} "${${_v}}")
    endforeach()
    # GCC-style level -> MSVC: there is no O3 / Os; O3 = /O2 with the most aggressive inlining,
    # Os = /O1 (MSVC's /O1 already means "minimize size").
    set(_msvc_O0 /Od /Ob0)
    set(_msvc_O1 /O1)
    set(_msvc_O2 /O2)
    set(_msvc_O3 /O2 /Ob3)
    set(_msvc_Os /O1)
endif()
foreach(_t glutil gdm_glad_debug gdm::defs gdm::opengl gdm::window gdm::math cpptrace::cpptrace)
    if(NOT TARGET ${_t})
        message(FATAL_ERROR "gdm target '${_t}' is missing (needs GDM_USE_GLUTIL=ON, GDM_GL_LOADER=glad, GDM_WINDOW_BACKEND=glfw)")
    endif()
endforeach()

set(_glutil "${GDM_SRC_DIR}/external/glutil")

# glad DEBUG loader that gdm generated (compiled again here at each optimization level)
get_target_property(_glad_sources gdm_glad_debug SOURCES)
get_target_property(_glad_includes gdm_glad_debug INTERFACE_INCLUDE_DIRECTORIES)
# glutil's private include paths (stb_image, tiny_obj_loader, dds-ktx, src/, its config.hpp)
get_target_property(_glutil_includes glutil INCLUDE_DIRECTORIES)
# glutil's public definitions (GLUTIL_DISABLE_LOG_ON_RELEASE=...)
get_target_property(_glutil_defs glutil INTERFACE_COMPILE_DEFINITIONS)
if(NOT _glutil_defs)
    set(_glutil_defs "")
endif()

# kInternalScanDepth* are read from gdm's real debug_stacktrace.cpp, so the "new" variant
# always measures exactly what that gdm ships. (Older gdm: one kInternalScanDepth for both.)
file(READ "${_glutil}/src/debug_stacktrace.cpp" _st_src)
string(REGEX MATCH "kInternalScanDepthGetCalled = ([0-9]+)" _m "${_st_src}")
set(GDM_K_GETCALLED "${CMAKE_MATCH_1}")
string(REGEX MATCH "kInternalScanDepthPrint = ([0-9]+)" _m "${_st_src}")
set(GDM_K_PRINT "${CMAKE_MATCH_1}")
if(NOT GDM_K_GETCALLED OR NOT GDM_K_PRINT)
    string(REGEX MATCH "kInternalScanDepth = ([0-9]+)" _m "${_st_src}")
    set(GDM_K_GETCALLED "${CMAKE_MATCH_1}")
    set(GDM_K_PRINT "${CMAKE_MATCH_1}")
endif()
if(NOT GDM_K_GETCALLED OR NOT GDM_K_PRINT)
    message(FATAL_ERROR "Could not read kInternalScanDepth* from ${_glutil}/src/debug_stacktrace.cpp "
                        "(that gdm predates the skip-by-name change?)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_glutil}/src/debug_stacktrace.cpp")
message(STATUS "[stacktrace bench] gdm:  ${GDM_SRC_DIR}")
message(STATUS "[stacktrace bench] glad: ${_glad_sources}")
message(STATUS "[stacktrace bench] K: getCalled=${GDM_K_GETCALLED} print=${GDM_K_PRINT}")

set(_glutil_sources
    ${_glutil}/src/shader.cpp
    ${_glutil}/src/inspector.cpp
    ${_glutil}/src/glutil.cpp
    ${_glutil}/src/path.cpp
    ${_glutil}/src/texture.cpp
    ${_glutil}/src/model.cpp
    ${_glutil}/src/debug.cpp
    ${_glutil}/src/debug_callback.cpp
    ${_glutil}/src/debug_info.cpp
    ${_glutil}/src/debug_snapshot.cpp
    # src/debug_stacktrace.cpp is replaced by:
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_stacktrace.cpp
)

set(STACKTRACE_BENCH_OPTS O0 O1 O2 O3 Os CACHE STRING "Optimization levels to build")

foreach(opt IN LISTS STACKTRACE_BENCH_OPTS)
    set(t stacktrace_bench_${opt})
    add_executable(${t}
        ${_glutil_sources}
        ${_glad_sources}
        gl_stub.cpp
        user_code.cpp
        bench_main.cpp
    )
    target_include_directories(${t} PRIVATE
        ${_glutil}/include
        ${_glutil_includes}
        ${_glad_includes}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # gdm's definitions (GDM_DEBUG=1, GDM_HAS_*) and dependencies, as glutil gets them.
    # Not glutil itself: its debug_stacktrace.cpp is the file being replaced.
    target_link_libraries(${t} PRIVATE gdm::defs gdm::opengl gdm::window gdm::math cpptrace::cpptrace)
    target_compile_definitions(${t} PRIVATE
        ${_glutil_defs}
        GDM_HAS_GLUTIL
        BENCH_OPT="${opt}"
        BENCH_TOOLCHAIN="${CMAKE_CXX_COMPILER_ID}-${CMAKE_SYSTEM_PROCESSOR}"
        GDM_K_GETCALLED=${GDM_K_GETCALLED}
        GDM_K_PRINT=${GDM_K_PRINT}
    )
    if(opt STREQUAL "O0")
        set(_ndebug -UNDEBUG)   # gdm Debug
    else()
        set(_ndebug -DNDEBUG)   # gdm RelWithDebInfo-like
    endif()
    # Appended after the CMAKE_<LANG>_FLAGS_<CONFIG> flags, so these win.
    if(MSVC)
        if(NOT DEFINED _msvc_${opt})
            message(FATAL_ERROR "no MSVC optimization flags for level '${opt}' (O0 O1 O2 O3 Os)")
        endif()
        # /Zi + /DEBUG: PDB (cpptrace reads file:line through dbghelp from it); /utf-8 as glutil;
        # /bigobj for the large TUs (debug_callback.cpp, gl.c).
        target_compile_options(${t} PRIVATE ${_msvc_${opt}} /Zi ${_ndebug} /w /utf-8 /bigobj)
        target_link_options(${t} PRIVATE /DEBUG)
    else()
        target_compile_options(${t} PRIVATE -${opt} -g ${_ndebug} -Wno-error -w)
    endif()
    if(MINGW)
        # Large TUs (debug_callback.cpp, gl.c) can exceed the default COFF section limit at -O0 -g.
        target_compile_options(${t} PRIVATE -Wa,-mbig-obj)
    endif()
    set_target_properties(${t} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)  # gdm: -std=gnu++17
    # Same place for every generator (a multi-config one would add a <Config>\ subfolder),
    # so the driver finds <build>/test/stacktrace/stacktrace_bench_<opt>[.exe]; PDB next to it.
    set_target_properties(${t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}>"
        PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}>")

    gdm_bench_copy_runtime_dlls(${t})
    add_test(NAME ${t}_verify COMMAND ${t} --verify)

    # The same checks on gdm's REAL debug_stacktrace.cpp (black box: labels, logger output),
    # so a change in gdm's own file is caught too (the benchmark above uses a copy of it).
    set(r stacktrace_verify_real_${opt})
    set(_real_sources ${_glutil_sources})
    list(REMOVE_ITEM _real_sources ${CMAKE_CURRENT_SOURCE_DIR}/bench_stacktrace.cpp)
    add_executable(${r} ${_real_sources} ${_glutil}/src/debug_stacktrace.cpp ${_glad_sources}
                   gl_stub.cpp user_code.cpp verify_real.cpp)
    get_target_property(_inc ${t} INCLUDE_DIRECTORIES)
    get_target_property(_defs ${t} COMPILE_DEFINITIONS)
    get_target_property(_opts ${t} COMPILE_OPTIONS)
    get_target_property(_lopts ${t} LINK_OPTIONS)
    target_include_directories(${r} PRIVATE ${_inc})
    target_compile_definitions(${r} PRIVATE ${_defs})
    target_compile_options(${r} PRIVATE ${_opts})
    if(_lopts)
        target_link_options(${r} PRIVATE ${_lopts})
    endif()
    target_link_libraries(${r} PRIVATE gdm::defs gdm::opengl gdm::window gdm::math cpptrace::cpptrace)
    set_target_properties(${r} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}>"
        PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}>")
    gdm_bench_copy_runtime_dlls(${r})
    add_test(NAME ${r} COMMAND ${r})
endforeach()
