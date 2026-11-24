#!/usr/bin/env python3
"""Runs the glutil stacktrace benchmark repeatedly and aggregates the results.

    python3 tools/run_stacktrace_bench.py [--build build-st] [--runs 7] [--cold-runs 15] [--opts O0,O2]

Warm: each binary run (--warm) verifies first, then measures every case with old/new
interleaved (12 rounds x 10 calls per variant). A key's per-run value is that run's
median; the report gives the median over runs and the min-max range over runs.
Cold: one fresh process per sample (first cpptrace call in the process).
Writes raw TSV + summary.md into results/stacktrace-<platform>-<timestamp>/.
Works on macOS, Linux and Windows (MinGW build: stacktrace_bench_<opt>.exe).
"""
import argparse, datetime, os, platform, statistics, subprocess, sys
from collections import defaultdict

FIELDS = ["kind", "opt", "trigger", "case", "api", "variant", "K", "skip", "requested", "resolved",
          "internal", "kept", "n", "trace_p10", "trace_med", "trace_p90", "total_p10", "total_med", "total_p90",
          "e2e_p10", "e2e_med", "e2e_p90"]
OPTS = ["O0", "O1", "O2", "O3", "Os"]
CASES = ["C1", "C2", "C3", "C4", "C5", "C6", "C7a", "C7b"]
COLD_TRIGGERS = ["C1", "C2C3", "C4-327", "C5", "C6", "C7a", "C7b"]
VARIANTS = ["old", "new", "new_kget6", "cache_hit", "cache_miss"]
COLD_VARIANTS = ["old", "new", "new_kget6"]  # a cache's first call is a miss, i.e. cold == new
# cold value is a true first call only for these (others run after another API call in the same trigger)
TRUE_COLD = {"C1", "C2", "C4", "C5", "C7b"}
CASE_DESC = {
    "C1": "checkGLErrorPostCallback:32 ← 사용자 GL 에러",
    "C2": "fullPostCallback:661 (에러 해시)",
    "C3": "fullPostCallback:671 (에러 출력)",
    "C4": "autoLabelGLObjects 7곳 (라벨링)",
    "C5": "autoPostInspcector:522/529",
    "C6": "중첩: labelGLobject의 GL 에러",
    "C7a": "중첩: autoPostInspcector:526의 GL 에러",
    "C7b": "중첩: autoLabelGLObjects:338의 GL 에러",
}


def parse(lines, kind):
    rows = []
    for ln in lines:
        if not ln.startswith(kind + "\t"):
            continue
        r = dict(zip(FIELDS, ln.rstrip("\r\n").split("\t")))
        for k in FIELDS[6:]:
            r[k] = float(r[k])
        rows.append(r)
    return rows


NTSTATUS = {0xC0000135: "DLL not found (STATUS_DLL_NOT_FOUND): put MinGW's bin dir on PATH or rebuild "
                        "(the build copies the runtime DLLs next to the exe)",
            0xC0000139: "DLL entry point not found (a different libstdc++/libgcc DLL is found first on PATH)",
            0xC0000005: "access violation (crash; see the [bench] CRASH lines above)",
            0xC0000409: "stack buffer overrun / abort",
            0xC000007B: "bad image format (32/64-bit DLL mismatch)"}


def describe_exit(code):
    u = code & 0xFFFFFFFF
    if u in NTSTATUS:
        return f"{code} = 0x{u:08X}: {NTSTATUS[u]}"
    if code == 1:
        return "1: --verify failed (see the FAIL lines)"
    if code == 3:
        return "3: no old skip for a case (run --verify first or set BENCH_OLD_SKIPS)"
    return str(code)


def run(cmd, timeout=300, env=None, log_dir=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        sys.exit(f"TIMEOUT (hang?) after {timeout}s: {' '.join(cmd)}")
    if p.returncode != 0:
        msg = f"FAILED (exit {describe_exit(p.returncode)}): {' '.join(cmd)}\n"
        if log_dir:
            # the whole output (a failing case dumps its stacks, a tail would cut the case line off)
            log = os.path.join(log_dir, "failed-" + os.path.basename(cmd[0]).split(".")[0] + ".txt")
            with open(log, "w", encoding="utf-8") as f:
                f.write(p.stdout + "\n--- stderr ---\n" + p.stderr)
            msg += f"full output: {log}\n"
        # the case lines that failed, each with the detail lines printed under it
        lines, report, keep = p.stdout.splitlines(), [], False
        for l in lines:
            if l.startswith("[") or l.startswith("S\t"):
                keep = " FAIL" in l
                if keep or l.startswith("[classifier]"):
                    report.append(l)
            elif keep and l.startswith("      ") and len(report) < 400:
                report.append(l)
        if report:
            msg += "--- failing cases ---\n" + "\n".join(report) + "\n"
        else:
            msg += f"--- stdout (tail) ---\n{p.stdout[-3000:]}\n"
        msg += f"--- stderr (tail) ---\n{p.stderr[-2000:]}"
        sys.exit(msg)
    return p.stdout.splitlines()


def med(v):
    return statistics.median(v) if v else float("nan")


def us(x):
    return f"{x / 1000:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-st")
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--cold-runs", type=int, default=15)
    ap.add_argument("--opts", default=",".join(OPTS))
    ap.add_argument("--from", dest="src", help="re-aggregate an existing results dir (no runs)")
    ap.add_argument("--tag", default="", help="extra name part of the results dir (e.g. the unwinder: winapi)")
    ap.add_argument("--out", default="", help="write the results into this dir instead of results/stacktrace-...")
    a = ap.parse_args()
    opts = a.opts.split(",")
    bindir = os.path.join(a.build, "test", "stacktrace")
    suffix = ".exe" if os.name == "nt" else ""
    plat = f"{platform.system()}-{platform.machine()}".lower()
    # build configuration of --build (CMAKE_BUILD_TYPE in its CMakeCache.txt): deb / rel
    cfg = "unknown"
    try:
        with open(os.path.join(a.build, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for ln in f:
                if ln.startswith("CMAKE_BUILD_TYPE:"):
                    bt = ln.split("=", 1)[1].strip()
                    cfg = {"Debug": "deb", "RelWithDebInfo": "rel"}.get(bt, bt.lower() or "unknown")
                    break
    except OSError:
        pass
    tag = f"-{a.tag}" if a.tag else ""
    out = a.out or os.path.join("results", f"stacktrace-{cfg}{tag}-{plat}-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))


    warm, cold, verify_lines = [], [], []
    if a.src:
        out = a.src
        def load(name):
            rows = []
            with open(os.path.join(out, name), encoding="utf-8") as f:
                hdr = f.readline().rstrip("\n").split("\t")
                for ln in f:
                    r = dict(zip(hdr, ln.rstrip("\n").split("\t")))
                    for k in FIELDS[6:]:
                        r[k] = float(r[k])
                    rows.append(r)
            return rows
        warm, cold = load("warm.tsv"), load("cold.tsv")
        opts = [o for o in OPTS if any(r["opt"] == o for r in warm)]
    else:
        os.makedirs(out, exist_ok=True)
        def save():
            # written after every level, so a failure at a later level keeps the earlier data
            with open(os.path.join(out, "verify.txt"), "w", encoding="utf-8") as f:
                f.write("\n".join(verify_lines) + "\n")
            for name, rows in (("warm.tsv", warm), ("cold.tsv", cold)):
                with open(os.path.join(out, name), "w", encoding="utf-8") as f:
                    f.write("\t".join(FIELDS + ["run"]) + "\n")
                    for r in rows:
                        f.write("\t".join(str(r.get(k, "")) for k in FIELDS + ["run"]) + "\n")

        # 1) verify every level before measuring anything: a wrong case fails in seconds,
        #    not after the earlier levels' runs
        vouts = {}
        for opt in opts:
            exe = os.path.join(bindir, f"stacktrace_bench_{opt}{suffix}")
            if not os.path.exists(exe):
                sys.exit(f"missing {exe} (build it first)")
            vouts[opt] = run([exe, "--verify"], log_dir=out)
            verify_lines += [l for l in vouts[opt] if l.startswith("[") or l.startswith("S\t")]
            print(f"verify {opt} OK", file=sys.stderr)
        save()
        print(f"RESULTS_DIR={out}")  # for scripts that run this driver

        # 2) measure
        for opt in opts:
            exe = os.path.join(bindir, f"stacktrace_bench_{opt}{suffix}")
            vout = vouts[opt]
            # old skips: the table's (toolchain it was made with) or the ones measured on the real stack
            skips = next((l.rstrip("\r").split("\t") for l in vout if l.startswith("S\t")), None)
            cold_env = dict(os.environ)
            if skips:
                cold_env["BENCH_OLD_SKIPS"] = ",".join(skips[4:])
                # "table": the hardcoded table in bench_main.cpp (made on AppleClang-arm64);
                # "measured": this toolchain is not the table's, so verify derived the correct old
                # skip of every case from the real stack (physical frames before the user frame)
                print(f"{opt} old skips ({skips[2]}, {'from the table' if skips[3] == 'table' else 'measured on this stack'}): "
                      f"{' '.join(skips[4:])}", file=sys.stderr)
            for r in range(a.runs):
                for row in parse(run([exe, "--warm"]), "W"):
                    row["run"] = r
                    warm.append(row)
                print(f"warm {opt} run {r + 1}/{a.runs}", file=sys.stderr)
            for trig in COLD_TRIGGERS:
                for var in COLD_VARIANTS:
                    for _ in range(a.cold_runs):
                        try:
                            p = subprocess.run([exe, "--cold", trig, var], capture_output=True, text=True,
                                               encoding="utf-8", errors="replace", timeout=60, env=cold_env)
                        except subprocess.TimeoutExpired:
                            sys.exit(f"TIMEOUT (hang?): {exe} --cold {trig} {var}")
                        if p.returncode == 2:
                            break  # unknown variant
                        if p.returncode != 0:
                            sys.exit(f"FAILED (exit {describe_exit(p.returncode)}): {exe} --cold {trig} {var}\n{p.stderr[-3000:]}")
                        cold += parse(p.stdout.splitlines(), "C")
            print(f"cold {opt} done", file=sys.stderr)
            save()


    # per (opt, case, variant) -> run -> values (C4: its 7 sites)
    tot = defaultdict(lambda: defaultdict(list))
    trc = defaultdict(lambda: defaultdict(list))
    e2e = defaultdict(lambda: defaultdict(list))
    shape = {}
    for r in warm:
        key = (r["opt"], r["case"], r["variant"])
        tot[key][r["run"]].append(r["total_med"])
        trc[key][r["run"]].append(r["trace_med"])
        e2e[key][r["run"]].append(r["e2e_med"])
        shape.setdefault(key, r)

    def agg(d, key):
        vals = [med(v) for v in d[key].values()]
        return (med(vals), min(vals), max(vals)) if vals else (float("nan"),) * 3

    cold_by = defaultdict(list)
    for r in cold:
        cold_by[(r["opt"], r["case"], r["variant"])].append(r["total_med"])

    def frames(key):
        s = shape.get(key)
        if not s:
            return "-"
        if s["variant"] == "old":
            return f"skip {int(s['skip'])} → 캡처 {int(s['requested'])} → resolve {int(s['resolved'])}"
        if s["variant"] == "cache_hit":
            return f"raw 캡처 {int(s['requested'])} → 캐시 적중 (resolve 0)"
        internal = int(s["internal"])
        surplus = int(s["resolved"] - internal - s["kept"])
        return (f"K={int(s['K'])}: 캡처 {int(s['requested'])} → resolve {int(s['resolved'])} "
                f"(내부 {internal} + 사용 {int(s['kept'])} + 잉여 {surplus})")

    L = [f"# glutil 스택 프레임 스킵 벤치마크 결과 ({plat})\n",
         f"- 생성: {datetime.datetime.now():%Y-%m-%d %H:%M}, warm {a.runs}회 실행 × (12라운드 × 10회, old/new 교차), "
         f"cold {a.cold_runs}회(매번 새 프로세스)",
         "- 값: API 호출 1회당 µs. warm은 '실행별 median'의 median이고 괄호 안은 실행 간 min–max임. C4는 사이트 7곳의 median임",
         "- T_total은 API 함수 진입부터 반환까지, e2e는 사용자 GL 호출 1회 전체(glad 래퍼, 콜백, 라벨링, Snapshot 등 포함)임",
         "- 프레임 표기: old는 `skip`(건너뛴 물리 프레임, resolve하지 않음) → `캡처`(max_depth) → `resolve`이고, "
         "new는 `캡처 K+d → resolve R = 내부(맨 위에서 버림) + 사용 + 잉여(맨 아래에서 버림)`임\n"]
    for api in ("getCalled", "print"):
        cases = [c for c in CASES if any(k[1] == c and shape[k]["api"] == api for k in shape)]
        # new_kget6 only differs for getCalledGLfunctionName
        vs = [v for v in VARIANTS if (api == "getCalled" or v in ("old", "new"))
              and any(k[2] == v and shape[k]["api"] == api for k in shape)]
        title = "getCalledGLfunctionName (라벨링, 빈번함)" if api == "getCalled" else "printStackTrace (에러 출력, 드묾)"
        L += [f"## {title}\n", "### 프레임 수\n", "| opt | case | " + " | ".join(vs) + " |",
              "|---|---|" + "---|" * len(vs)]
        for opt in opts:
            for c in cases:
                L.append(f"| {opt} | {c} | " + " | ".join(frames((opt, c, v)) for v in vs) + " |")
        for label, d in (("T_total (API 1회)", tot), ("T_trace (캡처+resolve+필터)", trc), ("e2e (사용자 GL 호출 1회)", e2e)):
            L += [f"\n### warm: {label}, µs\n",
                  "| opt | case | " + " | ".join(vs) + " | Δ new−old |", "|---|---|" + "---|" * (len(vs) + 1)]
            for opt in opts:
                for c in cases:
                    vals = {v: agg(d, (opt, c, v)) for v in vs}
                    cells = [f"{us(vals[v][0])} ({us(vals[v][1])}–{us(vals[v][2])})" for v in vs]
                    o, n = vals["old"][0], vals["new"][0]
                    L.append(f"| {opt} | {c} {CASE_DESC[c] if label.startswith('T_total') else ''} | "
                             + " | ".join(cells) + f" | +{us(n - o)} (×{n / o:.2f}) |")
        L += ["\n### cold: 프로세스 첫 호출 T_total, ms (median, min–max). *는 같은 트리거에서 앞선 API 호출 뒤의 값\n",
              "| opt | case | " + " | ".join(vs) + " |", "|---|---|" + "---|" * len(vs)]
        for opt in opts:
            for c in cases:
                cells = []
                for v in vs:
                    vals = cold_by.get((opt, c, v), [])
                    cells.append(f"{med(vals) / 1e6:.2f} ({min(vals) / 1e6:.2f}–{max(vals) / 1e6:.2f})" if vals else "-")
                L.append(f"| {opt} | {c}{'' if c in TRUE_COLD else '*'} | " + " | ".join(cells) + " |")
        L.append("")
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print(out)


if __name__ == "__main__":
    main()
