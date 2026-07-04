# Experimental WebView2 frontend (HTML "SnapHak Studio")

An alternative, **Qt-free** build of `snaphakui.dll` that renders the "SnapHak Studio" UI as HTML/CSS/JS
in a Microsoft Edge **WebView2** control instead of a Qt widget tree. It is a drop-in replacement for the
Qt frontend: same `snaphak_ui_init` entry (export ordinal 10), the same backend interface contract
(`src/common/snaphak_iface.h`), and the same manual 30 Hz think-loop draining the work-queue (`+0x1a0`).
The backend (`XINPUT1_3.dll`) is unchanged.

**Status: experimental / proof-of-concept.** This is a *parallel* frontend, not a replacement of the Qt
one. The default `build.ps1` still builds the faithful Qt UI; this frontend is built with a separate,
opt-in script (`build-webview.ps1`). It is not wired into CI.

## Why

- Drop the ~18 MB Qt runtime shipped in the overlay (`Qt5Core` / `Qt5Gui` / `Qt5Widgets` + the `qwindows`
  platform plugin).
- Iterate on the UI in HTML/CSS/JS instead of a hand-written Qt `setupUi`.
- Combine the Entities + Entity-State tabs into a single view.

Trade-off: it depends on the Microsoft Edge **WebView2 runtime** (preinstalled on Windows 11 and most
Windows 10) instead of the bundled Qt DLLs. The compiled DLL is ~120 KB (vs ~1.5 MB for the Qt one) and
statically links the WebView2 loader, so no extra loader DLL ships.

## Files

| File | What |
|---|---|
| `src/ui/webview/snaphak_ui_webview.cpp` | The WebView2 host: the `snaphak_ui_init` entry, a Win32 window, the WebView2 bring-up, the 30 Hz think-loop, and the JS <-> native bridge. |
| `src/ui/webview/mockup.html` | The UI (HTML/CSS/JS), embedded into the DLL at build time. Self-populates with sample data when opened in a plain browser (a "preview mode", inert in DOOM). |
| `src/ui/build-webview.ps1` | Builds a Qt-free `build/snaphakui.dll`: fetches the WebView2 SDK from NuGet into `build/` (gitignored), statically links the loader, embeds the HTML. Reuses the unchanged `sl_exports.cpp` + `snaphakui.def`. |

## Build + deploy

```powershell
# from src/ui/
powershell -NoProfile -ExecutionPolicy Bypass -File build-webview.ps1   # -> build/snaphakui.dll (Qt-free)
# from the repo root
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1          # -> dist/
installer\snaphak.exe install --local dist                               # deploy (DOOM must be closed)
```

Do **not** run `build.ps1` after `build-webview.ps1` -- it rebuilds the Qt `snaphakui.dll` and overwrites
the WebView2 one. Runtime log: `<DOOM>\snaphak_logs\webview_poc.log`. The overlay still copies the three
Qt DLLs (dead weight for this frontend); stripping them from `package.ps1` is a pending cleanup.

## How it maps to the backend interface

The frontend holds no engine addresses; it calls the backend only through the vtable slots pinned in
`src/common/snaphak_iface.h`:

| UI feature | Interface slot(s) |
|---|---|
| Entity list (valid ids, id-strings, displaynames) | `entity_count` +0x10, `is_valid_id` +0x28, `id_to_string` +0x18, `get_displayname` +0x58 |
| Hidden (dev-layer) filter | `id_dev_layer_hidden` +0x280 |
| Window shown only in the editor | `editor_ready_poll` +0x88 |
| State editor read | `get_declsource_copy` +0x30, `get_classname_copy` +0x48, `get_inherit_copy` +0x50, `get_displayname` +0x58 |
| Save to Decl | `apply_class_inherit` +0x268, `set_classname` +0x78, `set_inherit` +0x80, `set_entity_0x170` +0x128, `rebuild_set_declsource` +0x40 |
| Delete (context menu) | `selection_guard` +0x130 |
| Synchronize with editor (editor -> list) | `get_selection` +0x150 |
| Select in editor (list -> editor) | `clear_selection` +0x148, `add_to_selection` +0x138 |
| Class / Inherit autocomplete | `enum_valid_classes` +0x270, `enum_inherits` +0x278 |
| Installed version readout | reads `%LOCALAPPDATA%\open-snaphak\install.json` (written by the installer) |

Heavy engine writes (Save, Delete, Select-in-editor) are snapshotted in the JS message callback and
applied on the next think-loop frame under the loop mutex -- mirroring the Qt frontend's flag-word
dispatch, which keeps them off the re-entrant callback and on the main-thread execution point.

## Implemented (changelog)

- WebView2 host DLL that opens in DOOM, gated to the SnapMap editor (hidden on the menu).
- Live entity list: walks `0..entity_count`, keeps `is_valid_id`, skips `NULL_` placeholder slots, shows
  real id-strings + displaynames, sorted; auto-refreshes on a content signature (no needless re-renders).
- Dev-layer hidden entities filtered out by default, with a "Show hidden entities" toggle (greyed/italic).
- Live entity count (drops when entities are deleted).
- State editor: reads an entity's decl source + class / inherit / displayname; "Save to Decl" commits.
- Class / Inherit as an editable combobox with autocomplete (full list on the arrow, type to filter,
  free-text still allowed); blank class/inherit is blocked with an error toast.
- Multi-select (click / Ctrl-click / Shift-click, no text-highlight); the state editor steps aside for a
  placeholder when 2+ are selected (its actions live in the context menu).
- Right-click context menu: Copy ID (clipboard), Delete, Push to stack 0 (a stub -- see limitations).
- Sliding toasts for Copy / Save / Delete / Push, color-coded (success / warning / error).
- "Synchronize with editor" (editor selection -> list, any N) and "Select in editor" (list selection ->
  editor, hidden entities skipped). The two are mutually exclusive to avoid a selection feedback loop.
- Installed-version + connection status in the status bar.
- Browser preview mode: `mockup.html` self-populates with sample data and is fully interactive when
  opened without a WebView2 host (for fast UI iteration); inert in DOOM.

## Known limitations / TODO

- **Prefabs** and **Timelines / Timeline Editor** tabs are not ported (the Qt frontend has them). The heavy
  serialize/apply slots the Prefabs tab needs are not bound in the current backend build.
- **Push to stack 0** is a stub: the SnapStack subsystem (`snapstack.cpp`) is Qt-bound and its consuming
  ops are not ported to this frontend.
- Editing an entity's decl does not re-present it live in the editor (a decl commit updates the definition
  but not the already-spawned instance -- same as Save-to-Decl in the Qt UI). A live in-editor re-present
  via the engine's per-entity refresh is a possible future experiment.
- Undo covers only unsaved edits (the Revert button + the textarea's native undo); undoing a committed Save
  is not implemented.
- The overlay still ships the three Qt DLLs, and `build-webview.ps1` fetches the newest WebView2 SDK (a
  prerelease); both are pending cleanups (strip the Qt DLLs from `package.ps1`; pin the SDK).
- Not wired into CI (CI builds the Qt path via `build.ps1`).

## Preview mode

Open `src/ui/webview/mockup.html` directly in a browser to see and click through the UI with fake data --
useful for iterating on layout/behavior without building or deploying. This preview branch only runs when
there is no WebView2 host, so it has no effect inside DOOM.
