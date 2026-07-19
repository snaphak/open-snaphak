# installer/build.ps1 -- build snaphak.exe with an embedded Windows version resource + application
# manifest, symbol-stripped and path-trimmed. ONE recipe for both local dev and the release pipeline
# (release.yml calls this with the tag), so the installer users download is byte-shaped the same way
# it is tested. Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252).
#
# Why the resource + manifest + trimpath: a bare Go executable with no CompanyName/ProductName/
# FileDescription, no application manifest, and embedded build paths is exactly the shape generic
# antivirus heuristics score as suspicious. Giving the binary a real version resource, a normal Win32
# application manifest (asInvoker, supported-OS list, DPI), and stripping the local build paths makes it
# present as an ordinary desktop app. (Authenticode signing -- the strongest lever -- is applied on top
# of this in release.yml when a signing identity is configured.)
#
# goversioninfo is a BUILD-TIME tool only: it is `go install`-ed into GOPATH\bin and run here to emit
# resource.syso (version numbers + the manifest, pure data). It never becomes a module dependency --
# go.mod stays stdlib-only, no go.sum. resource.syso is git-ignored and regenerated on every build.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1                       # dev build -> installer\snaphak.exe (version "dev")
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Version v1.2.3        # stamped
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Version v1.2.3 -Out ..\dist\snaphak.exe
param(
    [string]$Version = "dev",
    [string]$Out = "snaphak.exe"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here
try {
    # Numeric FixedFileInfo from a vMAJOR.MINOR.PATCH tag; a plain "dev" build stays 0.0.0.
    $maj = 0; $min = 0; $pat = 0
    $m = [regex]::Match($Version, '^v?(\d+)\.(\d+)\.(\d+)')
    if ($m.Success) {
        $maj = [int]$m.Groups[1].Value
        $min = [int]$m.Groups[2].Value
        $pat = [int]$m.Groups[3].Value
    }

    # Pin the resource generator (a build tool, not a dependency).
    go install github.com/josephspurrier/goversioninfo/cmd/goversioninfo@v1.4.1
    if ($LASTEXITCODE -ne 0) { throw "goversioninfo install failed" }
    $gvi = Join-Path (& go env GOPATH) "bin\goversioninfo.exe"
    if (-not (Test-Path $gvi)) { throw "goversioninfo.exe not found at $gvi" }

    # resource.syso = version resource (StringFileInfo from versioninfo.json, numeric/string version from
    # the tag) + the application manifest. Go auto-embeds any *.syso in the package dir at build time.
    & $gvi -64 -o resource.syso `
        -manifest snaphak.manifest `
        -file-version $Version -product-version $Version `
        -ver-major $maj -ver-minor $min -ver-patch $pat `
        -product-ver-major $maj -product-ver-minor $min -product-ver-patch $pat `
        versioninfo.json
    if ($LASTEXITCODE -ne 0) { throw "goversioninfo failed" }

    # -trimpath strips local build/module paths; -s -w drop the symbol + DWARF tables (standard release
    # hygiene, smaller binary). -X stamps the version string the CLI prints.
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $Out .
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }

    Write-Host "built: installer\$Out (version $Version)"
}
finally {
    Pop-Location
}
