cmake_minimum_required(VERSION 3.24)  # gdm requires 3.24
project(gdm_benchmarks
    VERSION 1.0.0
    LANGUAGES C CXX
)

enable_testing()

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type: GDM_DEBUG (glutil's debug code, the subject of the stacktrace benchmark)
# is only compiled in Debug and RelWithDebInfo.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Warnings for this project's own targets only. Not add_compile_options(): that would
# also put them on gdm and its dependencies (cpptrace, libdwarf, glfw, ...).
# Warnings are fatal only on request, so another compiler's new warnings cannot break a build.
option(GDM_BENCH_WERROR "Treat warnings in this project's own targets as errors" OFF)
if(MSVC)
    set(GDM_BENCH_WARNINGS /W4 $<$<BOOL:${GDM_BENCH_WERROR}>:/WX>)
else()
    set(GDM_BENCH_WARNINGS -Wall -Wextra -Wpedantic $<$<BOOL:${GDM_BENCH_WERROR}>:-Werror>)
endif()

# Add stdlib flag for macOS to ensure libc++ is used
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")
endif()

### GDM ###
# Same as ogl-gdm: FetchGDM() (cmake/FetchGDM.cmake) keeps a copy of the gdm sources in
# external/gdm, tracking the head commit of GDM_TAG, and gdm is added as a subproject, so
# gdm's own configure generates glad and fetches glfw / cpptrace / stb / ... into
# GDM_EXTERNAL_DIR (default: <this project>/external; pass the same path as for other gdm
# builds to share the downloads).
# -DGDM_SOURCE_DIR=<checkout> uses a local gdm checkout instead (e.g. to benchmark
# uncommitted changes); nothing is fetched then.
set(GDM_WINDOW_BACKEND "glfw" CACHE STRING "")
set(GDM_GL_LOADER "glad" CACHE STRING "")   # the benchmark builds glad's DEBUG gl.c (gdm_glad_debug)
set(GDM_USE_GLM ON CACHE BOOL "")
set(GDM_USE_GLUTIL ON CACHE BOOL "")
set(GDM_GLFW_STATIC ON CACHE BOOL "Build GLFW static (no glfw3.dll/dylib at runtime)")
set(GDM_BUILD_EXAMPLES OFF CACHE BOOL "")
set(GDM_BUILD_TESTS OFF CACHE BOOL "")

set(GDM_SOURCE_DIR "" CACHE PATH "Local gdm checkout to use instead of fetching external/gdm")
if(GDM_SOURCE_DIR)
    get_filename_component(GDM_SRC_DIR "${GDM_SOURCE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${GDM_SRC_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "GDM_SOURCE_DIR (${GDM_SRC_DIR}) is not a gdm checkout")
    endif()
    message(STATUS "GDM: using local checkout ${GDM_SRC_DIR}")
else()
    set(GDM_SRC_DIR "${CMAKE_SOURCE_DIR}/external/gdm")
    set(GDM_TAG "master" CACHE STRING "GDM's version; branch, tag, or commit hash")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/FetchGDM.cmake")
    FetchGDM("${GDM_SRC_DIR}" "${GDM_TAG}")
endif()
# EXCLUDE_FROM_ALL: only what the benchmarks link (glfw, cpptrace, ...) gets built.
add_subdirectory("${GDM_SRC_DIR}" "${CMAKE_BINARY_DIR}/gdm" EXCLUDE_FROM_ALL)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CopyRuntimeDlls.cmake")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Benchmarks option
option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(BUILD_BENCHMARKS)
    add_subdirectory(test)
endif()

# Install (optional)
install(DIRECTORY include/ DESTINATION include)
