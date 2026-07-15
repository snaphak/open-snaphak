# build.ps1 -- build BOTH the backend (XINPUT1_3.dll) and the WebView2 (HTML) frontend (snaphakui.dll)
# together: backend first, frontend second, always in lockstep. Pure ASCII.
#
# Why this exists: the backend and whichever frontend you run share ONE ABI header
# (src\common\snaphak_iface.h). Every vtable extension the webview frontend has picked up over time
# (apply_class_inherit, enum_valid_classes/enum_inherits, id_dev_layer_hidden, and now push_to_stack for
# the backend-hosted SnapStack port) must be resolved by a MATCHING backend build -- an old backend paired
# with a freshly-built frontend that reads a newer vtable slot is undefined behavior (the frontend reads
# past the end of the backend's smaller vtable struct). Running src\ui\build.ps1 alone NEVER
# rebuilds the backend, so it's easy to end up mismatched if you only touch the frontend side. This script
# removes that footgun: one command, backend first, frontend second, always in lockstep.
#
# Needs: Build Tools for Visual Studio 2022 (C++ workload). No Qt required for this path.
#
# The two top-level build scripts: build-backend.ps1 (backend only) and this one (backend + frontend).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\backend\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "backend build failed" }
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\ui\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "frontend build failed" }
Write-Host "built: build/XINPUT1_3.dll + build/XINPUT1_4.dll + build/webview/snaphakui.dll"
Write-Host "package with: package.ps1"
