// glutil stack-frame skipping benchmark: old (hardcoded skip) vs new (skip by name).
//
//   stacktrace_bench_<opt> --verify            correctness gate only (exit 1 on failure)
//   stacktrace_bench_<opt> --warm              verify, then warm timing (TSV lines "W ...")
//   stacktrace_bench_<opt> --cold <case> <var> first call in a fresh process ("C ...")
//
// Real glutil sources + real glad DEBUG gl.c + a stub GL driver; only debug_stacktrace.cpp
// is replaced (bench_stacktrace.cpp). See analysis.md.
#include <glutil/gl.hpp>
#include <glutil/debug_callback.hpp>
#include <glutil/logging.hpp>

#include <cpptrace/cpptrace.hpp>
#include <cpptrace/utils.hpp>

#include "hashing.hpp"  // glutil/src: g_gladCallbackMap (error aggregation state)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include "bench_control.hpp"
#include "gl_stub.hpp"
#include "user_code.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef BENCH_OPT
#error "BENCH_OPT must be defined (O0, O1, O2, O3, Os)"
#endif

using bench::Api;
using bench::Method;
using Mode = glutil::debug::GladDebugCallbacksMode;

namespace {

// Discards output but keeps the stream in a good state, so formatting still runs.
struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
NullBuf g_nullBuf;
std::ostream g_null(&g_nullBuf);

// ---------------------------------------------------------------------------------------
// Old hardcoded skip per (optimization level, case). The value that makes old land on the
// same frame as new; checked by --verify against the value measured on the real stack.
struct SkipRow { const char* opt; const char* caseId; int skip; };
const SkipRow kOldSkip[] = {
    // default (Debug / -O0)
    {"*", "C1", 3}, {"*", "C2", 3}, {"*", "C3", 3}, {"*", "C4", 4},
    {"*", "C5", 4}, {"*", "C6", 3}, {"*", "C7a", 6}, {"*", "C7b", 6},
    // autoPostInspcector is inlined into fullPostCallback from -O1 up
    {"O1", "C5", 3}, {"O1", "C7a", 5},
    {"O2", "C5", 3}, {"O2", "C7a", 5},
    {"O3", "C5", 3}, {"O3", "C7a", 5},
    {"Os", "C5", 3}, {"Os", "C7a", 5},
};
// The table above was measured with this toolchain. With any other toolchain (e.g. MinGW
// GCC on Windows, which may inline differently) the correct skips are measured on the real
// stack by the verify pass instead (and printed as "S" lines so they can be added here).
#ifndef BENCH_TOOLCHAIN
#define BENCH_TOOLCHAIN "unknown"
#endif
constexpr const char* kOldSkipTableToolchain = "AppleClang-arm64";
bool tableApplies() {  // BENCH_FORCE_MEASURED=1 exercises the other-toolchain path
    static const bool forced = std::getenv("BENCH_FORCE_MEASURED") != nullptr;
    return !forced && !std::strcmp(BENCH_TOOLCHAIN, kOldSkipTableToolchain);
}

std::map<std::string, int> g_measuredSkip;  // case -> skip measured by the verify pass
bool g_measuring = false;                   // verify pass 1: skips not known yet

void loadSkipsFromEnv() {  // --cold runs get the measured skips from the driver
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = std::getenv("BENCH_OLD_SKIPS");  // "C1=3,C2=3,..."
    if (!e) return;
    std::string s = e;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos) g_measuredSkip[item.substr(0, eq)] = std::atoi(item.c_str() + eq + 1);
        pos = end + 1;
    }
}

int oldSkip(const char* caseId) {
    if (tableApplies()) {
        for (const SkipRow& r : kOldSkip)
            if (!std::strcmp(r.opt, BENCH_OPT) && !std::strcmp(r.caseId, caseId)) return r.skip;
        for (const SkipRow& r : kOldSkip)
            if (!std::strcmp(r.opt, "*") && !std::strcmp(r.caseId, caseId)) return r.skip;
    } else {
        loadSkipsFromEnv();
        if (auto it = g_measuredSkip.find(caseId); it != g_measuredSkip.end()) return it->second;
    }
    if (g_measuring) return 0;  // placeholder; pass 1 only measures the stack shape
    std::fprintf(stderr, "no old skip for %s (toolchain %s: run --verify first, or set BENCH_OLD_SKIPS)\n",
                 caseId, BENCH_TOOLCHAIN);
    std::exit(3);
}

// ---------------------------------------------------------------------------------------
struct Trigger {
    const char* name;
    const char* desc;
    Mode mode;
    user::SiteFn site;
    user::Site siteId;
    const char* printCase;      // case id the printStackTrace call(s) belong to, or nullptr
    const char* getCalledCase;  // case id the getCalledGLfunctionName call(s) belong to
    const char* sideGetCalled;  // getCalled calls that also happen but are not measured
    bool expectLabelGLobject;   // C6: new (and old) stop at glutil::debug::labelGLobject
    void (*prepare)();
};

void prepNone() {}
void prepUserError() { glutil::debug::g_gladCallbackMap.clear(); stub::setNextError(GL_INVALID_OPERATION); }
void prepNoVao() { user::unbindVertexArray(); }
void prepObjectLabel() { stub::g_inject.objectLabel = true; }
void prepCurrentProgram() { user::unbindVertexArray(); stub::g_inject.getIntegervCurrentProgram = true; }
void prepShaderiv() { stub::g_inject.getShaderivCompileStatus = true; }

using namespace user;
const Trigger kTriggers[] = {
    {"C1",      "checkGLErrorPostCallback <- user GL error (errorCheck mode)", Mode::errorCheck, siteBindTextureError, kBindTextureError, "C1", nullptr, nullptr, false, prepUserError},
    {"C2C3",    "fullPostCallback error path: :661 getCalled, :671 print",     Mode::full, siteBindTextureError, kBindTextureError, "C3", "C2", nullptr, false, prepUserError},
    {"C4-327",  "autoLabelGLObjects CreateShader",    Mode::full, siteCreateShader,    kCreateShader,    nullptr, "C4", nullptr, false, prepNone},
    {"C4-332",  "autoLabelGLObjects CreateProgram",   Mode::full, siteCreateProgram,   kCreateProgram,   nullptr, "C4", nullptr, false, prepNone},
    {"C4-345",  "autoLabelGLObjects CompileShader",   Mode::full, siteCompileShader,   kCompileShader,   nullptr, "C4", nullptr, false, prepNone},
    {"C4-407",  "autoLabelGLObjects GenBuffers",      Mode::full, siteGenBuffers,      kGenBuffers,      nullptr, "C4", nullptr, false, prepNone},
    {"C4-426",  "autoLabelGLObjects BindBuffer",      Mode::full, siteBindBuffer,      kBindBuffer,      nullptr, "C4", nullptr, false, prepNone},
    {"C4-437",  "autoLabelGLObjects BindVertexArray", Mode::full, siteBindVertexArray, kBindVertexArray, nullptr, "C4", nullptr, false, prepNone},
    {"C4-450",  "autoLabelGLObjects BindTexture",     Mode::full, siteBindTexture,     kBindTexture,     nullptr, "C4", nullptr, false, prepNone},
    {"C5",      "autoPostInspcector :522 + :529 (DrawArrays, no VAO/program)", Mode::full, siteDrawArrays, kDrawArrays, nullptr, "C5", nullptr, false, prepNoVao},
    {"C6",      "nested: glObjectLabel error in labelGLobject -> checkGLErrorPostCallback", Mode::full, siteCreateShader, kCreateShader, "C6", nullptr, "C4", true, prepObjectLabel},
    {"C7a",     "nested: glGetIntegerv error in autoPostInspcector:526 -> checkGLErrorPostCallback", Mode::full, siteDrawArrays, kDrawArrays, "C7a", nullptr, "C5", false, prepCurrentProgram},
    {"C7b",     "nested: glGetShaderiv error in autoLabelGLObjects:338 -> checkGLErrorPostCallback", Mode::full, siteCompileShader, kCompileShader, "C7b", nullptr, "C4", false, prepShaderiv},
};

struct Variant { const char* name; Method method; int kGetCalled; int kPrint; int cacheMode = 0; };
#ifndef GDM_K_GETCALLED
#error "GDM_K_GETCALLED / GDM_K_PRINT must be defined (read from gdm's debug_stacktrace.cpp by CMake)"
#endif
const Variant kVariants[] = {
    {"old",       Method::Old, 0, 0},
    {"new",       Method::New, GDM_K_GETCALLED, GDM_K_PRINT},  // skip-by-name, gdm's K values, no cache
    {"new_kget6", Method::New, 6, GDM_K_PRINT},                // before the split: one K = 6 for both
    {"cache_hit", Method::New, GDM_K_GETCALLED, GDM_K_PRINT, 1},  // + call-site cache (current gdm), repeated path
    {"cache_miss",Method::New, GDM_K_GETCALLED, GDM_K_PRINT, 2},  // + call-site cache (current gdm), first time
};
bool variantApplies(const Variant& v, const Trigger& t) {
    if (!std::strcmp(v.name, "new_kget6")) return t.getCalledCase != nullptr && GDM_K_GETCALLED != 6;
    if (v.cacheMode != 0) return t.getCalledCase != nullptr;  // the cache only exists for getCalledGLfunctionName
    return true;
}

void runTrigger(const Trigger& t, const Variant& v) {
    auto& c = bench::ctl();
    c.method = v.method;
    c.kGetCalled = v.kGetCalled ? v.kGetCalled : GDM_K_GETCALLED;
    c.kPrint = v.kPrint ? v.kPrint : GDM_K_PRINT;
    c.cacheMode = v.cacheMode;
    c.oldSkipPrint = t.printCase ? oldSkip(t.printCase) : 3;
    const char* gc = t.getCalledCase ? t.getCalledCase : t.sideGetCalled;
    c.oldSkipGetCalled = gc ? oldSkip(gc) : 4;
    glutil::debug::setGladDebugCallbacks(t.mode);
    const bool rec = c.record;
    c.record = false;
    t.prepare();
    c.record = rec;
    user::runAtDepth(t.site, nullptr);
    glutil::debug::setGladDebugCallbacks(Mode::full);
}

bool keptApi(const Trigger& t, Api a) { return a == Api::Print ? t.printCase != nullptr : t.getCalledCase != nullptr; }
const char* caseOf(const Trigger& t, Api a) { return a == Api::Print ? t.printCase : t.getCalledCase; }

// ---------------------------------------------------------------------------------------
struct CaseShape { std::string caseId; Api api; int internalResolved; int internalPhysical; };
std::vector<CaseShape> g_shapes;

int shapeInternal(const char* caseId, Api a) {
    for (auto& s : g_shapes) if (s.caseId == caseId && s.api == a) return s.internalResolved;
    return -1;
}

int kOfApi(Api a) { return a == Api::Print ? GDM_K_PRINT : GDM_K_GETCALLED; }

// Pass 1 (always): measure the correct old skip of every case on the real stack.
void measureSkips() {
    auto& c = bench::ctl();
    g_measuring = true;
    for (const Trigger& t : kTriggers) {
        c.verify = true;
        c.verifies.clear();
        runTrigger(t, kVariants[1]);
        c.verify = false;
        for (const auto& v : c.verifies)
            if (keptApi(t, v.api) && v.aligned && !g_measuredSkip.count(caseOf(t, v.api)))
                g_measuredSkip[caseOf(t, v.api)] = v.derivedOldSkip;
    }
    g_measuring = false;
    g_shapes.clear();
    std::printf("S\t%s\t%s\t%s", BENCH_OPT, BENCH_TOOLCHAIN, tableApplies() ? "table" : "measured");
    for (auto& [k, v] : g_measuredSkip) std::printf("\t%s=%d", k.c_str(), v);
    std::printf("\n");
}

bool verifyAll(bool verbose) {
    auto& c = bench::ctl();
    bool allOk = bench::classifierSelfTest();
    measureSkips();
    for (const Trigger& t : kTriggers) {
        c.verify = true;
        c.verifies.clear();
        runTrigger(t, kVariants[1]);
        c.verify = false;
        int checked = 0;
        for (const auto& v : c.verifies) {
            if (!keptApi(t, v.api)) continue;
            ++checked;
            const char* caseId = caseOf(t, v.api);
            const int table = oldSkip(caseId);
            const std::string expect = t.expectLabelGLobject
                ? std::string("glutil::debug::labelGLobject")
                : "user_code.cpp:" + std::to_string(user::g_siteLine[t.siteId]);
            const bool okTarget = v.firstKept.find(expect) != std::string::npos;
            const bool okSkip = v.aligned && v.derivedOldSkip == table;
            const bool same = v.oldOut == v.newOut;
            // Print only: new is the first `depth` resolved frames of old's output (old is longer
            // only by inline frames in the printed part) -> same target and sequence, OK.
            const bool depthOnly = !same && v.api == bench::Api::Print && v.newPrefixOfOld
                                && v.newFrames == v.depth && v.oldFrames > v.newFrames;
            const bool okSame = same || depthOnly;
            const bool okK = v.internalPhysical <= kOfApi(v.api);  // gdm's K covers this path
            const bool ok = okTarget && okSkip && okSame && okK;
            allOk &= ok;
            g_shapes.push_back({caseId, v.api, v.internalResolved, v.internalPhysical});
            std::printf("[%s] %-7s %-24s %-4s internal: %d resolved / %d physical | old skip table=%d measured=%d | "
                        "target %s | old==new %s | P<=K(%d) %s\n",
                        BENCH_OPT, t.name, bench::apiName(v.api), ok ? "OK" : "FAIL",
                        v.internalResolved, v.internalPhysical, table, v.derivedOldSkip,
                        okTarget ? "ok" : "WRONG", same ? "yes" : depthOnly ? "prefix(depth)" : "NO",
                        kOfApi(v.api), v.internalPhysical <= kOfApi(v.api) ? "yes" : "NO");
            if (verbose || !ok) {
                std::printf("      first frame kept by new: %s (expected %s)\n", v.firstKept.c_str(), expect.c_str());
                std::fputs(v.stackDump.c_str(), stdout);
                if (!okSame) std::printf("      %s\n", v.frameDiff.c_str());
                if (!okSame && verbose) std::printf("      --- old ---\n%s      --- new ---\n%s", v.oldOut.c_str(), v.newOut.c_str());
            } else if (depthOnly) {
                std::printf("      depth semantics: %s\n", v.frameDiff.c_str());
            }
        }
        if (checked == 0) { std::printf("[%s] %-7s FAIL: measured API was never called\n", BENCH_OPT, t.name); allOk = false; }
    }
    return allOk;
}

// ---------------------------------------------------------------------------------------
double pct(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
}

void emit(char kind, const Trigger& t, const Variant& v, Api a, const std::vector<bench::Sample>& s,
          const std::vector<double>& e2e) {
    if (s.empty()) return;
    std::vector<double> tr, to;
    for (auto& x : s) { tr.push_back(x.traceNs); to.push_back(x.totalNs); }
    const auto& f = s.front();
    const int k = v.method == Method::Old ? 0 : (a == Api::Print ? v.kPrint : v.kGetCalled);
    // kind opt trigger case api variant K skipArg requested resolved internal kept n
    // trace_p10 trace_med trace_p90 total_p10 total_med total_p90 e2e_p10 e2e_med e2e_p90
    std::printf("%c\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%llu\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\n",
                kind, BENCH_OPT, t.name, caseOf(t, a), a == Api::Print ? "print" : "getCalled", v.name, k,
                f.skipArg, f.requested, f.resolved, v.method == Method::Old ? 0 : shapeInternal(caseOf(t, a), a), f.kept,
                static_cast<unsigned long long>(s.size()),
                pct(tr, 0.1), pct(tr, 0.5), pct(tr, 0.9), pct(to, 0.1), pct(to, 0.5), pct(to, 0.9),
                e2e.empty() ? 0.0 : pct(e2e, 0.1), e2e.empty() ? 0.0 : pct(e2e, 0.5), e2e.empty() ? 0.0 : pct(e2e, 0.9));
}

void warm(int rounds, int perRound, int warmup) {
    auto& c = bench::ctl();
    for (const Trigger& t : kTriggers) {
        std::vector<const Variant*> vs;
        for (const Variant& v : kVariants) if (variantApplies(v, t)) vs.push_back(&v);
        std::vector<std::vector<bench::Sample>> got(vs.size());
        std::vector<std::vector<double>> e2e(vs.size());  // whole user GL call, incl. the debug layer
        for (const Variant* v : vs) for (int i = 0; i < warmup; ++i) runTrigger(t, *v);
        for (int r = 0; r < rounds; ++r) {
            for (std::size_t j = 0; j < vs.size(); ++j) {
                const std::size_t vi = (j + static_cast<std::size_t>(r)) % vs.size();  // rotate order
                runTrigger(t, *vs[vi]);  // unrecorded: re-primes per-variant state (e.g. the cache after cache_miss emptied it)
                c.samples.clear();
                c.record = true;
                for (int i = 0; i < perRound; ++i) {
                    const auto a0 = std::chrono::steady_clock::now();
                    runTrigger(t, *vs[vi]);
                    const auto a1 = std::chrono::steady_clock::now();
                    e2e[vi].push_back(std::chrono::duration<double, std::nano>(a1 - a0).count());
                }
                c.record = false;
                got[vi].insert(got[vi].end(), c.samples.begin(), c.samples.end());
            }
        }
        for (std::size_t vi = 0; vi < vs.size(); ++vi)
            for (Api a : {Api::Print, Api::GetCalled}) {
                if (!keptApi(t, a)) continue;
                std::vector<bench::Sample> s;
                for (auto& x : got[vi]) if (x.api == a) s.push_back(x);
                emit('W', t, *vs[vi], a, s, e2e[vi]);
            }
    }
}

// Warm cost of resolving each physical frame on its own (median of many single-frame
// resolves). Shows which of the frames new resolves in addition to old are expensive.
bool frameCosts(const char* triggerName) {
    auto& c = bench::ctl();
    // "C4" selects the first C4 site (C4-327); an exact name ("C4-407") selects that site
    const Trigger* sel = nullptr;
    for (const Trigger& t : kTriggers)
        if (!std::strcmp(t.name, triggerName)) { sel = &t; break; }
    if (!sel)
        for (const Trigger& t : kTriggers)
            if (!std::strncmp(t.name, triggerName, std::strlen(triggerName)) && t.name[std::strlen(triggerName)] == '-') { sel = &t; break; }
    if (!sel) {
        std::fprintf(stderr, "unknown case '%s'; one of:", triggerName);
        for (const Trigger& t : kTriggers) std::fprintf(stderr, " %s", t.name);
        std::fprintf(stderr, "\n");
        return false;
    }
    for (const Trigger& t : kTriggers) {
        if (&t != sel) continue;
        c.verify = true; c.captureCosts = true; c.verifies.clear();
        runTrigger(t, kVariants[1]);
        c.verify = false; c.captureCosts = false;
        for (const auto& v : c.verifies) {
            if (!keptApi(t, v.api)) continue;
            std::printf("F\t%s\t%s\t%s\n", BENCH_OPT, t.name, bench::apiName(v.api));
            // capture (unwind only, no resolve) of n frames from the API, median of 101
            for (auto& [n, ns] : v.captureNs)
                std::printf("F\t%s\t%s\tcapture\t%d frames\t%.0f ns\n", BENCH_OPT, t.name, n, ns);
            // resolve of one physical frame (p<i>), median of 300, with what it resolves to
            for (std::size_t i = 0; i < v.raw.frames.size() && i < 12; ++i) {
                cpptrace::raw_trace one;
                one.frames.push_back(v.raw.frames[i]);
                cpptrace::stacktrace r = one.resolve();  // warm the caches
                std::vector<double> ts;
                for (int k = 0; k < 300; ++k) {
                    const auto a = std::chrono::steady_clock::now();
                    cpptrace::stacktrace x = one.resolve();
                    const auto b = std::chrono::steady_clock::now();
                    ts.push_back(std::chrono::duration<double, std::nano>(b - a).count());
                }
                std::string names;
                for (auto& f : r.frames) {
                    if (!names.empty()) names += " | ";
                    names += (f.is_inline ? "[inline] " : "") + f.symbol.substr(0, 60);
                    if (f.line.has_value()) names += " @" + cpptrace::basename(f.filename);
                }
                std::printf("F\t%s\t%s\tp%llu\t%.0f\t%llu\t%s\n", BENCH_OPT, t.name, static_cast<unsigned long long>(i),
                        pct(ts, 0.5), static_cast<unsigned long long>(r.frames.size()), names.c_str());
            }
            break;
        }
    }
    return true;
}

#ifdef _WIN32
// An unhandled SEH exception (access violation, ...) otherwise ends the process silently.
LONG WINAPI reportCrash(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "[bench] CRASH: exception 0x%08lX at %p\n",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    cpptrace::generate_trace().print(std::cerr.rdbuf() == &g_nullBuf ? std::clog : std::cerr);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void setup() {
    // Unbuffered: on Windows a pipe is fully buffered, and a crash would lose everything printed so far.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(reportCrash);
#endif
    std::fprintf(stderr, "[bench] start: opt=%s toolchain=%s K(getCalled)=%d K(print)=%d\n",
                 BENCH_OPT, BENCH_TOOLCHAIN, GDM_K_GETCALLED, GDM_K_PRINT);
    // Experiment knob only (glutil never sets it): BENCH_CACHE=memory|hybrid|speed
    if (const char* m = std::getenv("BENCH_CACHE")) {
        using cm = cpptrace::cache_mode;
        cpptrace::experimental::set_cache_mode(!std::strcmp(m, "memory") ? cm::prioritize_memory
                                               : !std::strcmp(m, "hybrid") ? cm::hybrid : cm::prioritize_speed);
    }
    glutil::Logger::errLogger().setOutput(&g_null);
    glutil::Logger::outLogger().setOutput(&g_null);
    std::cout.rdbuf(&g_nullBuf);  // results use printf/fprintf, not iostreams
    std::cerr.rdbuf(&g_nullBuf);  // glutil::debug::Snapshot::capture() defaults to std::cerr
    if (!gladLoadGL(stub::loader)) { std::fprintf(stderr, "gladLoadGL failed\n"); std::exit(2); }
    glutil::debug::setGladDebugCallbacks(Mode::full);
}

} // namespace

int main(int argc, char** argv) {
    setup();
    const std::string mode = argc > 1 ? argv[1] : "--warm";
    if (mode == "--verify") return verifyAll(argc > 2 && !std::strcmp(argv[2], "-v")) ? 0 : 1;
    if (mode == "--warm") {
        if (!verifyAll(false)) return 1;
        warm(12, 10, 10);
        return 0;
    }
    if (mode == "--frames" && argc == 3) return frameCosts(argv[2]) ? 0 : 2;
    if (mode == "--cold" && argc == 4) {
        for (const Trigger& t : kTriggers) {
            if (std::strcmp(t.name, argv[2])) continue;
            for (const Variant& v : kVariants) {
                if (std::strcmp(v.name, argv[3])) continue;
                auto& c = bench::ctl();
                c.record = true;
                runTrigger(t, v);  // first call of cpptrace in this process
                c.record = false;
                for (Api a : {Api::Print, Api::GetCalled}) {
                    if (!keptApi(t, a)) continue;
                    for (auto& x : c.samples) if (x.api == a) { emit('C', t, v, a, {x}, {}); break; }
                }
                return 0;
            }
        }
        return 2;
    }
    std::fprintf(stderr, "usage: %s --verify [-v] | --warm | --cold <case> <variant> | --frames <case>\n", argv[0]);
    return 2;
}
