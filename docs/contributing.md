# Contributing to Snapmap+

Snapmap+ (repo `snapmap-plus`) is an open-source, clean-room reimplementation of **SnapHak** — Chrispy's closed-source modding
tool for DOOM 2016's in-game **SnapMap** level editor. It builds to two drop-in Windows DLLs (a backend
`XINPUT1_3.dll` and a WebView2/HTML frontend `snapmap-plus-ui.dll`) plus a Go installer (`snapmap-plus.exe`). This guide takes you
from a **fresh Windows machine** all the way to a built, tested change and an open pull request.

If anything here is wrong, missing, or unclear, fixing it is itself a welcome PR.

## Contents

1. [Ground rules](#1-ground-rules)
2. [Prerequisites (fresh Windows machine)](#2-prerequisites-fresh-windows-machine)
3. [Get the source](#3-get-the-source)
4. [Build the DLLs](#4-build-the-dlls)
5. [Package the overlay](#5-package-the-overlay)
6. [Deploy and test in DOOM](#6-deploy-and-test-in-doom)
7. [Run the tests](#7-run-the-tests)
8. [The pull-request workflow](#8-the-pull-request-workflow)
9. [Keep the docs in sync (required)](#9-keep-the-docs-in-sync-required)
10. [Generated headers — don't hand-edit](#10-generated-headers--dont-hand-edit)
11. [Reporting security issues](#11-reporting-security-issues)
12. [Repository layout](#12-repository-layout)
13. [Glossary](#13-glossary)

## 1. Ground rules

- **Clean-room only.** Contribute your **own** reverse-engineering and implementation. Never paste decompiled,
  disassembled, or copyrighted DOOM or original-SnapHak content into this repo. This repo ships no DOOM or
  SnapHak bytes, and it must stay that way.
- **No binaries — ever.** `.dll`, `.exe`, `.obj`, `.lib`, `.pdb`, `.zip`, … are gitignored, and CI **rejects
  any PR that adds one**. The source is the only deliverable; CI builds the binaries.
- **Pure ASCII source.** The PowerShell build reads BOM-less UTF-8 as Windows-1252, so keep `.c` / `.h` /
  `.cpp` / `.ps1` files ASCII-only (no smart quotes, em dashes, or accented characters in source).
- **Match the surrounding code.** The backend is plain C; the frontend is C++ (the WebView2 host) + HTML/CSS/JS
  in `mockup.html`; the installer is Go (run `gofmt`).
- **Supply-chain awareness.** Because the tool loads into DOOM, releases are a supply-chain target. PR CI runs
  in a secretless sandbox (it can't publish or touch signing keys), a maintainer reviews every diff, and a scan
  flags any new network / process-spawn / persistence code — the tool has no legitimate reason for any of that.

## 2. Prerequisites (fresh Windows machine)

You need 64-bit **Windows 10 or 11** and the tools below. Install them in this order.

| # | Tool | Version | Get it from |
|---|---|---|---|
| 1 | **Git** | any recent | <https://git-scm.com/download/win> |
| 2 | **Visual Studio 2022 Build Tools** | 2022 (v17) | <https://aka.ms/vs/17/release/vs_BuildTools.exe> |
| 3 | **Go** | 1.21+ | <https://go.dev/dl/> |
| 4 | **DOOM 2016** | Steam (app 379720) | required to actually run/test the mod |

The frontend renders in the Microsoft Edge **WebView2 runtime**, preinstalled on Windows 11 and on most
Windows 10 (via Edge) — nothing to install to build or run. (Its SDK headers + static loader are fetched
from NuGet at build time; there is nothing else to install.)

**Visual Studio 2022 Build Tools.** Run the installer and tick the **"Desktop development with C++"**
workload. That installs the MSVC x64 compiler (`cl.exe`) and the Windows 10/11 SDK the build needs. The build
locates the toolchain with `vswhere`, requiring the `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`
component — included in that workload. You do **not** need the full Visual Studio IDE; the Build Tools suffice.

**Go** is only needed to build the installer (`snapmap-plus.exe`); you can skip it if you only touch the DLLs.

**DOOM 2016** (Steam app `379720`) is required to deploy and test a build in the game.

> PowerShell tip: the build scripts are run with `-ExecutionPolicy Bypass` (shown below), so you don't need to
> change your machine's execution policy.

## 3. Get the source

Fork the repo on GitHub (the **Fork** button) unless you have write access, then:

```powershell
git clone https://github.com/<your-username>/snapmap-plus.git
cd snapmap-plus
```

## 4. Build the DLLs

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

This compiles both DLLs into **`build/`**: the backend `XINPUT1_3.dll` and the frontend
`build/webview/snapmap-plus-ui.dll` (the WebView2 SDK is auto-fetched from NuGet on the first build). `build/`
is gitignored. (`build.ps1` first builds the backend, then the frontend, so the two never drift out of ABI
sync -- see [`architecture.md`](architecture.md).)

`build.ps1` is the one top-level build script (used above and by CI). Pass `-BackendOnly` to skip the
frontend when iterating on backend code alone; any extra args (e.g. `-Diag`) forward through to
`src\backend\build.ps1`. The frontend itself is described in [`webview-ui.md`](webview-ui.md).

## 5. Package the overlay

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
```

This assembles the deployable **2-file overlay** into **`dist/`**: the two clone DLLs (the frontend
renders in the system-installed WebView2 runtime), laid out exactly as they drop into a DOOM install,
alongside a `MANIFEST.sha256`. `dist/` is gitignored. See [`docs/packaging.md`](packaging.md) for the full
file list.

## 6. Deploy and test in DOOM

Build the installer once, then deploy your fresh `dist/` into your own DOOM with its **local** mode:

```powershell
cd installer ; go build -o snapmap-plus.exe . ; cd ..
installer\snapmap-plus.exe install --local dist
```

It auto-detects your DOOM via Steam (or pass `--doom <path>` to the folder with `DOOMx64vk.exe`), backs up
anything it replaces, and records the install so **`installer\snapmap-plus.exe uninstall`** restores vanilla
exactly. (You can also drop `dist\*` into the DOOM root by hand — `dist/` mirrors the overlay tree.)

Launch DOOM and enter the SnapMap editor; the **Snapmap+** window opens (run `sh` in the in-game
console if it doesn't). When you're done, `snapmap-plus.exe uninstall` returns DOOM to vanilla and leaves your
modding data (`%LOCALAPPDATA%\snapmap-plus`) untouched.

## 7. Run the tests

**The installer (Go):**

```powershell
cd installer
gofmt -l .            # must print nothing (format with: gofmt -w .)
go vet ./...
go test ./...
cd ..
```

**The native unit tests:**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1
```

By default this compiles and runs eleven **self-contained native tests** (no game needed):

- **`shield_format_test`** — the fault-record string formatter (pure logic).
- **`hook_test`** — the inline-detour installer, exercised on a hand-laid scratch stub.
- **`crash_record_test`** — crash-record JSON formatting and escaping.
- **`report_scrub_test`** — report-log anonymization and bounded tail selection.
- **`dumpmap_path_test`** — `sh_dumpmap` path validation and output-name construction.
- **`config_json_test`** — bounded UTF-8 JSON parsing, duplicate-key rejection, mutation, and serialization.
- **`iface_config_test`** — the pinned `+0x2B0` / `+0x2B8` config ABI and callback binding.
- **`config_test`** — config creation, validation/repair/recovery, preservation, atomic-failure behavior,
  deletion reset, external/process writers, and the registered service.
- **`config_message_test`** — bounded raw WebView config-message extraction before UTF-8 conversion.
- **`theme_bootstrap_test`** — pre-navigation root-class seeding for a saved dark theme.
- **`theme_contract_test`** — the HTML config-message contract and PREVIEW-only browser storage.

Two more tests scan a **real DOOM image** — a `DOOMx64vk.exe` that's been unpacked from its Steam DRM wrapper
(e.g. with Steamless). Run them only if you're touching the signature resolver (`src/backend/signatures.c`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1 -Doom C:\path\to\unpacked-DOOMx64vk.exe
#   sig_test     -- every engine signature resolves to its known RVA
#   hooktol_test -- the resolver's hook-tolerant fallback (prologue-clobbered functions)
```

A third test, `xinput_ordinal_test.c`, is a **runtime** cross-check of the XInput ordinal invariant — it loads
a built DLL and calls its exports by ordinal. CI verifies that same invariant *statically* with `dumpbin` (the
"XInput ordinal parity" step), so you normally don't need to run it by hand.

CI runs the eleven self-contained native tests and the installer tests on every PR; the DOOM-image tests are local-only
(CI has no game image).

## 8. The pull-request workflow

1. **Branch:** `git switch -c fix/steam-path-detection` (or `feature/<thing>`).
2. **Change** code under `src/` (or `installer/`). Keep each PR focused on one thing.
3. **Build + package + test in DOOM:** `build.ps1` → `package.ps1` → `snapmap-plus.exe install --local dist`.
   A local round-trip is much faster than waiting on CI, and lets you actually see it working in DOOM rather
   than just "the build didn't fail." See [`architecture.md`](architecture.md)'s note on the vtable's
   extension slots for why a backend/frontend version mismatch is a real failure mode, not a theoretical one.
4. **Run the tests** (section 7) — both the Go and C suites.
5. **Update the docs** your change affects (section 9).
6. **Commit** with a clear, imperative message that names the area, e.g.
   `installer: fix Steam library path detection` or `backend: add sh_listwires command`.
7. **Push** to your fork and open a **pull request against `main`**.
8. The **CI gate** runs automatically, as two parallel jobs: a security scan (no-new-binaries ·
   capability-surface scan · gitleaks); and the build (`build.ps1` / `package.ps1`, a bundle guard that
   asserts the overlay stays the lean 2-file set, XInput ordinal parity, the native unit tests, and the installer's
   `gofmt` / `vet` / `test`). It runs in a **secretless** sandbox — fork PRs get a read-only token and zero
   repo secrets.
9. A **maintainer reviews** and merges (changes under `.github/`, `*.ps1`, and `installer/` are
   CODEOWNERS-gated). Releases are cut from reviewed, tagged commits — see the README's
   ["Versioning & releases"](../README.md#versioning--releases).

## 9. Keep the docs in sync (required)

**A change that alters behavior must update the matching docs in the *same* PR.** Reviewers check for this — a
behavior change with stale docs will be sent back. Use this map:

| If you change… | Update… |
|---|---|
| a console command, cvar, SnapStack op, or a Studio-window feature (`src/backend/`, `src/ui/`) | [`docs/capabilities.md`](capabilities.md) — the feature inventory |
| the frontend UI itself (`src/ui/webview/` — the host or `mockup.html`) | [`docs/webview-ui.md`](webview-ui.md) — its reference sections + a dated Changelog entry |
| the object model, the think-loop, the interface vtable, the persistent-settings registry, or the backend↔frontend boundary | [`docs/architecture.md`](architecture.md) |
| a deliberately-reproduced original quirk, or a sanctioned divergence | [`docs/fidelity.md`](fidelity.md) |
| a correctness bugfix in the shared `src/backend/` engine-call layer (not a fidelity divergence -- our own code was wrong) | [`docs/backend-changes.md`](backend-changes.md) |
| the shipped file set / what's deliberately dropped (`package.ps1`) | [`docs/packaging.md`](packaging.md) |
| the install / update / uninstall flow, its flags, or release channels (`installer/`) | [`installer/README.md`](../installer/README.md) and, if user-facing, the top-level [`README.md`](../README.md) |
| the build, test, or contribution process | this file (`docs/contributing.md`) |

If a change is purely internal and user-invisible, note that in the PR description so the reviewer knows the
docs were considered.

## 10. Generated headers — don't hand-edit

A few committed headers are **generated data tables**, not hand-authored source: `src/ui/sh_*.h` (entity
descriptions, the event catalog/docs, asset lists) and `src/backend/class_universe.h`. They're checked in so
the repo builds standalone — treat them as **vendored**.
Don't hand-edit them in a PR; open an issue describing the change you need instead.

## 11. Reporting security issues

This tool injects into DOOM, so a vulnerability here is a supply-chain risk for everyone who installs a
release. **Do not open a public issue for a security problem.** Use GitHub's **private vulnerability reporting**
(the repo's **Security** tab → **Report a vulnerability**) so it can be fixed before disclosure.

## 12. Repository layout

| Path | What |
|---|---|
| `src/backend/` | the backend DLL (`XINPUT1_3.dll`): the hook layer, console commands, cvars, persistent configuration, cvar-unlock, the resident fault-shield |
| `src/ui/` | the frontend DLL (`snapmap-plus-ui.dll`): the WebView2 Snapmap+ window -- `webview/` holds the host (`snapmap_plus_ui_webview.cpp`) + the UI (`mockup.html`) |
| `src/fault_shield/` | the recover-in-place vectored-exception fault shield (compiled into the backend) |
| `src/common/` | the shared backend↔frontend interface ABI (`snapmap_plus_iface.h`) |
| `installer/` | `snapmap-plus.exe` — the Go install / update / uninstall CLI |
| `tests/` | the native unit tests + `run-tests.ps1` |
| `docs/` | architecture · capabilities · fidelity · packaging · webview-ui · backend-changes · this guide |
| `build.ps1` | compile the DLLs → `build/` (backend + frontend; `-BackendOnly` for backend alone) |
| `package.ps1` | assemble the deployable overlay → `dist/` (the two clone DLLs) |
| `.github/workflows/` | `ci.yml` (the PR gate) · `release.yml` (tag-triggered release) |
| `LICENSE` | MIT |

## 13. Glossary

- **SnapMap** — DOOM 2016's in-game level editor. **Snapmap+** extends it.
- **The original SnapHak / "OG"** — Chrispy's closed-source tool that this project reimplements clean-room.
  **"The clone"** — this project's reimplementation.
- **The overlay** — the two files that deploy into a DOOM install (the backend + frontend DLLs).
- **Backend / frontend** — the backend `XINPUT1_3.dll` (the engine-side hook layer) and the frontend
  `snapmap-plus-ui.dll` (the WebView2/HTML UI); they talk over the interface ABI in `src/common/`.
- **`XINPUT1_3.dll` / ordinals** — the backend ships as an XInput proxy DLL DOOM already loads. It must export
  `XInputGetState` / `XInputSetState` at ordinals **2 / 3** (DOOM imports them *by ordinal*) and forwards every
  XInput call through to the real `System32` DLL, so the controller keeps working.
- **RVA** — relative virtual address: an offset into the DOOM image. **Signature ("sig") resolve** — locating
  an engine function by a masked byte-pattern instead of a hardcoded RVA, so the clone survives DOOM patches
  that shift addresses.
- **decl** — a DOOM engine declaration (an entity or resource definition). **cvar** — an engine console
  variable. **cvar-unlock** — re-enabling editor cvars the engine hides by default.
- **The fault-shield** — a vectored-exception handler that recovers in place from certain faults instead of
  letting the process die; the one sanctioned behavioral divergence from the original (see
  [`docs/fidelity.md`](fidelity.md)).
