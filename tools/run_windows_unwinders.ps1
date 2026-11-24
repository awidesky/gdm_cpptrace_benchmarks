<#
.SYNOPSIS
  Windows (MinGW GCC): build and run the whole stacktrace benchmark for every cpptrace unwinder,
  in Debug (O0) and RelWithDebInfo (O1 O2 O3 Os). One command, from the gdm_benchmarks folder:

    powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1

  or, in an open PowerShell window (allows scripts for this window only):

    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force; .\tools\run_windows_unwinders.ps1

  Takes about 1 hour. Needs no Visual Studio (see REQUIREMENTS).
  Help: Get-Help .\tools\run_windows_unwinders.ps1 -Full

.DESCRIPTION
  Combinations: 3 unwinders x 2 build configs = 6, each in its own build dir
  <BuildRoot>\build_gdm_bench_<BuildType>_<unwinder>:
    unwinder (cpptrace CPPTRACE_UNWIND_WITH_*)       build config
      dbghelp  dbghelp StackWalk64 (cpptrace's        Debug           O0
               default on Windows)                   RelWithDebInfo  O1 O2 O3 Os
      winapi   RtlCaptureStackBackTrace
      unwind   libgcc _Unwind_Backtrace
  Per combination:
    1. configure (only that unwinder ON, the others OFF)
    2. check the unwinder cpptrace is really compiled with (its CPPTRACE_UNWIND_WITH_* define)
    3. build
    4. per level: --verify (all cases C1..C7b, old vs new vs cache) and --frames C4 / C1
       (cost of capturing n frames, and of resolving each frame)
    5. the benchmark (warm and cold, old / new / new K=6 / cache hit / cache miss)
  A failing step skips the rest of that combination; the others still run.

  gdm under test:
    - external\gdm without a GDM_COMMIT file (a checkout copied there by hand, e.g. with
      unpushed changes) is used as is (-DGDM_SOURCE_DIR), so configuring cannot replace it;
    - otherwise gdm master is fetched into external\gdm (gdm_benchmarks' default);
    - -GdmSourceDir <dir> uses that checkout.
    Before anything is measured, the script stops if that gdm lacks the stack-trace changes
    the benchmark is about (split kInternalScanDepth, the MinGW callback-frame fix).

  RESULTS (one folder per run of this script; the names say what was run):
    results\windows-gcc<version>-gdm_<local|master-sha>-<yyyyMMdd-HHmmss>\
      comparison.md                     every combination side by side (start here)
      summary.txt                       status of each combination
      environment.txt                   tool versions, gdm used, build dirs
      Debug_O0_unwind-dbghelp\          summary.md (full tables), warm.tsv, cold.tsv,
      Debug_O0_unwind-winapi\           verify.txt, verify-<opt>.txt, frames-<opt>-C4.txt,
      ...                               frames-<opt>-C1.txt, logs\ (configure, build, bench)
      RelWithDebInfo_O1-O2-O3-Os_unwind-unwind\

  REQUIREMENTS (no Visual Studio needed; dbghelp.dll ships with Windows):
    CMake >= 3.24, MinGW-w64 GCC with mingw32-make (default c:\mingw64\bin), git (cpptrace
    clones libdwarf at configure time), Python 3, and network access to GitHub. The script
    checks them first and stops if one is missing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1 -Unwinders winapi -Configs rel -Runs 1 -ColdRuns 1
  (quick check of one combination, a few minutes)
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_windows_unwinders.ps1 -CC D:\mingw64\bin\gcc.exe -CXX D:\mingw64\bin\g++.exe
#>
param(
    [string]   $CC           = "c:\mingw64\bin\gcc.exe",
    [string]   $CXX          = "c:\mingw64\bin\g++.exe",
    [string[]] $Unwinders    = @("dbghelp", "winapi", "unwind"),
    [string[]] $Configs      = @("deb", "rel"),
    [int]      $Runs         = 7,    # driver --runs      (warm processes per level)
    [int]      $ColdRuns     = 15,   # driver --cold-runs (first-call processes per case)
    [int]      $Jobs         = [Environment]::ProcessorCount,
    [string]   $Python       = "python",
    [string]   $GdmSourceDir = "",   # a gdm checkout to test (default: see DESCRIPTION)
    [string]   $BuildRoot    = "",   # where the build dirs go (default: next to gdm_benchmarks)
    [switch]   $Fresh                # cmake --fresh: recreate the caches of existing build dirs
)

$ErrorActionPreference = "Continue"   # native tools write to stderr; exit codes decide
# "powershell -File" passes "-Unwinders winapi,unwind" as ONE string; split lists either way
$Unwinders = @($Unwinders | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ })
$Configs   = @($Configs   | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ })
foreach ($u in $Unwinders) { if ($u -notin @("dbghelp", "winapi", "unwind")) { Write-Host "ERROR: unknown unwinder '$u' (dbghelp, winapi, unwind)" -ForegroundColor Red; exit 1 } }
foreach ($c in $Configs)   { if ($c -notin @("deb", "rel"))                  { Write-Host "ERROR: unknown config '$c' (deb, rel)" -ForegroundColor Red; exit 1 } }
$Src = Split-Path -Parent $PSScriptRoot             # gdm_benchmarks
if (-not $BuildRoot) { $BuildRoot = Split-Path -Parent $Src }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

$AllUnwinders = @("DBGHELP", "WINAPI", "UNWIND", "LIBUNWIND", "EXECINFO", "NOTHING")
$ConfigInfo = @{
    deb = @{ BuildType = "Debug";          Opts = @("O0") }
    rel = @{ BuildType = "RelWithDebInfo"; Opts = @("O1", "O2", "O3", "Os") }
}

function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

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

# ---- 0. tools (no Visual Studio involved: MinGW's own toolchain and runtime) ----------------
$env_lines = New-Object System.Collections.Generic.List[string]
foreach ($t in @(@("CMake", "cmake", "--version"), @("git", "git", "--version"), @("Python", $Python, "--version"))) {
    if (-not (Get-Command $t[1] -ErrorAction SilentlyContinue)) { Fail "$($t[0]) not found ($($t[1])). Install it and put it on PATH." }
    $env_lines.Add("$($t[0]): $(First-Line $t[1] @($t[2]))")
}
# "python" can be the Microsoft Store alias, which only opens the Store
if ((First-Line $Python @("--version")) -notmatch "Python 3") { Fail "'$Python --version' is not Python 3 (Store alias? pass -Python <path to python.exe>)." }
$cmv = [regex]::Match((First-Line "cmake" @("--version")), "(\d+)\.(\d+)")
if ($cmv.Success -and ([int]$cmv.Groups[1].Value * 100 + [int]$cmv.Groups[2].Value) -lt 324) { Fail "CMake >= 3.24 is required (gdm)." }
foreach ($c in @($CC, $CXX)) { if (-not (Test-Path $c)) { Fail "compiler not found: $c (pass -CC / -CXX)" } }
$gccVer = First-Line $CXX @("-dumpfullversion")
$make = Join-Path (Split-Path -Parent $CXX) "mingw32-make.exe"
if (-not (Test-Path $make)) {
    $m = Get-Command "mingw32-make" -ErrorAction SilentlyContinue
    if (-not $m) { Fail "mingw32-make not found next to $CXX or on PATH." }
    $make = $m.Source
}
$env_lines.Add("compiler: $CXX (GCC $gccVer)")
$env_lines.Add("make: $make")

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

$OutRoot = Join-Path $Src "results\windows-gcc$gccVer-gdm_$gdmId-$stamp"
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
Write-Host "gdm: $(if ($GdmSourceDir) { "local checkout $GdmSourceDir" } else { 'gdm master, fetched into external\gdm' })"
Write-Host "results: $OutRoot"

$summary = New-Object System.Collections.Generic.List[object]
$gdmChecked = [bool]$GdmSourceDir
Push-Location $Src
try {
    foreach ($cfg in $Configs) {
        foreach ($uw in $Unwinders) {
            $info = $ConfigInfo[$cfg]
            if (-not $info) { Write-Host "unknown config '$cfg' (deb or rel)"; continue }
            $combo = "$($info.BuildType)_$($info.Opts -join '-')_unwind-$uw"
            $bdir = Join-Path $BuildRoot "build_gdm_bench_$($info.BuildType)_$uw"
            $cdir = Join-Path $OutRoot $combo
            $ldir = Join-Path $cdir "logs"
            New-Item -ItemType Directory -Force -Path $ldir | Out-Null
            $row = [ordered]@{ combination = $combo; status = "started"; cpptrace_unwinder = ""; build_dir = $bdir }
            $summary.Add($row)
            Write-Host "=== $combo  ($bdir)"

            # 1. configure
            $cfgArgs = @("-S", $Src, "-B", $bdir, "-G", "MinGW Makefiles", "-DCMAKE_TLS_VERIFY=OFF",
                         "-DCMAKE_C_COMPILER=$CC", "-DCMAKE_CXX_COMPILER=$CXX", "-DCMAKE_MAKE_PROGRAM=$make",
                         "-DCMAKE_BUILD_TYPE=$($info.BuildType)", "-DSTACKTRACE_BENCH_OPTS=$($info.Opts -join ';')",
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

            # 2. the unwinder cpptrace is compiled with
            $defs = @(Get-ChildItem -Path $bdir -Recurse -Filter "flags.make" -ErrorAction SilentlyContinue |
                      Select-String -Pattern "CPPTRACE_UNWIND_WITH_([A-Z]+)" -AllMatches -CaseSensitive |
                      ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
            $row.cpptrace_unwinder = ($defs -join ",")
            if ($defs.Count -gt 0 -and ($defs.Count -ne 1 -or $defs[0] -ne $uw.ToUpper())) {
                $row.status = "WRONG unwinder"; Write-Host "  cpptrace is configured with $($row.cpptrace_unwinder), not $uw - skipped"; continue
            }
            Write-Host "  cpptrace unwinder: $(if ($defs.Count) { $row.cpptrace_unwinder } else { '(define not found in flags.make)' })"

            # 3. build
            $log = Join-Path $ldir "build.txt"
            Write-Host "  building ..."
            if ((Invoke-Logged $log "cmake" @("--build", $bdir, "-j", "$Jobs")) -ne 0) {
                $row.status = "build FAILED"; Write-Host "  build FAILED ($log)"; Show-Tail $log; continue
            }

            # 4. verify + frame costs, per level
            $bad = $false
            foreach ($opt in $info.Opts) {
                $exe = Join-Path $bdir "test\stacktrace\stacktrace_bench_$opt.exe"
                $log = Join-Path $cdir "verify-$opt.txt"
                if ((Invoke-Logged $log $exe @("--verify", "-v")) -ne 0) {
                    $row.status = "verify $opt FAILED"; Write-Host "  verify $opt FAILED ($log)"
                    Select-String -Path $log -Pattern " FAIL" | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line)" }
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
            Write-Host "  benchmark ($Runs warm runs x $($info.Opts.Count) levels, $ColdRuns cold runs) ..."
            $rc = Invoke-Logged $log $Python @("tools\run_stacktrace_bench.py", "--build", $bdir, "--out", $cdir,
                                               "--opts", ($info.Opts -join ","), "--runs", "$Runs", "--cold-runs", "$ColdRuns")
            if ($rc -ne 0) { $row.status = "bench FAILED"; Write-Host "  bench FAILED ($log)"; Show-Tail $log; continue }
            $row.status = "OK"
            Write-Host "  OK"
        }
    }
}
finally {
    Pop-Location
    # the fetched gdm's commit goes into the results folder name
    if ($gdmId -eq "master" -and (Test-Path (Join-Path $ext "GDM_COMMIT"))) {
        $sha = (Get-Content (Join-Path $ext "GDM_COMMIT") -Raw).Trim().Substring(0, 7)
        $env_lines.Add("gdm: master $sha (fetched into $ext)")
    } else {
        $env_lines.Add("gdm: $(if ($GdmSourceDir) { "local checkout $GdmSourceDir" } else { 'not resolved' })")
    }
    $env_lines.Add("build dirs: $BuildRoot\build_gdm_bench_<BuildType>_<unwinder>")
    $env_lines.Add("runs: warm $Runs, cold $ColdRuns")
    $env_lines | Out-File -FilePath (Join-Path $OutRoot "environment.txt") -Encoding utf8
    $summary | ForEach-Object { [pscustomobject]$_ } | Format-Table -AutoSize | Out-String -Width 300 |
        Tee-Object -FilePath (Join-Path $OutRoot "summary.txt") | Write-Host
    & $Python -I (Join-Path $Src "tools\compare_results.py") $OutRoot 2>&1 | Out-Null
    if ($gdmId -eq "master" -and $sha) {
        $final = $OutRoot -replace "gdm_master-", "gdm_master-$sha-"
        Rename-Item -Path $OutRoot -NewName (Split-Path -Leaf $final) -ErrorAction SilentlyContinue
        if ($?) { $OutRoot = $final }
    }
    Write-Host "results: $OutRoot  (open comparison.md first)"
}
