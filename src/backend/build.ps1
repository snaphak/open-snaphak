# build.ps1 -- compile the SnapHak BACKEND DLL (our clean-room XINPUT1_3.dll) with MSVC (x64).
# Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252). Reuses the fault-shield build TEMPLATE; the backend
# is a DISTINCT DLL (separate dir, own DllMain) -- this is only the shared MSVC/proxy build approach.
#
# Usage:
#   pwsh -File build.ps1                 # -> XINPUT1_3.dll (the resident backend, proxy + foundation)
#   pwsh -File build.ps1 -Out x.dll      # alternate output name
#
# Sources (each is a clone of an OG XINPUT1_3 / engine mechanism):
# dllmain (bootstrap), signatures (the masked-byte engine resolver), hook (the inline-detour installer),
# smoke (the resolver + installer self-proof), backend_log, xinput_proxy (the XInput export-forwarding
# thunks -- DOOM's input keeps working).
# rawmap (the keystone rawmap LOAD swap, port of OG
# FUN_180023ad0's DeserializeFromJson detour, plus the SAVE shadow -- the inverse, port of OG
# FUN_180023e60's SerializeToJson detour: on every editor save, call the engine serialize then mirror the
# out-idStr JSON to rawmap.json); strids (the #str_ string injector, port of OG FUN_1800102e0/FUN_18000FF10
# -- a detour on the engine idLangDict sort that appends strings/strids.json rows to the live string table);
# overrides (the OVERRIDES FILE-SHADOW, port of OG FUN_18000b370 -- a VTABLE-SLOT swap of the engine
# resource-provider's open-by-name method, serving %USERPROFILE%\snaphak\overrides\<name> from disk).
# cvars (register the 9 cvars via the engine OUTER cvar register 0x1A04F00); commands (register the 22
# console commands via the engine AddCommand 0x1AA3630, cmdSystem global decoded from the CmdSystemLea
# accessor; the trivial handlers wire snapHak_rawmaps_on/off to the shipped ops -- ports of
# OG FUN_1800229b1's install spine). clipboard (CF_TEXT clipboard-set/get, port of OG FUN_1800053f0) feeds
# the sh_listres clipboard copy; sh_listres (port of FUN_180022000, GetDeclsOfType decl walk) + sh_entlist
# (port of FUN_180021b50, vendored class list in entlist_classes.h) are real handlers in commands. entity
# (sh_dumpdef / sh_spawninfo / sh_spawn -- ports of OG FUN_180021e60 / FUN_180024d90 / FUN_180021c90;
# gameMgr global decoded via the GameMgrLea sig reusing commands' sh_decode_rip_slot; FindEntity/GetOrigin/
# ExecuteCommandText vtable slots SEH-guarded; sh_spawn's teleport guarded against a bogus GetOrigin).
# typeinfo (cs_fieldinfo / sh_type -- ports of OG FUN_180021db0 / FUN_180021090; the reflection/type-info
# mgr reached via the hardcoded declMgr accessor RVA 0x17F7030 (NOT sig-able) + vtable+0x80, then
# FindTypeInfoByName/FindEnumByName sigs; field+enum record walks SEH-guarded + capped).
# patch (the reusable engine-code PATCH/DETOUR layer -- code_patch/code_unpatch = OG FUN_180001790
# memcpy-to-RX with a sig-anchored verify-before-write + a restore-record, all SEH-guarded; the detour
# family is a thin REUSE of hook.c's installer). Runs an in-DLL scratch-site self-test at install (apply/
# call-through/restore + the negative refuse-on-mismatch); installs NO engine patches itself.
# algo (cs_dontuse [18] + sh_alginfo -- ports of OG XINPUT1_3 FUN_1800223a0 + the 4 vendored snaphak_algo
# math overrides). cs_dontuse is a TOGGLE that FULL-replaces 4 engine math fns (matmul 0x1a82f10 / inverse
# 0x1a828f0 / packRGBA 0x1a19470 / curveEval 0x1a5eb40, sig-resolved AlgoMatMul/AlgoInverse/AlgoPackRGBA/
# AlgoCurveEval) with clean-room reimpls -- matmul/inverse/curveEval in f64 (more precise than the engine's
# native f32, satisfying OG's contract), color-pack BIT-EXACT to the OG round-half-up hook. OFF BY DEFAULT
# (the 2nd sanctioned divergence after the fault-shield).
# Runs an in-DLL math self-test at install (the 4 ops on known inputs, NO engine state). Replaces the
# cs_dontuse + sh_alginfo cosmetic stubs in commands.
# Add new backend sources to the $Sources list below.
#
# Needs Build Tools for Visual Studio 2022 (C++ workload).
param(
    [string[]]$Sources = @("dllmain.c", "signatures.c", "hook.c", "smoke.c",
                           "rawmap.c", "strids.c",
                           "overrides.c", "cvars.c", "commands.c", "clipboard.c",
                           "entity.c", "typeinfo.c", "patch.c", "algo.c", "target_any.c", "wiring_cleandirect.c", "ui_bridge.c",
                           "iface_engine.c", "apply_engine.c", "../common/snaphak_iface.c",
                           # cvar-unlock MERGED in: the former standalone dinput8 cvar-unlock now rides
                           # the backend (one fewer shipped DLL; no System32 dinput8 shadow). dinput8 forwarder
                           # dropped -- DOOM loads the real System32 dinput8. Spawned from dllmain (b2_cvar_unlock_start).
                           "cvar_unlock.c",
                           "backend_log.c", "xinput_proxy.c",
                           # FAULT-SHIELD (merged 2026-06-22): the recover-in-place shield rides the backend's
                           # proven XINPUT1_3 load. Reuses THIS dir's hook.c + signatures.c (no double-link --
                           # the shield's hook.c/signatures.c are NOT added). Installed from dllmain bootstrap.
                           "../fault_shield/veh.c", "../fault_shield/recovery.c",
                           "../fault_shield/fault_record.c", "../fault_shield/shield_sigs.c",
                           "../fault_shield/fault_shield.c"),
    [string]$Out = "XINPUT1_3.dll",
    # -Diag: build the DIAGNOSTIC variant -- adds the catch-all crash + environment logger (shield_diag.c)
    # under /DSNAPHAK_DIAG. Same output name (XINPUT1_3.dll) so an end-user just swaps it in, reproduces the
    # crash, and sends snaphak_diag.log. A TROUBLESHOOTING build only -- not for distribution.
    [switch]$Diag
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Allow comma-separated -Sources (handy from the shell): "a.c,b.c" -> @("a.c","b.c").
if ($Sources.Count -eq 1 -and $Sources[0] -match ",") { $Sources = $Sources[0].Split(",") }

# -Diag: pull in the diagnostic crash + environment logger and define SNAPHAK_DIAG (dllmain arms it).
$defs = ""
if ($Diag) {
    $Sources += "../fault_shield/shield_diag.c"
    $defs = "/DSNAPHAK_DIAG"
    Write-Host "[build] DIAGNOSTIC variant: +shield_diag.c /DSNAPHAK_DIAG"
}

# Compile with cwd = $here (relative names) so quoted absolute paths with trailing backslashes can't be
# mis-parsed by cmd. The XInput export NAMES *and ORDINALS* come from xinput1_3.def (/DEF: below); the
# xinput_proxy.c bodies are plain (no __declspec) so the .def is the SOLE export source. This is load-
# bearing: DOOM imports XINPUT1_3.dll BY ORDINAL, and __declspec auto-numbers exports ALPHABETICALLY ->
# the wrong ordinals -> DOOM's controller poll calls the wrong fn -> heap corruption. The .def pins the
# ordinals to the real System32 XInput1_3.dll. (.def EXPORTS of locally-DEFINED fns are not dotted
# forwarders, so there is no alias/collision -- the old reason for avoiding a .def no longer applies.)
$srcArgs = ($Sources | ForEach-Object { '"' + $_.Trim() + '"' }) -join " "
$implib  = $Out -replace '\.dll$', '.lib'   # import lib + .exp -> build\obj\backend (build\ root stays shippable DLLs only)
# /I..\common : the shared UI-interface ABI header (snaphak_iface.h) the ui-bridge + the common factory
# (../common/snaphak_iface.c) include. The interface object the backend creates here is the matched pair
# the frontend snaphakui.dll consumes.
# shell32.lib: SHGetFolderPathA -- the prefab path resolver (+0xc0, OG FUN_18000ce50).
# Output goes to the TOP-LEVEL open-snaphak\build\ (out of src\), via paths RELATIVE to cwd=$here so the
# quoted-trailing-backslash cmd footgun (see above) is avoided: ..\..\ from src\backend\ is the repo root.
# /DEF:xinput1_3.def + /I..\common stay cwd-relative (load-bearing -- the .def pins the XInput ordinals).
$cl = "cl /nologo /LD /O2 /W3 /MT $defs /Fo..\..\build\obj\backend\ /I..\common $srcArgs /Fe:..\..\build\$Out /link /DEF:xinput1_3.def /IMPLIB:..\..\build\obj\backend\$implib shell32.lib"

$cmd = "cd /d `"$here`" && `"$vcvars`" && $cl"
# vcvars64.bat emits a spurious "'vswhere.exe' is not recognized" line on stderr (it probes a bare-PATH
# vswhere before falling back); under $ErrorActionPreference='Stop' that native-command stderr line trips
# PS 5.1 as a terminating error even though cl succeeds. Route the whole cmd's stdout+stderr to a log and
# gate ONLY on the real signal -- $LASTEXITCODE from `cmd /c` (the same pattern the frontend build.ps1 uses).
$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"   # open-snaphak\build (out of src\)
New-Item -ItemType Directory -Force (Join-Path $outDir "obj\backend") | Out-Null
$buildLog = Join-Path $outDir "build.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
Write-Host "built $(Join-Path $outDir $Out)"
if ($Diag) {
    Write-Host "[build] *** DIAGNOSTIC build -- DO NOT DISTRIBUTE (troubleshooting only; writes snaphak_diag.log + snaphak_crash.dmp) ***"
} else {
    # a -Diag build emits shield_diag.obj into build\obj\backend; keep the release obj dir diag-free.
    Remove-Item (Join-Path $outDir "obj\backend\shield_diag.obj") -ErrorAction SilentlyContinue
}
