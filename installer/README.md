# snaphak.exe — the end-user installer

A small, single static Windows CLI that installs the SnapHak clone overlay into a DOOM 2016 install, with
backup and a clean uninstall. **Stdlib only — no external dependencies.**

## Build

```
go build -o snaphak.exe .            # needs Go 1.21+
go build -ldflags "-X main.version=v1.2.3" -o snaphak.exe .   # stamp a release version (CI does this)
```

## Use

```
snaphak install   [--doom <path>] [--local <dist-dir>] [--release <tag>] [--beta] [--yes]
snaphak update    [--doom <path>] [--release <tag>] [--beta] [--yes]
snaphak uninstall [--doom <path>] [--yes]
snaphak changelog
snaphak status
snaphak version
snaphak help
```

`install` / `update` / `uninstall` ask for a final **"are you sure?"** confirmation (after all checks pass);
`--yes` / `-y` skips it for scripts. **`snaphak changelog`** prints the published version history + notes
(it lives in the GitHub Releases — CI auto-generates each release's notes from the commits since the last tag).

- **`--doom <path>`** — the DOOM install dir (the folder with `DOOMx64vk.exe`). If omitted, the installer
  auto-detects it from your Steam libraries (reads `SteamPath` from the registry, scans
  `libraryfolders.vdf` for the library holding appid `379720`, and verifies `DOOMx64vk.exe` is there).
- **`--local <dist-dir>`** — install from a local `dist/` tree (built by the repo's `package.ps1`) instead of
  downloading. This is the contributor / local-test
  path. The installer reads whatever files `MANIFEST.sha256` lists and installs exactly those -- it has no
  hardcoded assumption about which frontend's bundle shape it's given.
- **`--release <tag>`** — install a specific release version instead of the latest.
- **`--beta`** — install the latest **beta** (pre-release) instead of the latest stable.

With no `--local`, `install`/`update` download from GitHub — the latest stable by default; while no
stable release has been published yet, they fall back to the newest beta (and say so).

## What it does

- **DOOM must be closed** for install / update / uninstall — a running game locks its DLLs. The installer
  detects a running DOOM and asks you to close it, instead of failing with a cryptic Windows file error.
- Verifies the bundle against its `MANIFEST.sha256` (every file present + hash-correct) **before** touching DOOM.
- **Backs up** any pre-existing file it would overwrite (e.g. a genuine `XINPUT1_3.dll`) to `<file>.snaphak-bak`.
- Records the install (files placed + backups taken) in `%LOCALAPPDATA%\open-snaphak\install.json`.
- **`uninstall`** reverses *exactly* that record: removes the files it placed, restores the backups, and cleans
  the dirs it created **only if they're empty** — a pre-existing `platforms/` or other content is left intact. Your
  `%USERPROFILE%\snaphak` data (overrides / prefabs / rawmaps) is **never** touched.

## Releases & channels

`install` / `update` download from **`snaphak/open-snaphak`** releases. `snaphak.exe` installs itself to
`%LOCALAPPDATA%\open-snaphak\` (with its `install.json` record); **double-clicking it** (no args) opens a
status-aware interactive prompt: not installed → Enter installs (auto-detect DOOM → confirm); installed
with a newer release out → an update notice, Enter updates; and a `snaphak>` prompt takes every command
above (with flags), so update / uninstall / changelog work without a terminal or PATH. `update` also
refreshes `snaphak.exe` itself (skip with `--no-self`).

- **Stable** (`snaphak update`): the latest plain `vX.Y.Z` release.
- **Beta**: a `vX.Y.Z-beta.N` pre-release — `snaphak update --beta` (latest beta) or
  `snaphak install --release <tag>` (a specific one).
- Pin any version with `--release <tag>` on install/update.

### Private-repo access (not needed while the repo is public)

If the release repo is private (e.g. a closed beta), downloading a release needs a GitHub token. Save one once:

```
snaphak set-token <github-token>      # stored in %LOCALAPPDATA%\open-snaphak\token
```

Then `snaphak update --beta` (and any install/update) authenticates with it. The installer also honors a
`SNAPHAK_TOKEN` env var or a `--token <tok>` flag. Use a **fine-grained** token scoped to this repo with
**Contents: read-only**. Once the repo is public, no token is needed.

(The release path goes live once the first GitHub Release is published; until then, use `--local <dist>` to
install from a local `package.ps1` build.)
