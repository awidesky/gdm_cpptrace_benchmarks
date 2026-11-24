# Benchmark targets

# Common benchmark utilities
add_library(benchmark_utils STATIC
    benchmark_utils.cpp
)

target_compile_options(benchmark_utils PRIVATE ${GDM_BENCH_WARNINGS})

target_include_directories(benchmark_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Encoding benchmark (copied from glutil)
add_executable(encoding_benchmark
    encoding_benchmark.cpp
)

target_link_libraries(encoding_benchmark PRIVATE benchmark_utils)
target_compile_options(encoding_benchmark PRIVATE ${GDM_BENCH_WARNINGS})

# Configure config.hpp
set(TEST_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets")
configure_file(
    ${CMAKE_SOURCE_DIR}/include/config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.hpp
    @ONLY
)

target_include_directories(encoding_benchmark PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Copy test assets
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

gdm_bench_copy_runtime_dlls(encoding_benchmark)
add_test(NAME encoding_benchmark COMMAND encoding_benchmark)

# glutil stack-frame skipping benchmark on the real glutil/glad sources (see BENCHMARK.md)
add_subdirectory(stacktrace)
