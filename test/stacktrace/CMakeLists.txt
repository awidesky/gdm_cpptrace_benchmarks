# glutil stack-frame skipping benchmark (old hardcoded skip vs new skip-by-name).
#
# Compiles the REAL glutil sources and glad DEBUG gl.c of the gdm subproject (added by the
# root CMakeLists.txt), with only src/debug_stacktrace.cpp replaced by bench_stacktrace.cpp.
# One executable per optimization level, all with -g. Everything else (definitions such as
# GDM_DEBUG, include paths, glfw, glm, cpptrace) is taken from gdm's own targets.

if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    message(FATAL_ERROR "The stacktrace benchmark needs CMAKE_BUILD_TYPE Debug or RelWithDebInfo "
                        "(gdm compiles glutil's debug code only there), got '${CMAKE_BUILD_TYPE}'.")
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
    target_compile_options(${t} PRIVATE -${opt} -g ${_ndebug} -Wno-error -w)
    if(MINGW)
        # Large TUs (debug_callback.cpp, gl.c) can exceed the default COFF section limit at -O0 -g.
        target_compile_options(${t} PRIVATE -Wa,-mbig-obj)
    endif()
    set_target_properties(${t} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)  # gdm: -std=gnu++17

    gdm_bench_copy_runtime_dlls(${t})
    add_test(NAME ${t}_verify COMMAND ${t} --verify)
endforeach()
