# run-tests.ps1 -- compile + run SnapHak's C unit tests with MSVC (x64). Pure ASCII.
# Needs Build Tools for Visual Studio 2022 (C++ workload) -- the same toolchain the build scripts use.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1 -Doom C:\path\to\unpacked-DOOMx64vk.exe
#
# Default: the self-contained tests (no game, no built DLL needed):
#   shield_format_test  -- the fault record string formatter (pure logic)
#   hook_test           -- the inline-detour installer, on a hand-laid scratch stub
#   crash_record_test   -- the crash-record JSON formatter + escaping (pure logic)
#   report_scrub_test   -- the crash-report log anonymization scrub + tail (pure logic)
# -Doom <unpacked DOOMx64vk.exe>: ALSO the signature-resolver tests, which scan a real
#   (Steamless-unpacked) DOOM image:
#   sig_test            -- every engine signature resolves to its known RVA
#   hooktol_test        -- the resolver's hook-tolerant fallback (prologue-clobbered fns)
#
# Exit 0 iff every selected test passes; non-zero (with the build log) on any failure.
# Objects + test exes land in tests\obj\ (gitignored). The runtime XInput-ordinal test
# (xinput_ordinal_test) is run by hand against a built build\XINPUT1_3.dll -- see docs\contributing.md.
param([string]$Doom = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$obj  = Join-Path $here "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# name | sources (relative to tests\) | runtime arg
$tests = @(
    @{ name = "shield_format_test"; src = 'shield_format_test.c ..\src\fault_shield\fault_record.c'; arg = "" }
    @{ name = "hook_test";          src = 'hook_test.c ..\src\backend\hook.c';                       arg = "" }
    @{ name = "crash_record_test";  src = 'crash_record_test.c ..\src\fault_shield\crash_record_format.c'; arg = "" }
    @{ name = "report_scrub_test";  src = 'report_scrub_test.c';                                     arg = "" }
)
if ($Doom) {
    if (-not (Test-Path $Doom)) { throw "-Doom path not found: $Doom" }
    $da = (Resolve-Path $Doom).Path
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c';     arg = $da }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c'; arg = $da }
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $obj ($t.name + ".exe")
    # Output paths are RELATIVE (cwd=tests via cd /d) -- a quoted absolute path with a trailing backslash
    # is the cmd `\"` footgun the build scripts document (cl D8036). obj\ exists (created above); names have no spaces.
    $cl  = "cl /nologo /O2 /MT /I..\src\backend /I..\src\fault_shield $($t.src) /Fe:obj\$($t.name).exe /Foobj\"
    $log = Join-Path $obj ($t.name + ".build.log")
    # vcvars64.bat prints a spurious 'vswhere not recognized' line to stderr; gate on cl's real exit only
    # (the same cmd /c pattern the build scripts use) instead of letting that stderr trip $ErrorActionPreference.
    cmd /c "cd /d `"$here`" && `"$vcvars`" && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log | Write-Host; Write-Host "[FAIL] compile $($t.name)"; $fail++; continue }
    if ($t.arg) { & $exe $t.arg } else { & $exe }
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
    else { Write-Host "[ok]   $($t.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail C test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all C tests passed ($($tests.Count))"
