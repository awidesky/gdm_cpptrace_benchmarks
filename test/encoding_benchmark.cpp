/*
 * Encoding and replacement benchmark for shader text sanitization.
 *
 * Measures the cost of non-ASCII detection and replacement against a real
 * MS949-derived shader source, at x1, x4, and x8 sizes.
 */

#include "benchmark_utils.hpp"
#include "config.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// 1) No-op (baseline)
static void algoNoOp(char* /*data*/, size_t /*size*/) {}

// 2) Check-only: hasNonASCII
static bool algoCheckNonASCII(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (static_cast<unsigned char>(data[i]) & 0x80) return true;
    }
    return false;
}

// 3) qword replace (8 bytes at a time)
static void algoQwordReplace(char* data, size_t size) {
    constexpr uint64_t HIGH_BIT_MASK_64 = 0x8080808080808080ULL;

    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        if ((word & HIGH_BIT_MASK_64) != 0) {
            unsigned char* bytes = reinterpret_cast<unsigned char*>(data + i);
            for (size_t j = 0; j < 8; ++j) {
                if (bytes[j] & 0x80) bytes[j] = ' ';
            }
        }
    }
    for (; i < size; ++i) {
        if (static_cast<unsigned char>(data[i]) & 0x80) data[i] = ' ';
    }
}

// 4) dword replace (4 bytes at a time)
static void algoDwordReplace(char* data, size_t size) {
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
        uint32_t word;
        std::memcpy(&word, data + i, sizeof(word));
        if ((word & 0x80808080U) != 0) {
            unsigned char* bytes = reinterpret_cast<unsigned char*>(data + i);
            for (size_t j = 0; j < 4; ++j) {
                if (bytes[j] & 0x80) bytes[j] = ' ';
            }
        }
    }
    for (; i < size; ++i) {
        if (static_cast<unsigned char>(data[i]) & 0x80) data[i] = ' ';
    }
}

// 5) byte-wise replace (naive)
static void algoByteReplace(char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (static_cast<unsigned char>(data[i]) & 0x80) data[i] = ' ';
    }
}

int main() {
    const std::filesystem::path ms949Path = benchmarks::TEST_ASSET_DIR / "shader" / "MS949.fs";
    const std::string ms949 = benchmarks::readFileRaw(ms949Path);

    if (ms949.empty()) {
        std::cerr << "Failed to read benchmark source: " << ms949Path << "\n";
        return 1;
    }

    std::cout << "Encoding check/replace benchmark (MS949 source)\n";
    std::cout << "----------------------------------------------\n";
    std::cout << "source: " << ms949Path << "\n";
    std::cout << "base size: " << ms949.size() << " bytes\n\n";

    const std::vector<size_t> factors = {1, 4, 8};
    const size_t iterations = 100000;

    for (size_t factor : factors) {
        const std::string data = benchmarks::duplicateText(ms949, factor);

        std::cout << "Case x" << factor << "\n";
        std::cout << "  size: " << data.size() << " bytes, iterations: " << iterations << "\n";

        // 1) no-op (baseline)
        auto noOp = benchmarks::benchmarkMutating(data, iterations, algoNoOp);
        noOp.sane = true;

        // 2) check-only
        auto check = benchmarks::benchmarkChecking(data, iterations, algoCheckNonASCII);
        check.sane = algoCheckNonASCII(data.data(), data.size());

        // 3) qword replace
        auto qword = benchmarks::benchmarkMutating(data, iterations, algoQwordReplace);
        {
            std::string work = data;
            algoQwordReplace(work.data(), work.size());
            qword.sane = !algoCheckNonASCII(work.data(), work.size());
            if (!qword.sane) qword.mismatchDetail = "qword replace still has non-ASCII";
        }

        // 4) dword replace
        auto dword = benchmarks::benchmarkMutating(data, iterations, algoDwordReplace);
        {
            std::string work = data;
            algoDwordReplace(work.data(), work.size());
            dword.sane = !algoCheckNonASCII(work.data(), work.size());
            if (!dword.sane) dword.mismatchDetail = "dword replace still has non-ASCII";
        }

        // 5) byte replace
        auto byte = benchmarks::benchmarkMutating(data, iterations, algoByteReplace);
        {
            std::string work = data;
            algoByteReplace(work.data(), work.size());
            byte.sane = !algoCheckNonASCII(work.data(), work.size());
            if (!byte.sane) byte.mismatchDetail = "byte replace still has non-ASCII";
        }

        benchmarks::printResult("1) no-op        ", noOp);
        benchmarks::printResult("2) check-only   ", check);
        benchmarks::printResult("3) qword replace", qword);
        benchmarks::printResult("4) dword replace", dword);
        benchmarks::printResult("5) byte replace ", byte);

        benchmarks::printComparison(noOp, check, qword, dword, byte);
        std::cout << "\n";
    }

    return 0;
}