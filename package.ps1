# package.ps1 -- assemble the deployable SnapHak overlay into dist\ (the tree you drop into a DOOM
# install). Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252).
#
# Ships ONLY the three clone DLLs -- the WebView2 (HTML) frontend has no Qt dependency and uses the
# system-installed WebView2 runtime (preinstalled on Windows 11; evergreen on most Windows 10) at run
# time, so there is nothing else to bundle.
#
# Consumes the DLLs built by src\backend\build.ps1 (XINPUT1_3.dll + XINPUT1_4.dll -- one package
# serving both the pre- and post-April-2024 DOOM builds) and src\ui\build.ps1 (snaphakui.dll), all in
# build\.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # open-snaphak\
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# --- consume build\ : all clone DLLs must be present. The frontend lands in build\webview\snaphakui.dll;
#     the backend ships as TWO differently-named DLLs (XINPUT1_3.dll + XINPUT1_4.dll -- see
#     xinput1_4.def) so one package serves both the pre- and post-April-2024 DOOM builds. Each DOOM
#     exe only ever loads the proxy whose name matches what it actually imports, so shipping both is
#     harmless -- no version detection needed. ---
$backendDll3 = Join-Path $build "XINPUT1_3.dll"
$backendDll4 = Join-Path $build "XINPUT1_4.dll"
$uiDll       = Join-Path $build "webview\snaphakui.dll"
foreach ($d in @($backendDll3, $backendDll4, $uiDll)) {
    if (-not (Test-Path $d)) { throw "missing $d -- run build.ps1 first (repo root; builds backend + frontend together)." }
}

# --- refuse a -Diag (troubleshooting) backend: it is self-labelled DO NOT DISTRIBUTE. The diagnostic
#     logger (shield_diag.c) is the only source of the string "snaphak_crash.dmp"; a release build has none. ---
foreach ($d in @($backendDll3, $backendDll4)) {
    $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($d))
    if ($ascii.Contains("snaphak_crash.dmp")) {
        throw "$d is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with src\backend\build.ps1 (no -Diag)."
    }
}

# --- assemble dist\ fresh ---
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$snapDir = Join-Path $dist "snaphak"
New-Item -ItemType Directory -Force $snapDir | Out-Null

Copy-Item $backendDll3 (Join-Path $dist "XINPUT1_3.dll")
Copy-Item $backendDll4 (Join-Path $dist "XINPUT1_4.dll")
Copy-Item $uiDll       (Join-Path $snapDir "snaphakui.dll")

# --- MANIFEST.sha256 : the deployable files (the installer's file list + per-file hash verify) ---
$files = @("XINPUT1_3.dll", "XINPUT1_4.dll", "snaphak\snaphakui.dll")
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
