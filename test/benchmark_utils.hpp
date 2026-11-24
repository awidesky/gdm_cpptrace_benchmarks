#ifndef BENCHMARK_UTILS_HPP
#define BENCHMARK_UTILS_HPP

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/time.h>

namespace benchmarks {

// Result structure for benchmarks
struct BenchResult {
    double totalUs = 0.0;
    double averageNs = 0.0;
    bool sane = true;
    std::string mismatchDetail;
};

// Read entire file as binary
std::string readFileRaw(const std::filesystem::path& path);  // path: wide on Windows, so non-ASCII paths work

// Duplicate text N times
std::string duplicateText(const std::string& src, size_t factor);

// Format microseconds for human-readable output
std::string format_us(double microseconds);

// Benchmark a mutating function (modifies buffer in-place)
template<typename F>
BenchResult benchmarkMutating(const std::string& source, size_t iterations, F&& fn) {
    std::string work;
    work.reserve(source.size());

    timeval t0;
    gettimeofday(&t0, nullptr);
    for (size_t i = 0; i < iterations; ++i) {
        work = source;
        fn(work.data(), work.size());
    }
    timeval t_end;
    gettimeofday(&t_end, nullptr);
    double elapsedUs = (t_end.tv_sec - t0.tv_sec) * 1000000.0 + (t_end.tv_usec - t0.tv_usec);

    BenchResult r;
    r.totalUs = elapsedUs;
    r.averageNs = (elapsedUs * 1000.0) / static_cast<double>(iterations);
    return r;
}

// Benchmark a checking function (read-only)
template<typename F>
BenchResult benchmarkChecking(const std::string& source, size_t iterations, F&& fn) {
    std::string work;
    work.reserve(source.size());

    timeval t0;
    gettimeofday(&t0, nullptr);
    volatile size_t trueCount = 0;
    for (size_t i = 0; i < iterations; ++i) {
        work = source;
        const bool has = fn(work.data(), work.size());
        if (has) trueCount = trueCount + 1;
    }
    timeval t_end;
    gettimeofday(&t_end, nullptr);
    double elapsedUs = (t_end.tv_sec - t0.tv_sec) * 1000000.0 + (t_end.tv_usec - t0.tv_usec);

    BenchResult r;
    r.totalUs = elapsedUs;
    r.averageNs = (elapsedUs * 1000.0) / static_cast<double>(iterations);
    return r;
}

// Print formatted result
void printResult(const std::string& label, const BenchResult& r);

// Print comparison relative to no-op baseline
void printComparison(const BenchResult& noOp, const BenchResult& check,
                     const BenchResult& qword, const BenchResult& dword,
                     const BenchResult& byte);

} // namespace benchmarks

#endif // BENCHMARK_UTILS_HPP