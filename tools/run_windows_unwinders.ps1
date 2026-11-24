<#
.SYNOPSIS
  Windows: build and run the whole stacktrace benchmark (verification + measurements) with
  MSVC and MinGW GCC, for every cpptrace unwinder, in Debug and RelWithDebInfo.
  One command, from the gdm_benchmarks folder:

    powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1

  or, in an open PowerShell window (allows scripts for this window only):

    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force; .\tools\run_windows_unwinders.ps1

  A toolchain that is not installed is skipped with a notice (MSVC needs Visual Studio or its
  Build Tools with the C++ workload; MinGW needs no Visual Studio). About 1.5-2 hours for both.
  Help: Get-Help .\tools\run_windows_unwinders.ps1 -Full

.DESCRIPTION
  Combinations (each in its own build dir <BuildRoot>\build_gdm_bench_<toolchain>_<BuildType>_<unwinder>):
    toolchain   unwinders (cpptrace CPPTRACE_UNWIND_WITH_*)          levels
    msvc        dbghelp, winapi                                      Debug: O0    RelWithDebInfo: O1 O2 O3
    mingw       dbghelp, winapi, unwind                              Debug: O0    RelWithDebInfo: O1 O2 O3 Os
      dbghelp = dbghelp StackWalk64 (cpptrace's default on Windows, both toolchains)
      winapi  = RtlCaptureStackBackTrace
      unwind  = libgcc _Unwind_Backtrace (GCC only)
    MSVC levels: O0 = /Od, O1 = /O1, O2 = /O2, O3 = /O2 /Ob3 (MSVC has no Os apart from /O1).
  Per combination:
    1. configure (only that unwinder ON, the others OFF)
    2. check the unwinder cpptrace is really compiled with (its CPPTRACE_UNWIND_WITH_* define)
    3. build the benchmark executables
    4. per level: --verify (all cases C1..C7b: new reaches the intended frame, equals old with the
       correct skip, internal frames <= K, cache path = plain path), the same intended frames with
       gdm's real debug_stacktrace.cpp (stacktrace_verify_real), and --frames C4 / C1
       (cost of capturing n frames, and of resolving each frame)
    5. the benchmark (warm and cold; old / new / new K=6 / cache hit / cache miss)
  A failing step skips the rest of that combination; the others still run.

  gdm under test:
    - external\gdm without a GDM_COMMIT file (a checkout copied there by hand, e.g. with
      unpushed changes) is used as is (-DGDM_SOURCE_DIR), so configuring cannot replace it;
    - otherwise gdm master is fetched into external\gdm (gdm_benchmarks' default);
    - -GdmSourceDir <dir> uses that checkout.
    Before anything is measured, the script stops if that gdm lacks the stack-trace changes
    the benchmark is about (split kInternalScanDepth, the MinGW callback-frame fix).

  RESULTS (one folder per run of this script; the names say what was run):
    results\windows-gdm_<local|master-sha>-<yyyyMMdd-HHmmss>\
      comparison.md                                   every combination side by side (start here)
      summary.txt                                     status of each combination
      environment.txt                                 tool versions, gdm used, build dirs
      msvc-vs17.14_Debug_O0_unwind-dbghelp\           summary.md (full tables), warm.tsv, cold.tsv,
      msvc-vs17.14_RelWithDebInfo_O1-O2-O3_unwind-winapi\   verify.txt, verify-<opt>.txt,
      mingw-gcc16.1.0_Debug_O0_unwind-unwind\         frames-<opt>-C4.txt, frames-<opt>-C1.txt,
      ...                                             logs\ (configure, build, bench)

  REQUIREMENTS: CMake >= 3.24 (>= 4.1 with Visual Studio 2026), git (cpptrace clones libdwarf
  at configure time), Python 3, network access to GitHub; plus, per toolchain, Visual Studio /
  Build Tools with "Desktop development with C++" (msvc), MinGW-w64 GCC with mingw32-make
  (mingw, default c:\mingw64\bin). dbghelp.dll ships with Windows. Everything is checked first.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1 -Toolchains msvc -Configs rel -Runs 1 -ColdRuns 1
  (quick check, a few minutes)
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1 -Toolchains mingw -CC D:\mingw64\bin\gcc.exe -CXX D:\mingw64\bin\g++.exe
#>
param(
    [string[]] $Toolchains   = @("msvc", "mingw"),
    [string[]] $Unwinders    = @("dbghelp", "winapi", "unwind"),   # per toolchain, those it supports
    [string[]] $Configs      = @("deb", "rel"),
    [string]   $CC           = "c:\mingw64\bin\gcc.exe",           # MinGW only
    [string]   $CXX          = "c:\mingw64\bin\g++.exe",           # MinGW only
    [int]      $Runs         = 7,    # driver --runs      (warm processes per level)
    [int]      $ColdRuns     = 15,   # driver --cold-runs (first-call processes per case)
    [int]      $Jobs         = [Environment]::ProcessorCount,
    [string]   $Python       = "python",
    [string]   $GdmSourceDir = "",   # a gdm checkout to test (default: see DESCRIPTION)
    [string]   $BuildRoot    = "",   # where the build dirs go (default: next to gdm_benchmarks)
    [switch]   $Fresh                # cmake --fresh: recreate the caches of existing build dirs
)

$ErrorActionPreference = "Continue"   # native tools write to stderr; exit codes decide
function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Note([string]$msg) { Write-Host "NOTE: $msg" -ForegroundColor Yellow }

# "powershell -File" passes "-Unwinders winapi,unwind" as ONE string; split lists either way
function Split-List($v) { @($v | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ }) }
$Toolchains = Split-List $Toolchains
$Unwinders  = Split-List $Unwinders
$Configs    = Split-List $Configs
foreach ($x in $Toolchains) { if ($x -notin @("msvc", "mingw"))              { Fail "unknown toolchain '$x' (msvc, mingw)" } }
foreach ($x in $Unwinders)  { if ($x -notin @("dbghelp", "winapi", "unwind")) { Fail "unknown unwinder '$x' (dbghelp, winapi, unwind)" } }
foreach ($x in $Configs)    { if ($x -notin @("deb", "rel"))                  { Fail "unknown config '$x' (deb, rel)" } }
$explicitToolchains = $PSBoundParameters.ContainsKey("Toolchains")

$Src = Split-Path -Parent $PSScriptRoot             # gdm_benchmarks
if (-not $BuildRoot) { $BuildRoot = Split-Path -Parent $Src }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$AllUnwinders = @("DBGHELP", "WINAPI", "UNWIND", "LIBUNWIND", "EXECINFO", "NOTHING")

# Runs a native command with all its output in $Log; returns the exit code.
function Invoke-Logged([string]$Log, [string]$Exe, [string[]]$Arguments) {
    "> $Exe $($Arguments -join ' ')" | Out-File -FilePath $Log -Encoding utf8
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -FilePath $Log -Append -Encoding utf8
    return $LASTEXITCODE
}
function Show-Tail([string]$Log, [int]$Lines = 25) {
    if (Test-Path $Log) { Get-Content $Log -Tail $Lines | ForEach-Object { Write-Host "    $_" } }
}
function First-Line([string]$Exe, [string[]]$Arguments) {
    try { return ((& $Exe @Arguments 2>&1 | Select-Object -First 1) -as [string]).Trim() } catch { return "" }
}

# ---- 0. common tools -------------------------------------------------------------------------
$env_lines = New-Object System.Collections.Generic.List[string]
foreach ($t in @(@("CMake", "cmake", "--version"), @("git", "git", "--version"), @("Python", $Python, "--version"))) {
    if (-not (Get-Command $t[1] -ErrorAction SilentlyContinue)) { Fail "$($t[0]) not found ($($t[1])). Install it and put it on PATH." }
    $env_lines.Add("$($t[0]): $(First-Line $t[1] @($t[2]))")
}
# "python" can be the Microsoft Store alias, which only opens the Store
if ((First-Line $Python @("--version")) -notmatch "Python 3") { Fail "'$Python --version' is not Python 3 (Store alias? pass -Python <path to python.exe>)." }
$cmv = [regex]::Match((First-Line "cmake" @("--version")), "(\d+)\.(\d+)")
$cmakeVer = if ($cmv.Success) { [int]$cmv.Groups[1].Value * 100 + [int]$cmv.Groups[2].Value } else { 0 }
if ($cmakeVer -lt 324) { Fail "CMake >= 3.24 is required (gdm)." }

# ---- toolchains ------------------------------------------------------------------------------
$tc = [ordered]@{}
foreach ($name in $Toolchains) {
    if ($name -eq "msvc") {
        # same resolution as gdm's CI: the newest Visual Studio with the x64 C++ toolset
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        $why = ""; $vsVer = ""; $gen = ""
        if (-not (Test-Path $vswhere)) { $why = "Visual Studio is not installed (no vswhere.exe)" }
        else {
            $vsVer = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion | Select-Object -First 1)
            if (-not $vsVer) { $why = "no Visual Studio with the C++ x64 toolset (Desktop development with C++)" }
            else {
                $major = [int]($vsVer.Split(".")[0])
                $gen = @{ 16 = "Visual Studio 16 2019"; 17 = "Visual Studio 17 2022"; 18 = "Visual Studio 18 2026" }[$major]
                if (-not $gen) { $why = "unsupported Visual Studio version $vsVer" }
                elseif ($major -ge 18 -and $cmakeVer -lt 401) { $why = "Visual Studio 2026 needs CMake >= 4.1" }
            }
        }
        if ($why) {
            if ($explicitToolchains) { Fail "msvc: $why" }
            Note "msvc skipped: $why"; continue
        }
        $short = ($vsVer.Split(".")[0..1] -join ".")
        $tc["msvc"] = @{ Label = "msvc-vs$short"; Unwinders = @("dbghelp", "winapi"); RelOpts = @("O1", "O2", "O3")
                         Args = @("-G", $gen, "-A", "x64") }
        $env_lines.Add("msvc: Visual Studio $vsVer ($gen)")
    } else {
        $why = ""
        foreach ($c in @($CC, $CXX)) { if (-not (Test-Path $c)) { $why = "compiler not found: $c (pass -CC / -CXX)" } }
        $make = ""
        if (-not $why) {
            $make = Join-Path (Split-Path -Parent $CXX) "mingw32-make.exe"
            if (-not (Test-Path $make)) {
                $m = Get-Command "mingw32-make" -ErrorAction SilentlyContinue
                if ($m) { $make = $m.Source } else { $why = "mingw32-make not found next to $CXX or on PATH" }
            }
        }
        if ($why) {
            if ($explicitToolchains) { Fail "mingw: $why" }
            Note "mingw skipped: $why"; continue
        }
        $gccVer = First-Line $CXX @("-dumpfullversion")
        $tc["mingw"] = @{ Label = "mingw-gcc$gccVer"; Unwinders = @("dbghelp", "winapi", "unwind"); RelOpts = @("O1", "O2", "O3", "Os")
                          Args = @("-G", "MinGW Makefiles", "-DCMAKE_C_COMPILER=$CC", "-DCMAKE_CXX_COMPILER=$CXX", "-DCMAKE_MAKE_PROGRAM=$make") }
        $env_lines.Add("mingw: $CXX (GCC $gccVer), $make")
    }
}
if ($tc.Count -eq 0) { Fail "no toolchain available (see the notes above)" }

# ---- gdm under test ------------------------------------------------------------------------
$ext = Join-Path $Src "external\gdm"
if ($GdmSourceDir) {
    $GdmSourceDir = (Resolve-Path $GdmSourceDir).Path; $gdmId = "local"
} elseif ((Test-Path (Join-Path $ext "CMakeLists.txt")) -and -not (Test-Path (Join-Path $ext "GDM_COMMIT"))) {
    $GdmSourceDir = $ext; $gdmId = "local"     # hand-copied checkout: never let FetchGDM replace it
} else {
    $gdmId = "master"                          # fetched by the first configure
}
function Test-GdmHasChanges([string]$dir) {
    $f = Join-Path $dir "external\glutil\src\debug_stacktrace.cpp"
    if (-not (Test-Path $f)) { return "no $f" }
    $t = Get-Content $f -Raw
    if ($t -notmatch "kInternalScanDepthGetCalled") { return "no split kInternalScanDepth (getCalled / print)" }
    if ($t -notmatch "isGlutilSource")             { return "no MinGW callback-frame fix (isGlutilSource)" }
    return ""
}
if ($GdmSourceDir) {
    $why = Test-GdmHasChanges $GdmSourceDir
    if ($why) { Fail "gdm at $GdmSourceDir is older than the stack-trace changes: $why." }
}

$OutRoot = Join-Path $Src "results\windows-gdm_$gdmId-$stamp"
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
Write-Host "toolchains: $(($tc.Values | ForEach-Object { $_.Label }) -join ', ')"
Write-Host "gdm: $(if ($GdmSourceDir) { "local checkout $GdmSourceDir" } else { 'gdm master, fetched into external\gdm' })"
Write-Host "results: $OutRoot"

$ConfigInfo = @{ deb = @{ BuildType = "Debug" }; rel = @{ BuildType = "RelWithDebInfo" } }
$summary = New-Object System.Collections.Generic.List[object]
$gdmChecked = [bool]$GdmSourceDir
Push-Location $Src
try {
    foreach ($tcName in $tc.Keys) {
        $t = $tc[$tcName]
        foreach ($cfg in $Configs) {
            $buildType = $ConfigInfo[$cfg].BuildType
            $opts = if ($cfg -eq "deb") { @("O0") } else { $t.RelOpts }
            foreach ($uw in $Unwinders) {
                if ($uw -notin $t.Unwinders) { continue }   # e.g. unwind (libgcc) with MSVC
                $combo = "$($t.Label)_${buildType}_$($opts -join '-')_unwind-$uw"
                $bdir = Join-Path $BuildRoot "build_gdm_bench_${tcName}_${buildType}_$uw"
                $cdir = Join-Path $OutRoot $combo
                $ldir = Join-Path $cdir "logs"
                New-Item -ItemType Directory -Force -Path $ldir | Out-Null
                $row = [ordered]@{ combination = $combo; status = "started"; cpptrace_unwinder = ""; build_dir = $bdir }
                $summary.Add($row)
                Write-Host "=== $combo  ($bdir)"

                # 1. configure
                $cfgArgs = @("-S", $Src, "-B", $bdir) + $t.Args +
                           @("-DCMAKE_TLS_VERIFY=OFF", "-DCMAKE_BUILD_TYPE=$buildType",
                             "-DSTACKTRACE_BENCH_OPTS=$($opts -join ';')",
                             "-DGDM_SOURCE_DIR=$GdmSourceDir")   # empty: fetch (also resets an older build dir)
                foreach ($u in $AllUnwinders) {
                    $cfgArgs += "-DCPPTRACE_UNWIND_WITH_${u}=" + $(if ($u -eq $uw.ToUpper()) { "ON" } else { "OFF" })
                }
                if ($Fresh) { $cfgArgs = @("--fresh") + $cfgArgs }
                $log = Join-Path $ldir "configure.txt"
                if ((Invoke-Logged $log "cmake" $cfgArgs) -ne 0) {
                    $row.status = "configure FAILED"; Write-Host "  configure FAILED ($log)"; Show-Tail $log; continue
                }
                if (-not $gdmChecked) {   # fetched master: check it once, before anything is measured
                    $why = Test-GdmHasChanges $ext
                    if ($why) {
                        $sha = if (Test-Path (Join-Path $ext "GDM_COMMIT")) { (Get-Content (Join-Path $ext "GDM_COMMIT") -Raw).Trim() } else { "?" }
                        $row.status = "gdm too old"
                        Fail ("fetched gdm master ($sha) is older than the stack-trace changes: $why.`n" +
                              "  Push the gdm changes, or copy a gdm checkout with them to external\gdm and run again.")
                    }
                    $gdmChecked = $true
                }

                # 2. the unwinder cpptrace is compiled with (Makefiles: flags.make, Visual Studio: .vcxproj)
                $defs = @(Get-ChildItem -Path $bdir -Recurse -Include "flags.make", "*.vcxproj" -ErrorAction SilentlyContinue |
                          Select-String -Pattern "CPPTRACE_UNWIND_WITH_([A-Z]+)" -AllMatches -CaseSensitive |
                          ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
                $row.cpptrace_unwinder = ($defs -join ",")
                if ($defs.Count -gt 0 -and ($defs.Count -ne 1 -or $defs[0] -ne $uw.ToUpper())) {
                    $row.status = "WRONG unwinder"; Write-Host "  cpptrace is configured with $($row.cpptrace_unwinder), not $uw - skipped"; continue
                }
                Write-Host "  cpptrace unwinder: $(if ($defs.Count) { $row.cpptrace_unwinder } else { '(define not found in the build files)' })"

                # 3. build (only the benchmark executables; --config for Visual Studio)
                $log = Join-Path $ldir "build.txt"
                Write-Host "  building ..."
                $targets = @($opts | ForEach-Object { "stacktrace_bench_$_"; "stacktrace_verify_real_$_" })
                if ((Invoke-Logged $log "cmake" (@("--build", $bdir, "--config", $buildType, "-j", "$Jobs", "--target") + $targets)) -ne 0) {
                    $row.status = "build FAILED"; Write-Host "  build FAILED ($log)"; Show-Tail $log; continue
                }

                # 4. verify + frame costs, per level
                $bad = $false
                foreach ($opt in $opts) {
                    $exe = Join-Path $bdir "test\stacktrace\stacktrace_bench_$opt.exe"
                    $log = Join-Path $cdir "verify-$opt.txt"
                    if ((Invoke-Logged $log $exe @("--verify", "-v")) -ne 0) {
                        $row.status = "verify $opt FAILED"; Write-Host "  verify $opt FAILED ($log)"
                        Select-String -Path $log -Pattern " FAIL" | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line)" }
                        $bad = $true; break
                    }
                    # the same intended frames with gdm's REAL debug_stacktrace.cpp
                    $real = Join-Path $bdir "test\stacktrace\stacktrace_verify_real_$opt.exe"
                    $rlog = Join-Path $cdir "verify-real-$opt.txt"
                    if ((Invoke-Logged $rlog $real @()) -ne 0) {
                        $row.status = "verify-real $opt FAILED"; Write-Host "  verify (real gdm code) $opt FAILED ($rlog)"
                        Select-String -Path $rlog -Pattern " FAIL" | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line)" }
                        $bad = $true; break
                    }
                    foreach ($case in @("C4", "C1")) {
                        $null = Invoke-Logged (Join-Path $cdir "frames-$opt-$case.txt") $exe @("--frames", $case)
                    }
                    Write-Host "  verify $opt OK"
                }
                if ($bad) { continue }

                # 5. benchmark
                $log = Join-Path $ldir "bench.txt"
                Write-Host "  benchmark ($Runs warm runs x $($opts.Count) levels, $ColdRuns cold runs) ..."
                $rc = Invoke-Logged $log $Python @("tools\run_stacktrace_bench.py", "--build", $bdir, "--out", $cdir,
                                                   "--opts", ($opts -join ","), "--runs", "$Runs", "--cold-runs", "$ColdRuns")
                if ($rc -ne 0) { $row.status = "bench FAILED"; Write-Host "  bench FAILED ($log)"; Show-Tail $log; continue }
                $row.status = "OK"
                Write-Host "  OK"
            }
        }
    }
}
finally {
    Pop-Location
    $sha = ""
    if ($gdmId -eq "master" -and (Test-Path (Join-Path $ext "GDM_COMMIT"))) {
        $sha = (Get-Content (Join-Path $ext "GDM_COMMIT") -Raw).Trim().Substring(0, 7)
        $env_lines.Add("gdm: master $sha (fetched into $ext)")
    } else {
        $env_lines.Add("gdm: $(if ($GdmSourceDir) { "local checkout $GdmSourceDir" } else { 'not resolved' })")
    }
    $env_lines.Add("build dirs: $BuildRoot\build_gdm_bench_<toolchain>_<BuildType>_<unwinder>")
    $env_lines.Add("runs: warm $Runs, cold $ColdRuns")
    $env_lines | Out-File -FilePath (Join-Path $OutRoot "environment.txt") -Encoding utf8
    $summary | ForEach-Object { [pscustomobject]$_ } | Format-Table -AutoSize | Out-String -Width 300 |
        Tee-Object -FilePath (Join-Path $OutRoot "summary.txt") | Write-Host
    & $Python -I (Join-Path $Src "tools\compare_results.py") $OutRoot 2>&1 | Out-Null
    if ($sha) {
        $final = $OutRoot -replace "gdm_master-", "gdm_master-$sha-"
        Rename-Item -Path $OutRoot -NewName (Split-Path -Leaf $final) -ErrorAction SilentlyContinue
        if ($?) { $OutRoot = $final }
    }
    Write-Host "results: $OutRoot  (open comparison.md first)"
}
