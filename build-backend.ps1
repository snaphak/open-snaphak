# build-backend.ps1 -- build ONLY the backend (XINPUT1_3.dll). A thin wrapper around
# src\backend\build.ps1 (which works fine standalone -- this exists purely so all three top-level build
# scripts are peers at the repo root and equally discoverable). Pure ASCII.
#
# One of two top-level build scripts -- build-backend.ps1 (this one) and build.ps1 (backend + frontend).
# Forwards any extra args (e.g. -Diag) straight through to src\backend\build.ps1.
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\backend\build.ps1" @Rest
if ($LASTEXITCODE -ne 0) { throw "backend build failed" }
Write-Host "built: build/XINPUT1_3.dll + build/XINPUT1_4.dll"
