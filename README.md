# Snapmap+

**Snapmap+** is an open-source, clean-room reimplementation of **SnapHak** — Chrispy's closed-source
modding tool for DOOM 2016's in-game **SnapMap** level editor. It builds to two drop-in DLLs that,
deployed into a stock DOOM 2016 install, reproduce (and extend) the original's editor extensions: the
console-command/cvar hook layer and the **Snapmap+** window.

**This repo ships NO DOOM or SnapHak bytes.** Every line is built from the project's own reverse-engineering
of the engine and the original tool — no decompiled or copied binary content. The original SnapHak is
closed-source; this is an independent, ground-up reimplementation. Legitimate single-player game-modding
research; the third-party runtime it links against (the DOOM engine, Microsoft's WebView2 runtime) is not included.

## Repository layout

| Path | What |
|---|---|
| `src/backend/` | the backend DLL (`XINPUT1_3.dll`): the hook layer, 29 console commands, 10 cvars, cvar-unlock, and the resident fault-shield |
| `src/ui/` | the frontend DLL (`snapmap-plus-ui.dll`): the WebView2 **Snapmap+** window (`webview/` = the host + `mockup.html`) |
| `src/common/` | the shared backend↔frontend interface ABI (`snapmap_plus_iface.h`) |
| `src/fault_shield/` | the recover-in-place vectored-exception fault shield (compiled into the backend) |
| `build.ps1` | compile the DLLs → `build/` (backend + frontend; `-BackendOnly` skips the frontend) |
| `package.ps1` | assemble the deployable overlay → `dist/` (the two clone DLLs) |
| `installer/` | `snapmap-plus.exe` — the end-user install / update / uninstall CLI (Go) |
| `docs/` | contributor documentation: architecture · capabilities · fidelity · packaging · webview-ui · backend-changes |
| `site/` | the website ([doom-snapmap.github.io/snapmap-plus](https://doom-snapmap.github.io/snapmap-plus/)) — deployed by `.github/workflows/pages.yml` |

`build/` and `dist/` are gitignored — the **source is the deliverable**; the binaries are rebuilt.

## Quick start (players)

You do **not** need to build anything. Get `snapmap-plus.exe` from the latest release and **double-click it** — it
auto-detects your DOOM install via Steam, asks you to confirm, and installs. (From a terminal: `snapmap-plus install`.)

`snapmap-plus.exe` installs itself to `%LOCALAPPDATA%\snapmap-plus\` (also the home of your overrides /
prefabs / rawmaps). Run it again any time for `snapmap-plus update`, `snapmap-plus status`,
`snapmap-plus version`, and `snapmap-plus uninstall` (which restores DOOM to vanilla and leaves your
modding data untouched). Coming from the **original SnapHak**? Install/update detects it in
your DOOM folder and removes its files as part of the install — your maps, prefabs and overrides carry
straight over. See [`installer/README.md`](installer/README.md).

> Releases are produced by CI. Until the first release is published, build from source (below).

## Build from source

**Requirements** (exact download links + setup are in [`docs/contributing.md`](docs/contributing.md))
- **MSVC 2022 Build Tools** (the "Desktop development with C++" workload)
- **Go 1.21+** (only to build the installer)

The frontend renders in the Microsoft Edge **WebView2 runtime** (preinstalled on Windows 11 / most Windows 10);
its SDK is auto-fetched from NuGet at build time. Nothing else to install.

```powershell
# 1. compile both DLLs -> build/XINPUT1_3.dll + build/webview/snapmap-plus-ui.dll
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 2. assemble the deployable overlay -> dist/ (the 2-file DOOM tree: the two clone DLLs)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File package.ps1

# 3. (optional) build the installer
cd installer ; go build -o snapmap-plus.exe .
```

## Deploy a local build (contributors / testing)

Deploy your fresh `dist/` into your own DOOM with the installer's **local** mode — the same path end users
take, just from your build instead of a release:

```
installer\snapmap-plus.exe install --local dist
```

`snapmap-plus.exe uninstall` reverses it. (Or drop `dist\*` into the DOOM root by hand — `dist/` mirrors the exact
overlay tree.) Launch DOOM, enter the SnapMap editor; the Snapmap+ window opens (run `sh` in the
console if it doesn't). DOOM keeps using the real `XInput1_3.dll` in System32 for controller input — the
backend forwards every XInput export through to it.

## Versioning & releases

Versions follow **semantic versioning** — `vMAJOR.MINOR.PATCH` (e.g. `v0.1.0`). **The git tag is the version**;
there is no `VERSION` file to maintain. One tag = one release containing **both** the mod bundle and
`snapmap-plus.exe`, both stamped with that tag.

Cut a release (maintainer):

```
git tag v0.1.0
git push origin v0.1.0      # fires .github/workflows/release.yml
```

CI builds the DLLs + the installer (stamping `snapmap-plus.exe` via `-ldflags -X main.version=v0.1.0`), packages the
overlay, and publishes a GitHub Release with `snapmap-plus-bundle.zip` + `snapmap-plus.exe` + `install.ps1`.

**Release channels** (set by the *tag*, not a branch):
- **Stable** — a plain tag `v0.3.0`. This is what end users' `snapmap-plus update` gets.
- **Beta** — a pre-release tag `v0.3.0-beta.1` (any tag with a `-`; CI auto-marks it a GitHub pre-release). It's
  excluded from "latest", so end users never receive it. Beta testers opt in:
  `snapmap-plus install --release v0.3.0-beta.1`.

Pin any version explicitly with `--release <tag>` on `install` or `update`.

- **`snapmap-plus version`** prints the installer's version (and the installed mod version, if any).
- **`snapmap-plus update`** pulls the latest release; **`snapmap-plus status`** shows what's installed.
- A local/dev build reports `dev` (unstamped) or `local` (a `--local` install) — never a release number.

**Surviving DOOM updates (planned):** the clone resolves engine functions by *signature*, so many DOOM patches
need no rebuild at all. When a patch shifts things enough to require one, an **auto-re-patcher** CI job
(re-resolve signatures against the new DOOM build → rebuild → if green, publish a compatible release) is the
intended automation. Stubbed for now (see `release.yml`).

## Contributing

Contributions are welcome. **New here?** The full guide — fresh-machine setup (Git, MSVC, Go), the
build → package → test loop, the pull-request workflow, and the rule that the `docs/` are updated alongside
code — is in **[`docs/contributing.md`](docs/contributing.md)**. The short version:

1. **Fork** this repo (or branch, if you have write access).
2. Make your change under `src/`. Build (`build.ps1`), package (`package.ps1`), and test it in your own DOOM
   via `installer\snapmap-plus.exe install --local dist`.
3. Open a **pull request** against `main`. The CI gate runs a security scan (no new binaries · capability-surface
   scan · gitleaks), the Windows build + package, the XInput ordinal-parity check, the C unit tests
   (`tests\run-tests.ps1`), and the installer's `gofmt`/`vet`/`test`; a maintainer reviews and merges. Tagged,
   reviewed commits are what produce releases.

**Keep PRs clean:**
- **No binaries.** Never commit a `.dll`/`.exe`/`.obj`/etc. — they're gitignored and **CI rejects any PR that
  adds one**. The source is the only deliverable; CI builds the binaries.
- **Clean-room only.** Contribute your **own** RE/implementation. Do not paste decompiled or copyrighted
  DOOM/SnapHak content.
- **Match the surrounding code** — the backend is plain C, the frontend is C++ + HTML/CSS/JS; keep source **pure ASCII** (the
  PowerShell build reads BOM-less UTF-8 as Windows-1252). Run **`gofmt`** on anything in `installer/`.

Because the tool injects into DOOM, the release channel is a supply-chain target. PR CI runs in a
**secretless** sandbox (it cannot publish or touch signing keys), a maintainer reviews every diff, and a scan
flags any newly-introduced network / process-spawn / persistence code — the tool has no legitimate reason to
do any of that.

### Generated headers — don't hand-edit

A few committed headers are **generated data tables** derived from the project's reverse-engineering of the
engine and the original tool, not hand-authored source: `src/ui/sh_*.h` (entity descriptions, event
catalog/docs, asset lists) and `src/backend/class_universe.h`. They're checked in
so the repo builds standalone — treat them as **vendored**: don't hand-edit them in a PR; open an issue
describing the change instead.

## Architecture & reference

| Doc | What |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | the backend↔frontend boundary, the WebView2 host, the 30 Hz think-loop, the 77-slot interface vtable |
| [`docs/fidelity.md`](docs/fidelity.md) | the original's quirks the clone reproduces on purpose, and the one sanctioned divergence (the fault-shield) |
| [`docs/capabilities.md`](docs/capabilities.md) | the full feature inventory — every console command, cvar, SnapStack op, and GUI tab |
| [`docs/packaging.md`](docs/packaging.md) | the deployable bundle: the lean 2-file overlay |

## Overrides (runtime)

At runtime the tool reads per-user **override decls** from `%LOCALAPPDATA%\snapmap-plus\overrides\` (a
file-shadow over the engine's resource loader — e.g. to make extra editor entities placeable). Resolution is
three-layer: **your file wins**, then the tool's few built-in default decls (the "*Custom" palette tab —
served from memory, never written to your folder; delete your file at one of those names to get the
default back), then the game's own packaged resource. A broken override set can be bisected by setting
the `sh_user_overrides` cvar to 0 (ignores your files; built-ins still serve) or renaming the
`overrides` folder. The backend log lists your active overrides at startup. Runtime logs go to
`<DOOM>\snapmap-plus\logs\`. (Content from the original SnapHak's / older releases' `%USERPROFILE%\snaphak`
folder is copied forward on install; a legacy root-level `snaphak_logs\` is folded in too.)

## Credits

Snapmap+ stands on the shoulders of **SnapHak**, the original closed-source SnapMap modding tool by
**Chrispy**. The original pioneered the rawmap format, the override file-shadow, the hidden-entity
unhide, and the in-editor companion window that this project reimplements clean-room — every feature
here traces back to what that tool proved possible. Thank you, Chrispy.

## License

MIT — see [`LICENSE`](LICENSE).
