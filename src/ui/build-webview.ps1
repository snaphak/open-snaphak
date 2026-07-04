# build-webview.ps1 -- PROOF-OF-CONCEPT: build a Qt-FREE snaphakui.dll that hosts the SnapHak Studio UI
# in a Microsoft Edge WebView2 control (HTML/CSS/JS) instead of Qt. Drop-in replacement for the Qt
# snaphakui.dll (same exports, same backend contract). Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252).
#
# What it does:
#   1. locate MSVC via vswhere -> vcvars64 (same pattern as build.ps1).
#   2. fetch the Microsoft.Web.WebView2 SDK (headers + static loader lib) from NuGet into
#      build\webview2sdk (gitignored) if not already there. NO binaries land in the repo.
#   3. generate build\obj\uiwv\mockup_html.h from webview\mockup.html (the UI, embedded in the DLL).
#   4. cl-compile webview\snaphak_ui_webview.cpp + sl_exports.cpp -> build\snaphakui.dll, statically
#      linking WebView2LoaderStatic.lib (no WebView2Loader.dll to ship) and NO Qt.
#
# Usage:  pwsh -NoProfile -ExecutionPolicy Bypass -File build-webview.ps1
#
# Needs: Build Tools for Visual Studio 2022 (C++ workload). Uses the system-installed WebView2 runtime
# at RUN time (preinstalled on Windows 11; evergreen runtime on most Windows 10).
param(
    [string]$Out = "snaphakui.dll"
)
$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path            # src\ui
$repo   = Split-Path -Parent (Split-Path -Parent $here)             # repo root
$common = Join-Path (Split-Path -Parent $here) "common"            # src\common
$build  = Join-Path $repo "build"
$objDir = Join-Path $build "obj\uiwv"
$sdkDir = Join-Path $build "webview2sdk"
New-Item -ItemType Directory -Force $objDir | Out-Null

# --- 1. MSVC toolchain --------------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install VS 2022 Build Tools (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# --- 2. WebView2 SDK (NuGet) --------------------------------------------------------------------------
$wvInclude = Join-Path $sdkDir "build\native\include"
$wvLib     = Join-Path $sdkDir "build\native\x64\WebView2LoaderStatic.lib"
if (-not (Test-Path (Join-Path $wvInclude "WebView2.h"))) {
    Write-Host "Fetching Microsoft.Web.WebView2 SDK from NuGet..."
    $idx = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json"
    $ver = $idx.versions[-1]
    Write-Host "  latest version: $ver"
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg"
    $zip = Join-Path $build "webview2.$ver.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip
    if (Test-Path $sdkDir) { Remove-Item -Recurse -Force $sdkDir }
    Expand-Archive -Path $zip -DestinationPath $sdkDir -Force
    Remove-Item $zip -Force
}
if (-not (Test-Path (Join-Path $wvInclude "WebView2.h"))) { throw "WebView2.h missing after SDK fetch ($wvInclude)" }
if (-not (Test-Path $wvLib)) { throw "WebView2LoaderStatic.lib missing after SDK fetch ($wvLib)" }
Write-Host "WebView2 SDK ready at $sdkDir"

# --- 3. embed the HTML --------------------------------------------------------------------------------
# Wrap mockup.html verbatim in a C++ raw string literal so the DLL carries the UI with no shipped file.
$htmlPath = Join-Path $here "webview\mockup.html"
if (-not (Test-Path $htmlPath)) { throw "mockup.html not found at $htmlPath" }
$html = Get-Content -Raw -Path $htmlPath
# MSVC caps a single string literal at ~16 KB (error C2026). Split into <16 KB chunks emitted as
# ADJACENT raw string literals -- the compiler concatenates them into one array. Raw literals need no
# escaping; any byte is safe except the exact ")SNAPHAK" delimiter, which the HTML never contains.
$chunkSize = 8000
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* generated from webview/mockup.html by build-webview.ps1 -- do not edit */")
[void]$sb.AppendLine("static const char kMockupHtml[] =")
for ($i = 0; $i -lt $html.Length; $i += $chunkSize) {
    $len = [Math]::Min($chunkSize, $html.Length - $i)
    [void]$sb.AppendLine('R"SNAPHAK(' + $html.Substring($i, $len) + ')SNAPHAK"')
}
[void]$sb.AppendLine(";")
$hdrPath = Join-Path $objDir "mockup_html.h"
Set-Content -Path $hdrPath -Value $sb.ToString() -Encoding ascii -NoNewline
Write-Host "generated $hdrPath ($([Math]::Round(($html.Length/1KB),1)) KB of HTML)"

# --- 4. compile -------------------------------------------------------------------------------------
# /MD (dynamic CRT: the WebView2 static loader + the process's existing MSVCP140/VCRUNTIME140 expect it),
# /EHsc /std:c++17. Includes: WebView2 headers, the generated header dir, the shared iface ABI dir.
# Sources: the WebView2 host + the unchanged sl_* export stubs. Links the static WebView2 loader + the
# Win32 libs its COM/shell calls need. NO Qt. /DEF pins the OG export set (snaphak_ui_init @10 + sl_*).
$incArgs = @(
    "/I`"$wvInclude`"",
    "/I`"$objDir`"",
    "/I`"$common`""
) -join " "
$srcArgs = "webview\snaphak_ui_webview.cpp sl_exports.cpp"
$libArgs = @(
    "`"$wvLib`"",
    "ole32.lib", "oleaut32.lib", "shell32.lib", "shlwapi.lib",
    "version.lib", "advapi32.lib", "user32.lib", "gdi32.lib"
) -join " "
$implib = $Out -replace '\.dll$', '.lib'

$cl  = "cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MD /DWIN32 /D_WINDOWS /Fo..\..\build\obj\uiwv\ " +
       "$incArgs $srcArgs /Fe:..\..\build\$Out " +
       "/link /DEF:snaphakui.def /IMPLIB:..\..\build\obj\uiwv\$implib $libArgs"
$cmd = "cd /d `"$here`" && `"$vcvars`" && $cl"

$buildLog = Join-Path $build "build-webview.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
Write-Host "built $(Join-Path $build $Out) (Qt-free WebView2 POC)"
