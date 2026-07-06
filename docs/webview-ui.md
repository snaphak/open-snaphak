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
| Camera Origin (X/Y/Z + Lock Position) | `get_editor_vec3` +0x08, `set_editor_vec3` +0x00 |
| Installed version readout | reads `%LOCALAPPDATA%\open-snaphak\install.json` (written by the installer) |
| Deselect (explicit button, "Select in 3D editor" mode) | `clear_selection` +0x148 |
| Live "Create from selection (N)" button count | `get_selection` +0x150, polled every ~330 ms independent of the sync checkboxes |
| Prefabs list, detail pane, delete/rename, folders (create/rename/delete/move) | `resolve_prefab_path` +0xc0 only -- pure Win32 file/directory ops (`FindFirstFileA`, `DeleteFileA`, `MoveFileA`, `CreateDirectoryA`, `RemoveDirectoryA`) on the resolved path. No other engine slot involved, so none of this can hit the +0xb0 crash below. |
| Create from selection -- **BLOCKED, see Known limitations** | `serialize_selection` +0xb0 (hard-crashes DOOM on this build) |
| Load / Place -- **deferred, see Known limitations** | would need `apply_edit` kind=2 (stage + `PasteInstantiate` + enter grab mode), blocked on the same crash |

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
- "Follow editor selection" (editor selection -> list, any N) and "Select in 3D editor" (list selection ->
  editor, hidden entities skipped). The two are mutually exclusive to avoid a selection feedback loop.
- Camera Origin bar (always visible): X/Y/Z fields track the live editor camera; "Lock Position" pins it
  (writes the stored vec3 every frame); a committed field edit writes back. Mirrors the Qt camera sync.
- Modern light/dark theme with a menu bar toggle (remembered via localStorage); a menu bar with a Settings
  placeholder for future feature toggles. Native controls (scrollbars, checkboxes) follow the theme.
- Installed-version + connection status in the status bar.
- Browser preview mode: `mockup.html` self-populates with sample data and is fully interactive when
  opened without a WebView2 host (for fast UI iteration); inert in DOOM.
- Default window size bumped to 1440x900 (from 1040x720) so the Entities and Prefabs tabs fit without a
  manual resize on first launch.
- Explicit **Deselect** button next to "Select in 3D editor" (only visible while that mode is on): calls
  `clear_selection` directly. A native click on empty space in the 3D view doesn't clear a selection that
  was set via `add_to_selection` (confirmed: a purely native selection deselects fine on its own -- only
  our externally-driven selection gets stuck), and the root cause is unRE'd in this codebase, so this is a
  reliable escape hatch rather than a fix for the underlying click behavior.
- **Prefabs tab, wired to the real filesystem** (`%USERPROFILE%\snaphak\prefabs\`) -- no fake/mockup data:
  - Live list of real `.json` prefab files, refreshed from disk on every Prefabs-tab click; an empty-state
    message when there are none yet.
  - Detail pane on selecting a prefab: real entity count and a per-`className` tally, read directly from
    the file (a targeted "find key -> read quoted value" scan, not a full JSON parser -- same approach as
    the JS<->native command parsing). Description/Tags fields are visibly disabled ("a later step"): the
    prefab JSON has no metadata field, so there's nothing to show until a sidecar file is designed.
  - Delete and Rename are real file operations (`DeleteFileA` / `MoveFileA`), each with a collision/confirm
    guard client-side and a safe no-overwrite guarantee native-side.
  - **Folders**: one real level of subdirectories under `prefabs\` (no nested-within-nested) -- the
    directory *is* the source of truth, no separate manifest file to desync. New Folder button, drag-and-
    drop a prefab between folders/root, folder Rename, and folder Delete (moves any remaining contents back
    to the root list, then removes the now-empty directory). Folders render above root-level items.
  - Filter/search box narrows the list client-side over the last real fetch (same pattern as the Entities
    tab's filter); folders with zero matches are hidden while filtering, with a "No matches." empty state.
  - "Create from selection" and "Load / Place" are visible but intentionally neutered to a "coming soon"
    toast -- see Known limitations for why.

## Known limitations / TODO

- **Create from selection / Load-Place are BLOCKED on a backend crash, not a frontend gap.**
  `serialize_selection` (+0xb0) hard-crashes DOOM -- confirmed via `webview_poc.log`: the last durable log
  line is immediately before the +0xb0 call, and neither the fault-shield VEH nor the frontend's own SEH
  guard catches anything, meaning it's a stack/heap fault inside the engine's prefab ctor/populate/
  serialize/render chain, not a clean access violation. The Qt Prefabs tab was always a "Coming soon" stub
  (see `sh_tabs.cpp`), so this backend path has *never* been exercised by either frontend -- this webview
  attempt was the first real call. The frontend code for both features is written and correct (native
  `poc_apply_create_prefab` with step-by-step logging, the JS create modal + overwrite guard, the planned
  `PasteInstantiate` + grab-mode flow for Load/Place mirroring the working Timeline-spawn precedent) but
  both buttons are neutered to a safe "coming soon" toast until the backend's prefab RVAs are fixed/
  re-derived (expected as part of the mentioned backend rewrite). Delete/Rename/Folders are unaffected --
  they're pure Win32 file ops through `resolve_prefab_path` (+0xc0) only, no `serialize_selection` involved.
- **Timelines / Timeline Editor** tab is not ported (the Qt frontend has it, and per `sh_timeline.cpp` even
  the OG Qt behavior has a faithfully-reproduced "Create New Timeline" brokenness). Deferred.
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
