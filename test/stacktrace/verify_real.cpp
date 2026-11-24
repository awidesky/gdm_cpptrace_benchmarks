// Correctness check of gdm's REAL src/debug_stacktrace.cpp (not the benchmark's copy).
//
// Same GL stub and user call sites as the benchmark, but nothing of glutil is replaced, so
// whatever gdm ships is what is checked: the skip-by-name classifier, kInternalScanDepth*,
// the call-site cache. Black box, through what glutil actually outputs:
//   getCalledGLfunctionName  -> the labels glutil passes to glObjectLabel (C4), and the
//                               "(called from ...)" of autoPostInspcector's messages (C5)
//   printStackTrace          -> the trace written to glutil's error logger (C1 C3 C6 C7a C7b):
//                               its first frame, and that it has the full depth (15 frames;
//                               fewer would mean more internal frames than kInternalScanDepthPrint)
//   cache                    -> the same site run again gives the same call-site string
// Expected positions: the user's GL call line (user_code.cpp), except C6, which is reported at
// glutil::debug::labelGLobject (glutil's own GL call, outside the callbacks).
// Exit code 0 = every check passed.
#include <glutil/gl.hpp>
#include <glutil/debug_callback.hpp>
#include <glutil/logging.hpp>

#include "hashing.hpp"  // glutil/src: g_gladCallbackMap (error aggregation state)

#include <cstdio>
#include <iostream>
#include <regex>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "gl_stub.hpp"
#include "user_code.hpp"

#ifndef BENCH_OPT
#define BENCH_OPT "?"
#endif

namespace {

using Mode = glutil::debug::GladDebugCallbacksMode;

struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
};
NullBuf g_nullBuf;
std::ostream g_null(&g_nullBuf);
std::ostringstream g_err;  // glutil's error logger, per check

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, const std::string& detail) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %-4s %s\n", BENCH_OPT, ok ? "OK" : "FAIL", what.c_str());
    if (!ok) std::printf("       %s\n", detail.c_str());
}

std::string loc(user::Site s) { return "user_code.cpp:" + std::to_string(user::g_siteLine[s]); }

// "<file>:<line>" followed by a non-digit (":col", " ", end): user_code.cpp:18 must not match :180
bool hasLocation(const std::string& text, const std::string& location) {
    for (std::size_t p = text.find(location); p != std::string::npos; p = text.find(location, p + 1)) {
        const std::size_t e = p + location.size();
        if (e >= text.size() || !std::isdigit(static_cast<unsigned char>(text[e]))) return true;
    }
    return false;
}

struct Trace { std::string header, first; int frames = 0; };
// The first stack trace in the logger output after the line containing `header`.
Trace traceAfter(const std::string& out, const std::string& header) {
    Trace t;
    std::istringstream in(out);
    std::string line;
    bool inTrace = false;
    static const std::regex frame(R"(^\s*#\d+\s)");
    while (std::getline(in, line)) {
        if (!inTrace) {
            if (line.find(header) != std::string::npos) { inTrace = true; t.header = line; }
            continue;
        }
        if (std::regex_search(line, frame)) {
            if (t.frames == 0) t.first = line;
            ++t.frames;
        } else if (t.frames > 0 && !line.empty() && line[0] != ' ' && line[0] != '\t') {
            break;  // next log entry
        }
    }
    return t;
}

void run(Mode mode, user::SiteFn site, void (*prepare)() = nullptr) {
    g_err.str({});
    g_err.clear();
    stub::g_labels.clear();
    glutil::debug::setGladDebugCallbacks(mode);
    if (prepare) prepare();
    user::runAtDepth(site, nullptr);
    glutil::debug::setGladDebugCallbacks(Mode::full);
}

void prepUserError() { glutil::debug::g_gladCallbackMap.clear(); stub::setNextError(GL_INVALID_OPERATION); }
void prepNoVao() { user::unbindVertexArray(); }

constexpr int kDepth = 15;  // printStackTrace's default depth, as glutil's callbacks call it

void checkPrint(const char* name, const std::string& header, const std::string& expect) {
    const Trace t = traceAfter(g_err.str(), header);
    check(!t.header.empty(), std::string(name) + " printStackTrace: trace printed (" + header + ")",
          "no \"" + header + "\" in the error log:\n" + g_err.str());
    if (t.header.empty()) return;
    const bool okFirst = expect.rfind("user_code.cpp:", 0) == 0 ? hasLocation(t.first, expect)
                                                                : t.first.find(expect) != std::string::npos;
    check(okFirst, std::string(name) + " printStackTrace: first frame is " + expect, "first frame: " + t.first);
    check(t.frames == kDepth, std::string(name) + " printStackTrace: " + std::to_string(kDepth) + " frames",
          std::to_string(t.frames) + " frames (internal frames > kInternalScanDepthPrint?)");
}

// getCalled through the labels: one of them carries the site's location
void checkLabels(const char* name, const std::string& expect, std::string* site = nullptr) {
    std::string found;
    for (const std::string& l : stub::g_labels)
        if (hasLocation(l, expect)) { found = l; break; }
    std::string all;
    for (const std::string& l : stub::g_labels) all += "\n         \"" + l + "\"";
    check(!found.empty(), std::string(name) + " getCalledGLfunctionName: label has " + expect,
          stub::g_labels.empty() ? std::string("no glObjectLabel call") : "labels:" + all);
    if (site && !found.empty()) {
        const std::size_t p = found.find("user_code.cpp:");
        *site = found.substr(p, found.find(')', p) == std::string::npos ? std::string::npos : found.find(')', p) - p);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    glutil::Logger::errLogger().setOutput(&g_err);
    glutil::Logger::outLogger().setOutput(&g_null);
    std::cout.rdbuf(&g_nullBuf);
    std::cerr.rdbuf(&g_nullBuf);  // Snapshot::capture() defaults to std::cerr
    stub::g_recordLabels = true;
    if (!gladLoadGL(stub::loader)) { std::printf("gladLoadGL failed\n"); return 2; }
    glutil::debug::setGladDebugCallbacks(Mode::full);
    std::printf("[%s] gdm's real debug_stacktrace.cpp, K(getCalled)=%d K(print)=%d\n", BENCH_OPT, GDM_K_GETCALLED, GDM_K_PRINT);

    using namespace user;
    // C1: user GL error, errorCheck mode -> checkGLErrorPostCallback:32 -> printStackTrace
    run(Mode::errorCheck, siteBindTextureError, prepUserError);
    checkPrint("C1", "in function glBindTexture", loc(kBindTextureError));
    // C2/C3: user GL error, full mode -> fullPostCallback:661 getCalled (hash), :671 printStackTrace
    run(Mode::full, siteBindTextureError, prepUserError);
    checkPrint("C3", "In function glBindTexture", loc(kBindTextureError));
    // C4: autoLabelGLObjects -> getCalled in the labels; run twice: the second is a cache hit
    struct { const char* name; SiteFn fn; Site id; } c4[] = {
        {"C4-327", siteCreateShader, kCreateShader},     {"C4-332", siteCreateProgram, kCreateProgram},
        {"C4-345", siteCompileShader, kCompileShader},   {"C4-426", siteBindBuffer, kBindBuffer},
        {"C4-437", siteBindVertexArray, kBindVertexArray}, {"C4-450", siteBindTexture, kBindTexture},
    };
    for (auto& s : c4) {
        std::string first, second;
        run(Mode::full, s.fn);
        checkLabels(s.name, loc(s.id), &first);
        run(Mode::full, s.fn);
        checkLabels((std::string(s.name) + " (2nd call: cache)").c_str(), loc(s.id), &second);
        if (!first.empty() && !second.empty())
            check(first == second, std::string(s.name) + " cache: same call-site string twice", "\"" + first + "\" vs \"" + second + "\"");
    }
    // C5: autoPostInspcector :522/:529 -> "(called from <getCalled>)"
    run(Mode::full, siteDrawArrays, prepNoVao);
    {
        const std::string out = g_err.str();
        const std::size_t p = out.find("No VAO bound for glDrawArrays");
        const std::string msg = p == std::string::npos ? std::string() : out.substr(p, out.find('\n', p) - p);
        check(hasLocation(msg, loc(kDrawArrays)), "C5 getCalledGLfunctionName: autoPostInspcector names " + loc(kDrawArrays),
              msg.empty() ? "no \"No VAO bound\" message:\n" + out : msg);
    }
    // C6: glutil's own glObjectLabel fails -> reported at glutil::debug::labelGLobject
    run(Mode::full, siteCreateShader, [] { stub::g_inject.objectLabel = true; });
    checkPrint("C6", "in function glObjectLabel", "glutil::debug::labelGLobject");
    // C7a/C7b: glutil's GL call inside the callbacks fails -> the user call being handled
    run(Mode::full, siteDrawArrays, [] { user::unbindVertexArray(); stub::g_inject.getIntegervCurrentProgram = true; });
    checkPrint("C7a", "in function glGetIntegerv", loc(kDrawArrays));
    run(Mode::full, siteCompileShader, [] { stub::g_inject.getShaderivCompileStatus = true; });
    checkPrint("C7b", "in function glGetShaderiv", loc(kCompileShader));

    std::printf("[%s] %s: %d checks, %d failed\n", BENCH_OPT, g_failures ? "FAIL" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
