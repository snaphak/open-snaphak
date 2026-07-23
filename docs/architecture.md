# Architecture

A contributor-orientation map of how the two DLLs fit together. For the *what it does*
feature list see [`capabilities.md`](capabilities.md); for the deliberately-faithful quirks see
[`fidelity.md`](fidelity.md).

## The two DLLs and the boundary between them

The clone is a **backend** (`XINPUT1_3.dll`, built from `src/backend/`) and a **frontend**
(`snapmap-plus-ui.dll`, built from `src/ui/`).

- The backend loads first. DOOM loads `XINPUT1_3.dll` at startup (it sits in the game root and
  forwards the real XInput exports through to System32). Once running, the **backend** does
  `LoadLibraryA(".\\snapmap-plus\\snapmap-plus-ui.dll")` and then `CreateThread(sh_ui_init, ...)` to
  bring the frontend window up on its own thread.
- The frontend never touches the engine directly. Every engine read or write the UI needs goes
  through a shared **interface object** that the backend creates and hands to the frontend's
  init thread.

This split is the version-portability story: all the build-specific engine offsets and
signature-resolved engine calls live **behind the interface, in the backend**. The frontend
holds no raw engine addresses, so a DOOM update only forces a re-derive on the backend side.

## The frontend: a WebView2 (HTML) window

The frontend (`src/ui/webview/snapmap_plus_ui_webview.cpp`) hosts the Snapmap+ UI as HTML/CSS/JS in a
Microsoft Edge **WebView2** control inside a plain Win32 window. Its `sh_ui_init` entry (export
ordinal 10, the same entry the backend calls) creates the window, brings up WebView2, loads the UI
(`mockup.html`, compiled into the DLL), wires the JS <-> native bridge, stores the backend **interface**
pointer, then enters the think-loop and never returns. The UI's structure — the tabs, the entity list,
the entity-state editor, the timeline editor, prefabs — lives in the HTML; the C++ host is a thin bridge
that turns JS messages into interface-slot calls and posts results back to the page. Full detail:
[`webview-ui.md`](webview-ui.md).

Theme selection is available before the first navigation: the host reads the registered `theme` setting,
adds `class="dark"` to the embedded document root when needed, and only lets the native window become
visible after a successful `NavigationCompleted`. A returning dark-theme user therefore never sees a
light or blank first frame.

## The 30 Hz manual think-loop

The frontend runs its own pump (the same shape as OG `FUN_180015c04`), once per frame at roughly 30 Hz,
under a loop mutex — draining the backend work-queue rather than relying on any UI toolkit's event loop:

```
lock(loop_mutex)
    (*(interface + 0x1a0))()      // drain the backend work-queue: run queued {handler, args}
    apply deferred UI-driven writes (snapshotted in the JS message callback)
unlock
pump the window's messages
Sleep(33ms)                       // ~30 Hz
```

This is **load-bearing**, not a stylistic choice. Heavy engine work (the SnapStack apply chain,
Save-to-Decl, timeline commits) is snapshotted off the re-entrant JS message callback and applied here,
on the think-loop thread; the manual pump plus the `+0x1a0` work-queue drain *are* the frontend's
main-thread execution point (a UI-thread or RPC-thread engine call deadlocks the engine's command-system
lock). Replicate the pump.

## The interface vtable (the matched-pair ABI)

The shared interface object is defined once, in `src/common/snapmap_plus_iface.h`, and **both DLLs
include that header** — it is a matched pair. The backend writes the vtable and fields; the
frontend reads them at the same offsets.

- The backend builds it (`operator_new(0x60)`), installs the vtable — the **77 original-faithful
  slots** (`+0x00..+0x260`) plus the **clone-extension slots** appended after them (`+0x268..+0x2B8`
  today: the atomic class+inherit apply, the class/inherit enumerators, the dev-layer query, the
  wire-edit generation counter, the synchronous `apply_sync`, the timeline inherit-normalize,
  push/clear-stack, and the generic configuration getter/setter) — initializes the mutex at `+0x08`,
  and hangs a sub-object off `+0x58` that holds the SnapStack subcommand map and the main-thread
  work-queue.
- **Extension slots are append-only**: a new capability gets the next slot after the current end;
  original-block offsets never move. This is also a real failure mode, not a formality — a frontend
  calling an extension slot that an older backend never installed would call through garbage. That is
  why `build.ps1` builds both DLLs from the same header in one pass by default (its `-BackendOnly`
  switch skips only the frontend — the safe direction, since an older frontend never reads past a
  newer backend's vtable), and why the frontend null-probes an extension slot (falling back or
  skipping the feature) rather than assuming it.
- The frontend calls vtable slots for everything it needs from the engine: entity
  count/validity, classname/inherit/displayname read and write, serialize/deserialize an
  entity, apply an edit (`+0xd0`), enqueue and drain the work-queue (`+0x90` / `+0x1a0`),
  register/unregister SnapStack subcommands (`+0x188` / `+0x190`), enumerate decls, manage the
  selection, show toasts (`+0x1b8`), and read/write registered settings as JSON fragments
  (`config_get_json` `+0x2B0` / `config_set_json` `+0x2B8`).

Because this vtable is the *clone's own* ABI — not a DOOM structure — it is self-consistent and
not DOOM-build-dependent. The only hardcoded offsets that cross the DLL line are these vtable
slot offsets and the `WIN[...]` field offsets. **They must stay pinned identically in both
DLLs**; the two are a matched set. The build-specific *engine* offsets sit behind the vtable in
the backend, where they are re-derived per build.

## Persistent configuration

The backend is the sole owner of `%LOCALAPPDATA%\snapmap-plus\config.json`; the installer does not
generate, parse, or replace it. `sh_config_init` runs after the common per-user directories are available
and creates this version-1 document when the file is absent:

```json
{
  "schema_version": 1,
  "settings": {
    "theme": "light"
  }
}
```

Deleting the file deliberately is therefore a clean reset: the next startup, or the next setting write
in a running session, recreates it. The one descriptor table in `src/backend/config.c` declares each
setting's key, JSON type, default, validator/normalizer, and backend/frontend read/write permissions.
Adding a setting means adding a descriptor and its behavior/tests; the wire contract remains generic.

Values cross the matched-pair ABI as complete UTF-8 JSON fragments. `config_get_json` at `+0x2B0`
supports a size query and reports status flags; `config_set_json` at `+0x2B8` validates the registered
key/value and returns rejected, persisted, or session-only. The WebView host exposes those calls to the
page as generic `configGet` / `configSet` messages carrying `valueJson`. This accommodates future
booleans, numbers, strings, arrays, and objects without growing the ABI once per setting.

The parser accepts an optional UTF-8 BOM, caps the file at 64 KiB, rejects malformed UTF-8, malformed
JSON, excessive nesting, and duplicate object keys, and requires the supported schema version. For a
supported document it repairs missing or invalid registered values to their defaults while preserving
unknown members under both the root and `settings`. A malformed, structurally invalid, or oversized file
is moved to a timestamped `config.<timestamp>[.<collision>].corrupt.json` backup and replaced with
defaults; the UI warns once for that startup. A document with a newer schema version is instead left
byte-for-byte untouched: the current process uses defaults and refuses to overwrite preferences it does
not understand.

Writes are serialized by an in-process lock and a local-session named mutex. A setter rereads the file
while holding that mutex so it does not discard an external writer's unknown values, writes and flushes a
same-directory temporary file, then atomically replaces `config.json`. Existing-file replacements use
paired temporary/rollback names; if a process stops in Windows' documented partial-replacement state,
the next startup recognizes the pair and restores the prior file before applying missing-file reset
semantics. Creation, read, write, flush, backup, replacement, or mutex failures leave the last good
on-disk file intact where possible and switch the affected value to session-only memory with a visible
warning. The two-DLL overlay and installer payload are unchanged; update/uninstall/reinstall preserve
this runtime-owned file.
