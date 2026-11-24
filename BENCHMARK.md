# glutil stack-frame skipping benchmark

Measures the **extra cost of gdm's new way of skipping glutil's internal stack frames**
(capture with `skip = 0`, then drop the frames whose symbol is internal: gdm 358d42c and
later) compared with the **old way** (a hardcoded `skip` passed to
`cpptrace::stacktrace::current(skip, depth)`), on the real call stacks of every place gdm
uses it.

Detailed background, the full survey of call sites, and the reasoning behind every decision
are in [analysis.md](analysis.md).

## How it works

- **Real code, fake GPU.** `test/stacktrace/` compiles gdm's real glutil sources and the real
  glad DEBUG loader (`gl.c`) from the gdm checkout, and hands glad a stub GL driver
  (`gl_stub.cpp`). The user's GL calls go through glad's debug wrapper and glutil's callbacks
  exactly as in an application: same functions, same TUs, same inlining, same symbols.
- **Only `debug_stacktrace.cpp` is replaced** by `bench_stacktrace.cpp`. In the same functions
  (same symbols, same stack), it switches at run time between:
  - **old**: the 358d42c^ body, `current(skip, depth)`. `skip` is the value that is correct for
    that case and optimization level (`kOldSkip` in `bench_main.cpp`). It is checked against the
    real stack, so old is measured at its best and never on a wrong frame.
  - **new**: the gdm HEAD body. `kInternalScanDepthGetCalled` / `kInternalScanDepthPrint` are
    read from gdm's `debug_stacktrace.cpp` at configure time, so "new" is always what gdm ships.
    `new_kget6` is the same body with the earlier single value K=6, for comparison.
- **User stack**: every GL call site sits 32 non-inlined user frames deep (`user_code.cpp`), so
  new's `K + depth` capture is filled with real frames, as in an application.
- **Correctness gate (`--verify`)**: before timing anything, each case is checked:
  - new keeps the expected first frame (the user's GL call, or `labelGLobject` for C6);
  - the old skip in the table equals the physical-frame count measured on the real stack;
  - old and new produce identical output;
  - the path's internal physical frames P fit into gdm's K.
  Any failure makes the run (and `ctest`) fail.
- **Timing** is taken inside the API: `T_trace` (capture + resolve + filter) and `T_total`
  (whole API call). `e2e` is the whole user GL call (debug layer included). old and new run
  interleaved; each binary run gives medians; the driver repeats runs and reports the median of
  the run medians with the min–max over runs. `cold` is the first call in a fresh process.
- Build matrix: `-O0` (= gdm Debug), `-O1`, `-O2` (= gdm RelWithDebInfo), `-O3`, `-Os`, all
  `-g`, `GDM_DEBUG=1`.

## Cases (every use in gdm, nothing else)

| case | call site (`external/glutil/src/debug_callback.cpp`) | API | internal frames P (O0 / O1+) | old skip |
|---|---|---|---|---|
| C1 | `:32` `checkGLErrorPostCallback` ← user GL error | printStackTrace | 3 / 3 | 3 |
| C2 | `:661` `fullPostCallback` error hash | getCalledGLfunctionName | 3 / 3 | 3 |
| C3 | `:671` `fullPostCallback` error print | printStackTrace | 3 / 3 | 3 |
| C4 | `:327 332 345 407 426 437 450` `autoLabelGLObjects` (labels) | getCalledGLfunctionName | 4 / 4 | 4 |
| C5 | `:522 529` `autoPostInspcector` (draw without VAO / program) | getCalledGLfunctionName | 4 / 3 (+1 inline) | 4 / **3** |
| C6 | `:32` nested: GL error inside `labelGLobject` | printStackTrace | 3 / 3 | 3 |
| C7a | `:32` nested: GL error at `autoPostInspcector:526` | printStackTrace | 6 / 5 (+1 inline) | 6 / **5** |
| C7b | `:32` nested: GL error at `autoLabelGLObjects:338` | printStackTrace | 6 / 6 | 6 |

## Running

gdm is a subproject, fetched like ogl-gdm does: `cmake/FetchGDM.cmake` keeps a copy of
gdm in `external/gdm` (tracking `GDM_TAG`, default `master`), and gdm's own configure then
generates glad and fetches glfw / cpptrace / stb / ... into `GDM_EXTERNAL_DIR` (default
`external/`). Nothing has to be configured beforehand. `-DGDM_SOURCE_DIR=<checkout>` uses a
local gdm checkout instead, e.g. to benchmark uncommitted changes. The build type must be
`Debug` or `RelWithDebInfo` (default), because gdm compiles glutil's debug code only there.

### macOS / Linux

```sh
cmake -S . -B build-st                                   # add -DGDM_SOURCE_DIR=../gdm for a local checkout
cmake --build build-st -j8
ctest --test-dir build-st -R stacktrace                  # correctness gate only
python3 tools/run_stacktrace_bench.py --build build-st   # writes results/stacktrace-<deb|rel>-<platform>-<time>/
```

### CI (`.github/workflows/verify.yml`)

Correctness only (no timings), on every push to master and by hand (Actions > verify > Run
workflow, optionally with a gdm branch/tag/commit): Linux GCC and Clang, macOS Clang, Windows MSVC
and MinGW, each in Debug (O0) and RelWithDebInfo (O1..Os). Per level it runs the benchmark's
`--verify` and `stacktrace_verify_real_<opt>`, which checks gdm's real `debug_stacktrace.cpp`
(black box: the labels glutil sets and the traces it logs). A change in gdm does not start it.

### Windows (MSVC, MinGW): see analysis.md §11

Everything with MSVC and MinGW GCC (cpptrace unwinders dbghelp / winapi / unwind(GCC only) x
Debug / RelWithDebInfo: verify, frame costs, benchmark), one command from the gdm_benchmarks
folder; a toolchain that is not installed is skipped (MinGW needs no Visual Studio):

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1
```

Results: `results\windows-gdm_<local|master-sha>-<time>\<toolchain>_<BuildType>_<levels>_unwind-<unwinder>\`
(e.g. `msvc-vs17.14_RelWithDebInfo_O1-O2-O3_unwind-dbghelp`), with `comparison.md` (all
combinations side by side) in the top folder. MSVC levels: O0 `/Od`, O1 `/O1`, O2 `/O2`,
O3 `/O2 /Ob3`. The harness builds with GCC, Clang and MSVC (`test/stacktrace/portable.hpp`).

```powershell
cmake -S gdm_benchmarks -B "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TLS_VERIFY=OFF `
  -DCMAKE_C_COMPILER="c:\mingw64\bin\gcc.exe" -DCMAKE_CXX_COMPILER="c:\mingw64\bin\g++.exe" `
  -DGDM_EXTERNAL_DIR="$env:USERPROFILE\Downloads\external" -DSTACKTRACE_BENCH_OPTS=O0
cmake --build "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" -j 8
python gdm_benchmarks\tools\run_stacktrace_bench.py --build "$env:USERPROFILE\Downloads\build_gdm_bench_dbg" --opts O0
```

Other modes of a single binary: `--verify [-v]` (with `-v`, dumps every stack),
`--frames <case>` (warm cost of resolving each frame on its own), and
`--cold <case> <variant>`.

## Results: macOS 15 (Darwin 24.6), Apple M-series arm64, Apple clang 17, cpptrace 1.0.4

Raw data: `results/stacktrace-darwin-arm64-20261123-115957/` (`summary.md` has every level).
7 warm runs × (12 rounds × 10 calls), 15 cold processes. µs per API call (median of run medians).

**getCalledGLfunctionName** (labeling; frequent)

| build | case | old: frames → µs | new (K=4): frames → µs | Δ | new_kget6 µs |
|---|---|---|---|---|---|
| O0 | C2 | skip 3, resolve 1 → **37.4** | capture 5, resolve 5 (3 internal + 1 + 1 surplus) → **190.3** | +153 (×5.1) | 261.3 |
| O0 | C4 | skip 4, resolve 1 → **36.5** | capture 5, resolve 5 (4 + 1 + 0) → **192.6** | +156 (×5.3) | 265.6 |
| O0 | C5 | skip 4, resolve 1 → **36.5** | capture 5, resolve 5 (4 + 1 + 0) → **191.2** | +155 (×5.2) | 264.7 |
| O2 | C2 | skip 3, resolve 1 → **13.2** | capture 5, resolve 5 (3 + 1 + 1) → **148.0** | +135 (×11) | 169.5 |
| O2 | C4 | skip 4, resolve 1 → **12.2** | capture 5, resolve 5 (4 + 1 + 0) → **272.6** | +260 (×22) | 294.8 |
| O2 | C5 | skip 3, resolve 1 → **11.9** | capture 5, resolve 6 (4 incl. 1 inline + 1 + 1) → **178.0** | +166 (×15) | 200.9 |

O1 and O3 are within 5% of O2; Os is 13–20% lower. Cold (first call in a process): old
1.5 ms (O0: 4.3 ms), new 22–23 ms.

**printStackTrace** (error output; rare), K=6 unchanged

| build | case | old | new | Δ |
|---|---|---|---|---|
| O0 | C1 / C3 / C6 | skip 3, resolve 15 → 584–594 | capture 21, resolve 21 (3 + 15 + 3) → 809–824 | +225–230 (×1.39) |
| O0 | C7a / C7b | skip 6, resolve 15 → 579–583 | capture 21, resolve 21 (6 + 15 + 0) → 814–819 | +231–240 (×1.40) |
| O2 | C1 | skip 3 → 217 | (3 + 15 + 3) → 321 | +104 (×1.48) |
| O2 | C3 | skip 3 → 218 | 368 | +150 (×1.69) |
| O2 | C6 | skip 3 → 413 | 520 | +107 (×1.26) |
| O2 | C7a | skip 5 → 214 | capture 21, resolve 22 (6 + 15 + 1) → 412 | +197 (×1.92) |
| O2 | C7b | skip 6 → 216 | (6 + 15 + 0) → 542 | +326 (×2.51) |

**Why**: old never resolves the frames it skips, while new has to resolve them to read their
names. At O1 and above, resolving a frame of a large function with many inlined callees
(`autoLabelGLObjects`, `fullPostCallback`) costs 84–122 µs warm, against about 12 µs for a glad or
user frame (`--frames`). Lowering K only removes cheap surplus user frames, so getCalled K=4
saves about 22 µs (O2) / 73 µs (O0) per call compared with K=6.

Impact on applications (startup, render loop, error loops) is analysed in analysis.md §10;
expected old / new / new+cache costs of every ogl-gdm and glutil-examples target: §13.

## Raw-address result cache for getCalledGLfunctionName (now in gdm)

Variants `cache_hit` / `cache_miss` measure the cache that gdm's `getCalledGLfunctionName`
now has (same key: skip + raw return addresses); `new` is skip-by-name without the cache.
The cache captures the top `K + skip + 1` frames raw (no resolve), looks that address
sequence up, and resolves only on a miss. Raw data: `results/stacktrace-darwin-arm64-20261123-124336/`.

| per getCalled call | old | new | cache hit | cache miss |
|---|---|---|---|---|
| O2, C4 (labeling) | 12.1 µs | 320.2 µs | **0.2 µs** | 313.4 µs |
| O2, C2 (error hash) | 13.5 µs | 191.8 µs | **0.5 µs** | 185.9 µs |
| O0, C4 | 34.2 µs | 181.6 µs | **0.5 µs** | 184.9 µs |

A hit is 25–60× faster than old; a miss costs the same as new. Scenario-level comparison
(loading, render-loop error/warning loops): analysis.md §10.5.

## Caveats

- Stub GL driver: real driver time (`glObjectLabel`, ...) is not part of `e2e`; the API-internal
  times are unaffected by it.
- Measured on macOS arm64 only so far. Other ABIs can change inlining and tail calls; `--verify`
  tells whether the old-skip table still holds there.
- C3, C6 and C7a "cold" values are not true first calls (another API call precedes them in
  the same GL call).
