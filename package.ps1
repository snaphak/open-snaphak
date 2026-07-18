# package.ps1 -- assemble the deployable SnapHak overlay into dist\ (the tree you drop into a DOOM
# install). Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252).
#
# Ships ONLY the two clone DLLs -- the WebView2 (HTML) frontend has no Qt dependency and uses the
# system-installed WebView2 runtime (preinstalled on Windows 11; evergreen on most Windows 10) at run
# time, so there is nothing else to bundle.
#
# Consumes the DLLs built by src\backend\build.ps1 (XINPUT1_3.dll) and src\ui\build.ps1 (snaphakui.dll),
# both in build\.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # open-snaphak\
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# --- consume build\ : both clone DLLs must be present. The frontend lands in build\webview\snaphakui.dll,
#     the backend directly in build\XINPUT1_3.dll. ---
$backendDll = Join-Path $build "XINPUT1_3.dll"
$uiDll      = Join-Path $build "webview\snaphakui.dll"
foreach ($d in @($backendDll, $uiDll)) {
    if (-not (Test-Path $d)) { throw "missing $d -- run build.ps1 first (repo root; builds backend + frontend together)." }
}

# --- refuse a -Diag (troubleshooting) backend: it is self-labelled DO NOT DISTRIBUTE. The diagnostic
#     logger (shield_diag.c) is the only source of the string "snaphak_diag.log"; a release build has
#     none. (The old marker, "snaphak_crash.dmp", stopped being diag-unique when the release fatal
#     path gained its own crash-dump write for the crash-report dialog.) ---
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($backendDll))
if ($ascii.Contains("snaphak_diag.log")) {
    throw "build\XINPUT1_3.dll is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with src\backend\build.ps1 (no -Diag)."
}

# --- assemble dist\ fresh ---
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$snapDir = Join-Path $dist "snaphak"
New-Item -ItemType Directory -Force $snapDir | Out-Null

Copy-Item $backendDll (Join-Path $dist "XINPUT1_3.dll")
Copy-Item $uiDll      (Join-Path $snapDir "snaphakui.dll")

# --- MANIFEST.sha256 : the 2 deployable files (the installer's file list + per-file hash verify) ---
$files = @("XINPUT1_3.dll", "snaphak\snaphakui.dll")
$lines = foreach ($f in $files) {
    $h = (Get-FileHash (Join-Path $dist $f) -Algorithm SHA256).Hash
    "{0}  {1}" -f $h, $f
}
[System.IO.File]::WriteAllLines((Join-Path $dist "MANIFEST.sha256"), $lines, (New-Object System.Text.UTF8Encoding $false))

Write-Host "packaged $($files.Count) files into $dist :"
foreach ($f in $files) {
    Write-Host ("  {0,-24} {1,10}" -f $f, (Get-Item (Join-Path $dist $f)).Length)
}
Write-Host "MANIFEST.sha256 written (the install/verify map)."
