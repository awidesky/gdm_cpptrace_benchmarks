// Shared state between the benchmark harness and bench_stacktrace.cpp (which replaces
// glutil's debug_stacktrace.cpp in the benchmark build).
#pragma once

#include <cpptrace/basic.hpp>

#include <string>
#include <vector>

namespace bench {

enum class Method : int { Old, New };
enum class Api : int { Print, GetCalled };

inline const char* apiName(Api a) { return a == Api::Print ? "printStackTrace" : "getCalledGLfunctionName"; }

// One call of a glutil stacktrace API, measured inside the API itself.
struct Sample {
    Api api;
    Method method;
    double traceNs;      // entry -> frames ready (capture + resolve [+ filter])
    double totalNs;      // entry -> return (incl. formatting / snippet)
    int requested;       // max_depth passed to cpptrace (physical frames to capture)
    int skipArg;         // skip passed to cpptrace (old: hardcoded skip, new: 0)
    int resolved;        // frames cpptrace returned (incl. inline frames)
    int internalRemoved; // new: leading internal frames dropped (filled from verify, -1 here)
    int surplusRemoved;  // new: frames dropped at the bottom (filled from verify, -1 here)
    int kept;            // frames used (printed / looked at)
};

// Correctness check of one API call (verify mode only, never timed).
struct VerifyRec {
    Api api;
    std::string oldOut, newOut;  // formatted trace (print) or returned string (getCalled)
    int derivedOldSkip = -1;     // physical frames before the first frame new keeps
    int internalResolved = 0;    // leading internal frames, resolved (incl. inline)
    int internalPhysical = 0;    // leading internal frames, physical only
    bool aligned = true;         // first kept frame starts a physical frame (old can reach it)
    std::string firstKept;       // symbol + file:line of the first frame new keeps
    std::string stackDump;       // full resolved stack (for the report)
    // print only: old prints `depth` PHYSICAL frames (+ their inline frames), new prints `depth`
    // RESOLVED frames. With an inline frame in the printed part old is longer; that is the known
    // depth-semantics difference, not a wrong target, as long as new is a prefix of old.
    int depth = 0, oldFrames = 0, newFrames = 0;
    bool newPrefixOfOld = false;
    std::string frameDiff;       // first differing frame (old vs new), for the report
    std::vector<std::pair<int, double>> captureNs;  // --frames: (frames captured, median ns of generate_raw_trace)
    cpptrace::raw_trace raw;     // physical frames (for the per-frame resolve cost report)
};

struct Control {
    Method method = Method::New;
    int oldSkipPrint = 3;       // hardcoded old skip for the current case
    int oldSkipGetCalled = 4;
    int kPrint = 6;             // kInternalScanDepth used by printStackTrace (new)
    int kGetCalled = 6;         // kInternalScanDepth used by getCalledGLfunctionName (new)
    int cacheMode = 0;          // getCalled raw-address cache (proposal): 0 off, 1 on, 2 on but cleared before every call (always miss)
    bool record = false;
    bool verify = false;
    bool captureCosts = false;  // --frames: also time generate_raw_trace at the real API depth
    std::vector<Sample> samples;
    std::vector<VerifyRec> verifies;
};

Control& ctl();
bool classifierSelfTest();  // isInternalFrame on clang / GCC symbol and path forms

} // namespace bench
