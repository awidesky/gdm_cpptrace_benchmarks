// Benchmark replacement for glutil's src/debug_stacktrace.cpp.
//
// Same TU role, same public symbols (glutil::debug::printStackTrace, ...), so the real
// debug_callback.cpp / glad gl.c call into it through exactly the same stack as in gdm.
// Each API holds both implementations, selected at run time:
//   Old : body of 358d42c^ (cpptrace::stacktrace::current(skip, depth)), with the
//         hardcoded skip replaced by the per-case correct value set by the harness.
//   New : body of gdm HEAD (+ B1 fix: both glad prefixes), with kInternalScanDepth taken
//         from the harness so different values can be measured.
// Timing points are inside the API: entry, frames ready, return.
#if GDM_DEBUG

#include <glutil/gl.hpp>
#include <glutil/debug_stacktrace.hpp>

#include <cpptrace/cpptrace.hpp>
#include <cpptrace/formatting.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "bench_control.hpp"
#include "portable.hpp"

namespace bench {
Control& ctl() { static Control c; return c; }
bool classifierSelfTest();
}

namespace glutil::debug {
namespace {

using clk = std::chrono::steady_clock;
inline double ns(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

// ---- verbatim from gdm debug_stacktrace.cpp (color helpers) -----------------------
constexpr const char* kForce   = "FORCE_COLOR";
constexpr const char* kNoColor = "NO_COLOR";
const char* safe_getenv(const char* name) {
#ifdef _WIN32
    size_t len = 0;
    char* buf = nullptr;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        static thread_local std::string cached;
        cached = buf;
        free(buf);
        return cached.c_str();
    }
    return nullptr;
#else
    return std::getenv(name);
#endif
}
bool envSet(const char* name) {
    const char* v = safe_getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
FILE* fileOf(std::ostream& out) {
    if(&out == &std::cout) return stdout;
    if(&out == &std::cerr || &out == &std::clog) return stderr;
    return nullptr;
}
bool isTerminal(FILE* f) {
#ifdef _WIN32
    return _isatty(_fileno(f)) != 0;
#else
    return isatty(fileno(f)) != 0;
#endif
}
bool resolveColor(ColorMode mode, std::ostream& out) {
    if(mode == ColorMode::Always) return true;
    if(mode == ColorMode::Never)  return false;
    if(envSet(kNoColor)) return false;
    if(envSet(kForce)) return true;
    if(envSet("XCODE_VERSION_ACTUAL") && !envSet("TERM_PROGRAM")) return false;
    if(const char* t = safe_getenv("TERM"); t && !std::strcmp(t, "dumb")) return false;
    const FILE* f = fileOf(out);
    return f && isTerminal(const_cast<FILE*>(f));
}

// True when `path` names glutil's src/<file> (separator-insensitive suffix match, so it
// works for "/.../external/glutil/src/x.cpp" as well as "C:\\...\\glutil\\src\\x.cpp").
// A user file that merely has the same name (e.g. their own debug_callback.cpp) does not match.
bool isGlutilSource(std::string_view path, std::string_view file) {
    const std::string_view parts[] = {file, "src", "glutil"};
    std::size_t end = path.size();
    for (std::string_view part : parts) {
        if (end < part.size()) return false;
        const std::size_t begin = end - part.size();
        if (path.substr(begin, part.size()) != part) return false;
        if (part == "glutil") return begin == 0 || path[begin - 1] == '/' || path[begin - 1] == '\\';
        if (begin == 0 || (path[begin - 1] != '/' && path[begin - 1] != '\\')) return false;
        end = begin - 1;
    }
    return false;
}

// ---- verbatim from gdm debug_stacktrace.cpp (new method), B1 fix applied ----------
bool isInternalFrame(const cpptrace::stacktrace_frame& frame) {
    static constexpr std::string_view kPrefixes[] = {
        "glutil::debug::callbacks::",
        "glutil::debug::printStackTrace",
        "glutil::debug::stringStackTrace",
        "glutil::debug::getCalledGLfunctionName",
        "glad_debug_impl_",
        "_glad_debug_impl_",
    };
    const std::string_view symbol = frame.symbol;
    for (std::string_view prefix : kPrefixes)
        if (symbol.substr(0, prefix.size()) == prefix) return true;
    // The GL-callback functions are static functions of debug_callback.cpp. Clang names
    // them "glutil::debug::callbacks::fullPostCallback(...)", but GCC emits no qualified
    // (linkage) name for internal-linkage functions, so with MinGW cpptrace reports a bare
    // "fullPostCallback". Match those frames by their source file, which every toolchain
    // reports the same way (needs line info, as getCalledGLfunctionName does anyway).
    if (isGlutilSource(frame.filename, "debug_callback.cpp")) return true;
    return false;
}

bool keepCallerFrames(cpptrace::stacktrace& trace, int extraSkip, int depth) {
    auto& frames = trace.frames;
    if (frames.empty() || !isInternalFrame(frames.front())) return false;

    auto firstUser = std::find_if_not(frames.begin(), frames.end(), isInternalFrame);
    const auto remaining = static_cast<std::size_t>(frames.end() - firstUser);
    firstUser += static_cast<std::ptrdiff_t>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(std::max(extraSkip, 0))));
    frames.erase(frames.begin(), firstUser);

    const auto keep = static_cast<std::size_t>(std::max(depth, 0));
    if (frames.size() > keep) frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(keep), frames.end());
    return true;
}

// gdm: kInternalScanDepth + skip + depth. Here kInternalScanDepth is a parameter.
std::size_t captureDepth(std::size_t kInternalScanDepth, int skip, int depth) {
    return kInternalScanDepth + static_cast<std::size_t>(std::max(skip, 0))
                              + static_cast<std::size_t>(std::max(depth, 0));
}

// ---- benchmark-only helpers (never on a timed path) --------------------------------
std::size_t leadingInternal(const cpptrace::stacktrace& t) {
    std::size_t i = 0;
    while (i < t.frames.size() && isInternalFrame(t.frames[i])) ++i;
    return i;
}

std::string frameText(const cpptrace::stacktrace_frame& f) {
    std::string s = f.symbol;
    if (f.line.has_value()) s += " @ " + cpptrace::basename(f.filename) + ":" + std::to_string(f.line.value());
    return s;
}

// Fills the stack-shape part of a VerifyRec from a full skip=0 trace taken in the API.
void analyseFull(const cpptrace::stacktrace& full, bench::VerifyRec& v) {
    const std::size_t j = leadingInternal(full);
    v.internalResolved = static_cast<int>(j);
    int phys = 0;
    for (std::size_t i = 0; i < j; ++i) if (!full.frames[i].is_inline) ++phys;
    v.internalPhysical = phys;
    // cpptrace lists the inlined callees of a physical frame first, then the physical
    // frame itself (is_inline == false); a new physical frame starts after it.
    v.aligned = (j == 0) || !full.frames[j - 1].is_inline;
    v.derivedOldSkip = v.aligned ? phys : -1;
    if (j < full.frames.size()) v.firstKept = frameText(full.frames[j]);
    std::ostringstream ss;
    int p = 0;
    for (std::size_t i = 0; i < full.frames.size() && i < 14; ++i) {
        const auto& f = full.frames[i];
        ss << "      [" << i << "] " << (f.is_inline ? "inline " : "phys p") ;
        if (!f.is_inline) ss << p++;
        ss << "  " << frameText(f).substr(0, 110) << (i < j ? "   <- internal" : "") << '\n';
    }
    v.stackDump = ss.str();
}

std::string formatTrace(const cpptrace::stacktrace& t, int snippets, int ctx) {
    std::ostringstream ss;
    cpptrace::formatter{}
      .colors(cpptrace::formatter::color_mode::none)
      .addresses(cpptrace::formatter::address_mode::raw)
      .paths(cpptrace::formatter::path_mode::basename)
      .snippets(snippets).snippet_context(ctx).columns(true)
      .print(ss, t);
    return ss.str();
}

// getCalledGLfunctionName's frame -> string step (verbatim body, factored for verify).
std::string callSiteText(const cpptrace::stacktrace& trace) {
    if (trace.frames.empty()) return "unknown source location";
    const cpptrace::stacktrace_frame& frame = trace.frames.front();
    if (!frame.line.has_value() || frame.filename.empty()) return "unknown source location";
    std::string code;
    std::string snippet = cpptrace::get_snippet(frame.filename, frame.line.value(), 0, false);
    std::size_t marker = snippet.find('>');
    if (marker != std::string::npos) {
        std::size_t colon = snippet.find(':', marker);
        if (colon != std::string::npos) {
            code = snippet.substr(colon + 1);
            const char* ws = " \t\r\n";
            std::size_t begin = code.find_first_not_of(ws);
            std::size_t end = code.find_last_not_of(ws);
            if (begin == std::string::npos) code.clear();
            else code = code.substr(begin, end - begin + 1);
        }
    }
    std::string location = cpptrace::basename(frame.filename) + ":" + std::to_string(frame.line.value());
    return code.empty() ? location : location + " -> " + code;
}

// ---- proposal: getCalledGLfunctionName result cache keyed by raw return addresses ----
// The raw (unresolved) top `cap` frames identify the call path: same addresses -> same
// internal frames and same user call site -> same result. Unwinding without resolving costs
// well under a microsecond, so a hit skips all symbol/DWARF work.
struct RawKeyHash {
    std::size_t operator()(const std::vector<cpptrace::frame_ptr>& v) const noexcept {
        std::size_t h = static_cast<std::size_t>(1469598103934665603ull);
        for (cpptrace::frame_ptr p : v) { h ^= static_cast<std::size_t>(p); h *= static_cast<std::size_t>(1099511628211ull); }
        return h;
    }
};
std::mutex g_callSiteCacheMutex;
std::unordered_map<std::vector<cpptrace::frame_ptr>, std::string, RawKeyHash> g_callSiteCache;

} // namespace

bool colorForced() { return envSet(kForce); }
bool colorEnabled(std::ostream& out) { return resolveColor(ColorMode::Auto, out); }

std::string frameKey(const cpptrace::stacktrace_frame& f) {
    std::string t = (f.is_inline ? "(inline) " : "") + f.symbol + " @ " + cpptrace::basename(f.filename);
    if (f.line.has_value()) t += ":" + std::to_string(f.line.value());
    return t;
}

// old vs new frame by frame (symbol, file, line, inline flag; addresses are not compared).
void compareFrames(const cpptrace::stacktrace& o, const cpptrace::stacktrace& n, bench::VerifyRec& v) {
    v.oldFrames = static_cast<int>(o.frames.size());
    v.newFrames = static_cast<int>(n.frames.size());
    std::size_t i = 0;
    while (i < o.frames.size() && i < n.frames.size() && frameKey(o.frames[i]) == frameKey(n.frames[i])) ++i;
    v.newPrefixOfOld = i == n.frames.size();
    if (i < o.frames.size() || i < n.frames.size()) {
        int inl = 0;
        for (const auto& f : o.frames) inl += f.is_inline ? 1 : 0;
        v.frameDiff = "first difference at #" + std::to_string(i) + ": old "
                    + (i < o.frames.size() ? frameKey(o.frames[i]) : std::string("<end>")) + " | new "
                    + (i < n.frames.size() ? frameKey(n.frames[i]) : std::string("<end>"))
                    + " (old " + std::to_string(v.oldFrames) + " frames incl. " + std::to_string(inl)
                    + " inline, new " + std::to_string(v.newFrames) + ")";
    }
}

// --frames: cost of capturing n physical frames from inside the API (the same stack depth and
// unwinder the old / new / cache paths use; nothing is resolved). Inlined into the API body so
// the captured frames start at the API's own frame, like the real calls.
BENCH_FORCEINLINE void measureCapture(bench::VerifyRec& v) {
    static constexpr int kDepths[] = {1, 2, 4, 5, 7, 16, 21};
    for (int n : kDepths) {
        std::vector<double> ts;
        for (int k = 0; k < 101; ++k) {
            const auto a = clk::now();
            cpptrace::raw_trace r = cpptrace::generate_raw_trace(0, static_cast<std::size_t>(n));
            const auto b = clk::now();
            ts.push_back(std::chrono::duration<double, std::nano>(b - a).count());
        }
        std::nth_element(ts.begin(), ts.begin() + 50, ts.end());
        v.captureNs.emplace_back(n, ts[50]);
    }
}

// NOTE: every cpptrace::stacktrace::current() call below sits directly in the API body,
// so frame 0 is always the API function itself, exactly like in gdm.

void printStackTrace(std::ostream& out, std::string header, int skip, int depth, bool snippets,
                     int snippet_context, ColorMode color) {
    auto& c = bench::ctl();

    if (c.verify) {
        bench::VerifyRec v{bench::Api::Print, {}, {}};
        cpptrace::stacktrace full = cpptrace::stacktrace::current(0, 64);
        v.raw = cpptrace::generate_raw_trace(0, 24);
        if (c.captureCosts) measureCapture(v);
        analyseFull(full, v);
        cpptrace::stacktrace o = cpptrace::stacktrace::current(static_cast<std::size_t>(c.oldSkipPrint), depth);
        cpptrace::stacktrace n = cpptrace::stacktrace::current(0, captureDepth(c.kPrint, skip, depth));
        if (!keepCallerFrames(n, skip, depth)) v.newOut = "[keepCallerFrames failed]\n";
        v.oldOut = formatTrace(o, snippets, snippet_context);
        v.newOut += formatTrace(n, snippets, snippet_context);
        v.depth = depth;
        compareFrames(o, n, v);
        c.verifies.push_back(std::move(v));
        return;
    }

    const auto t0 = clk::now();
    if (c.method == bench::Method::Old) {
        // ---- 358d42c^ ----
        const bool useColor = resolveColor(color, out);
        cpptrace::stacktrace trace = cpptrace::stacktrace::current(static_cast<std::size_t>(c.oldSkipPrint), depth);
        const auto t1 = clk::now();
        const int resolved = static_cast<int>(trace.frames.size());
        cpptrace::formatter{}
          .header(header)
          .colors(useColor ? cpptrace::formatter::color_mode::always
                           : cpptrace::formatter::color_mode::none)
          .addresses(cpptrace::formatter::address_mode::raw)
          .paths(cpptrace::formatter::path_mode::basename)
          .snippets(snippets)
          .snippet_context(snippet_context)
          .columns(true)
          .print(out, trace);
        const auto t2 = clk::now();
        if (c.record)
            c.samples.push_back({bench::Api::Print, bench::Method::Old, ns(t0, t1), ns(t0, t2), depth,
                                 c.oldSkipPrint, resolved, 0, 0, resolved});
    } else {
        // ---- gdm HEAD ----
        const bool useColor = resolveColor(color, out);
        const std::size_t cap = captureDepth(c.kPrint, skip, depth);
        cpptrace::stacktrace trace = cpptrace::stacktrace::current(0, cap);
        const int resolved = static_cast<int>(trace.frames.size());
        if (!keepCallerFrames(trace, skip, depth)) {
            out << "[glutil] no symbols available: internal frames cannot be skipped, raw trace follows\n";
        }
        const auto t1 = clk::now();
        const int kept = static_cast<int>(trace.frames.size());
        cpptrace::formatter{}
          .header(header)
          .colors(useColor ? cpptrace::formatter::color_mode::always
                           : cpptrace::formatter::color_mode::none)
          .addresses(cpptrace::formatter::address_mode::raw)
          .paths(cpptrace::formatter::path_mode::basename)
          .snippets(snippets)
          .snippet_context(snippet_context)
          .columns(true)
          .print(out, trace);
        const auto t2 = clk::now();
        if (c.record)
            c.samples.push_back({bench::Api::Print, bench::Method::New, ns(t0, t1), ns(t0, t2), static_cast<int>(cap),
                                 0, resolved, -1, -1, kept});  // internal/surplus come from the verify pass
    }
}

std::string stringStackTrace(std::string header, int skip, int depth, bool snippets,
                             int snippet_context, ColorMode color) {
    // No caller in the gdm code base; kept only so the symbol exists.
    (void)header; (void)skip; (void)depth; (void)snippets; (void)snippet_context; (void)color;
    return {};
}

std::string getCalledGLfunctionName(int skip) {
    auto& c = bench::ctl();

    if (c.verify) {
        bench::VerifyRec v{bench::Api::GetCalled, {}, {}};
        cpptrace::stacktrace full = cpptrace::stacktrace::current(0, 64);
        v.raw = cpptrace::generate_raw_trace(0, 24);
        if (c.captureCosts) measureCapture(v);
        analyseFull(full, v);
        cpptrace::stacktrace o = cpptrace::stacktrace::current(static_cast<std::size_t>(c.oldSkipGetCalled), 1);
        cpptrace::stacktrace n = cpptrace::stacktrace::current(0, captureDepth(c.kGetCalled, skip, 1));
        v.oldOut = callSiteText(o);
        v.newOut = (!keepCallerFrames(n, skip, 1) || n.frames.empty()) ? "unknown source location" : callSiteText(n);
        {   // the cache's miss path (raw capture, then resolve) must give the same result
            cpptrace::raw_trace raw = cpptrace::generate_raw_trace(0, captureDepth(c.kGetCalled, skip, 1));
            cpptrace::stacktrace r = raw.resolve();
            const std::string viaRaw = (!keepCallerFrames(r, skip, 1) || r.frames.empty()) ? "unknown source location" : callSiteText(r);
            if (viaRaw != v.newOut) v.newOut += "  [raw-capture path differs: " + viaRaw + "]";
        }
        const std::string ret = v.newOut;
        c.verifies.push_back(std::move(v));
        return ret;
    }

    if (c.cacheMode == 2) {  // measure the miss path: start every call with an empty cache (not timed)
        std::lock_guard<std::mutex> lock(g_callSiteCacheMutex);
        g_callSiteCache.clear();
    }
    const auto t0 = clk::now();
    std::string result;
    if (c.method == bench::Method::Old) {
        // ---- 358d42c^ ----
        cpptrace::stacktrace trace = cpptrace::stacktrace::current(static_cast<std::size_t>(c.oldSkipGetCalled), 1);
        const auto t1 = clk::now();
        const int resolved = static_cast<int>(trace.frames.size());
        if (trace.frames.empty()) {
            result = "unknown source location";
        } else {
            result = callSiteText(trace);  // verbatim snippet/format code of both versions
        }
        const auto t2 = clk::now();
        if (c.record)
            c.samples.push_back({bench::Api::GetCalled, bench::Method::Old, ns(t0, t1), ns(t0, t2), 1,
                                 c.oldSkipGetCalled, resolved, 0, 0, resolved});
    } else {
        if (c.cacheMode != 0) {
            // ---- proposal: gdm HEAD + raw-address cache ----
            const std::size_t cap = captureDepth(c.kGetCalled, skip, 1);
            cpptrace::raw_trace raw = cpptrace::generate_raw_trace(0, cap);
            std::vector<cpptrace::frame_ptr> key;  // same key as gdm: skip + raw addresses
            key.reserve(raw.frames.size() + 1);
            key.push_back(static_cast<cpptrace::frame_ptr>(skip));
            key.insert(key.end(), raw.frames.begin(), raw.frames.end());
            {
                std::lock_guard<std::mutex> lock(g_callSiteCacheMutex);
                auto it = g_callSiteCache.find(key);
                if (it != g_callSiteCache.end()) {
                    result = it->second;
                    const auto t1 = clk::now();
                    if (c.record)
                        c.samples.push_back({bench::Api::GetCalled, bench::Method::New, ns(t0, t1), ns(t0, t1),
                                             static_cast<int>(cap), 0, 0, -1, -1, 1});
                    return result;
                }
            }
            cpptrace::stacktrace trace = raw.resolve();
            const int resolved = static_cast<int>(trace.frames.size());
            const bool ok = keepCallerFrames(trace, skip, 1) && !trace.frames.empty();
            const auto t1 = clk::now();
            result = ok ? callSiteText(trace) : "unknown source location";
            {
                std::lock_guard<std::mutex> lock(g_callSiteCacheMutex);
                g_callSiteCache.emplace(std::move(key), result);
            }
            const auto t2 = clk::now();
            if (c.record)
                c.samples.push_back({bench::Api::GetCalled, bench::Method::New, ns(t0, t1), ns(t0, t2),
                                     static_cast<int>(cap), 0, resolved, -1, -1, 1});
            return result;
        }
        // ---- gdm HEAD ----
        const std::size_t cap = captureDepth(c.kGetCalled, skip, 1);
        cpptrace::stacktrace trace = cpptrace::stacktrace::current(0, cap);
        const int resolved = static_cast<int>(trace.frames.size());
        const bool ok = keepCallerFrames(trace, skip, 1) && !trace.frames.empty();
        const auto t1 = clk::now();
        const int kept = static_cast<int>(trace.frames.size());
        result = ok ? callSiteText(trace) : "unknown source location";
        const auto t2 = clk::now();
        if (c.record)
            c.samples.push_back({bench::Api::GetCalled, bench::Method::New, ns(t0, t1), ns(t0, t2), static_cast<int>(cap),
                                 0, resolved, -1, -1, kept});  // internal/surplus come from the verify pass
    }
    return result;
}

} // namespace glutil::debug

// Toolchain-independent check of the internal-frame classifier (run by --verify): symbol and
// file forms seen from clang (macOS) and GCC/MinGW (Windows: unqualified static functions).
bool bench::classifierSelfTest() {
    struct Case { const char* symbol; const char* file; bool internal; };
    const Case cases[] = {
        {"glutil::debug::callbacks::fullPostCallback(void*, char const*, void (*)(), int, ...)", "/x/external/glutil/src/debug_callback.cpp", true},
        {"fullPostCallback", "C:\\u\\external\\gdm\\external\\glutil\\src\\debug_callback.cpp", true},   // MinGW
        {"autoLabelGLObjects", "C:/u/external/glutil/src/debug_callback.cpp", true},
        {"checkGLErrorPostCallback", "D:\\gdm\\external/glutil\\src/debug_callback.cpp", true},                // mixed separators
        {"glutil::debug::getCalledGLfunctionName[abi:cxx11](int)", "/x/bench_stacktrace.cpp", true},            // GCC abi tag
        {"glad_debug_impl_glBindTexture", "/x/gl.c", true},
        {"_glad_debug_impl_glBindTexture", "", true},                                                        // Mach-O symtab
        {"glutil::debug::labelGLobject(unsigned int, unsigned int, std::string const&)", "/x/external/glutil/src/debug.cpp", false},
        {"fullPostCallback", "C:\\myapp\\src\\debug_callback.cpp", false},     // user's own file of the same name
        {"fullPostCallback", "/x/notglutil/src/debug_callback.cpp", false},
        {"user::siteDrawArrays(void*)", "/x/user_code.cpp", false},
        {"autoLabelGLObjects", "", false},                                         // no line info: name alone is not enough
    };
    bool ok = true;
    for (const Case& c : cases) {
        cpptrace::stacktrace_frame f{};
        f.symbol = c.symbol;
        f.filename = c.file;
        if (glutil::debug::isInternalFrame(f) != c.internal) {
            std::printf("[classifier] FAIL: %s @ %s -> expected %s\n", c.symbol, c.file, c.internal ? "internal" : "user");
            ok = false;
        }
    }
    std::printf("[classifier] self-test %s (%zu cases)\n", ok ? "OK" : "FAIL", sizeof(cases) / sizeof(cases[0]));
    return ok;
}
#endif
