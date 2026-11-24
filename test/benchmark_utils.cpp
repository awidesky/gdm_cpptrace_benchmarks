#include "benchmark_utils.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace benchmarks {

std::string readFileRaw(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string duplicateText(const std::string& src, size_t factor) {
    std::string out;
    out.reserve(src.size() * factor);
    for (size_t i = 0; i < factor; ++i) out += src;
    return out;
}

std::string format_us(double microseconds) {
    std::ostringstream os;
    os << std::fixed;
    if (microseconds >= 1'000'000.0) {
        os << std::setprecision(1) << (microseconds / 1'000'000.0) << "s";
    } else if (microseconds >= 1'000.0) {
        os << std::setprecision(1) << (microseconds / 1'000.0) << "ms";
    } else {
        os << std::setprecision(1) << microseconds << "us";
    }
    return os.str();
}

void printResult(const std::string& label, const BenchResult& r) {
    std::cout << "  " << label << " : total: " << r.totalUs << " us, average: " << r.averageNs << " ns/call\n";
    if (!r.sane) std::cout << "    sanity: [MISMATCH] " << r.mismatchDetail << "\n";
}

void printComparison(const BenchResult& noOp, const BenchResult& check,
                     const BenchResult& qword, const BenchResult& dword,
                     const BenchResult& byte) {
    if (noOp.averageNs > 0.0) {
        std::cout << "  no-op=1x, "
                  << "check=" << (check.averageNs / noOp.averageNs) << "x, "
                  << "qword=" << (qword.averageNs / noOp.averageNs) << "x, "
                  << "dword=" << (dword.averageNs / noOp.averageNs) << "x, "
                  << "byte=" << (byte.averageNs / noOp.averageNs) << "x "
                  << "(" << (noOp.totalUs / 1000.0) << "ms, "
                  << (check.totalUs / 1000.0) << "ms, "
                  << (qword.totalUs / 1000.0) << "ms, "
                  << (dword.totalUs / 1000.0) << "ms, "
                  << (byte.totalUs / 1000.0) << "ms)\n";
    }
}

} // namespace benchmarks