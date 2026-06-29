# build.ps1 -- build BOTH SnapHak clone DLLs (backend XINPUT1_3.dll, then the Qt frontend
# snaphakui.dll). Pure ASCII. Needs MSVC 2022 Build Tools + Qt 5.9.9 (see README).
# -QtDir forwards to the frontend build (CI installs Qt to a non-default path).
param([string]$QtDir = "C:\Qt\5.9.9\msvc2017_64")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\backend\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "backend build failed" }
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\ui\build.ps1" -QtDir $QtDir
if ($LASTEXITCODE -ne 0) { throw "frontend build failed" }
Write-Host "built: build/XINPUT1_3.dll + build/snaphakui.dll"
