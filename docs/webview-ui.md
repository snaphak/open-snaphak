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
# from the repo root -- builds the backend (build/XINPUT1_3.dll) + the Qt-free frontend (build/webview/snaphakui.dll)
powershell -NoProfile -ExecutionPolicy Bypass -File build-webview.ps1
# assemble the lean overlay (2 files only: XINPUT1_3.dll + snaphak/snaphakui.dll -- NO Qt runtime)
powershell -NoProfile -ExecutionPolicy Bypass -File package-webview.ps1   # -> dist/
installer\snaphak.exe install --local dist --yes                          # deploy (DOOM must be closed)
```

The Qt and WebView frontends build to **separate** output paths (`build/qt/` vs `build/webview/`), so building
one no longer overwrites the other -- and `package-webview.ps1` assembles a lean overlay that ships **only**
the two clone DLLs (the WebView2 runtime is system-installed; no Qt DLLs, unlike the Qt overlay from
`package-qt.ps1`). Runtime log: `<DOOM>\snaphak_logs\webview_poc.log`.

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
| Prefabs list, detail pane, delete/rename, folders (create/rename/delete/move) | `resolve_prefab_path` +0xc0 only -- pure Win32 file/directory ops (`FindFirstFileA`, `DeleteFileA`, `MoveFileA`, `CreateDirectoryA`, `RemoveDirectoryA`) on the resolved path. No other engine slot involved, unaffected by the +0xb0 issues below. |
| Create from selection | `serialize_selection` +0xb0 |
| Load / Place | `apply_edit` kind=1 (mkcmd, the same path the Qt `mkcmd` command uses) -- stages into the paste slot only; the user presses Ctrl+V themselves. See the Changelog for why this is stage-only rather than fully automated. |
| Timelines list (dual-add `idTarget_Timeline` / `idEncounterManager`) | `get_classname_copy` +0x48 -- change-gated (see Changelog), not a fixed timer |
| Open a timeline (tabs + events) | `serialize_entity` +0xc8 -- the same slot Save-to-Decl and Push-to-stack already use, JSON-parsed client-side |
| Timeline event-arg dropdowns (decl / enum / per-entity asset lists) | `enum_decls_of_resclass` +0x110 -- the same shared slot for both decl-name and enum-member enumeration |
| Save Timeline (commit `componentTimeLine` / `encounterComponent`) | `apply_edit` kind=0 -- the same path Save-to-Decl already uses, id-targeted instead of paste-targeted; see the Changelog and [`fidelity.md`](fidelity.md) for a real limitation on when this accepts data |

Heavy engine writes (Save, Delete, Select-in-editor) are snapshotted in the JS message callback and
applied on the next think-loop frame under the loop mutex -- mirroring the Qt frontend's flag-word
dispatch, which keeps them off the re-entrant callback. That think-loop frame runs on the **frontend's
own UI/think-loop thread**, not DOOM's main thread -- the same thread where the inline class/inherit
apply-guard (`sh_iface_class_inherit_ok`) fails open, exactly as it does for the Qt frontend. The path
that actually runs on DOOM's main thread, guarded by `ExecuteCommandBuffer`, is the *scheduled* `+0xd0`
apply (`clone_bss_apply`) -- a different, deferred path, kept only as an old-backend fallback and for
prefab/mkcmd staging (see [`backend-changes.md`](backend-changes.md) and [`qt-changes.md`](qt-changes.md)
for why decl-edits must NOT go through it).

## Changelog

Newest first. Each dated entry covers one working session's worth of change; the undated **Baseline**
entry at the bottom is the original POC buildout, before this doc tracked dates per entry.

### 2026-07-13 -- Timeline parity with Qt: palette-inherit normalize, inline Save Timeline, and the fresh-save `typeof null` bug

A focused session bringing WebView's Timeline handling to full parity with Qt. Three real fixes, found by
methodical in-game testing (Qt as the validated reference) plus, for the last one, a step-by-step
JS→native chain trace. All verified in-game on both **palette-placed and reclassed** Timelines, including
play/save/reload persistence.

- **Palette-Timeline portable-inherit normalize now works in WebView** (it didn't before). A Timeline
  placed from the in-game palette carries the non-portable `snapmaps/editor_only/placeholder_target`
  inherit; Qt rewrote it to `snapmaps/unknown`, WebView did nothing. The rewrite was **ported into a shared
  backend slot (`+0x298`)** so both frontends run one implementation (see
  [`backend-changes.md`](backend-changes.md)); WebView calls it from its Timeline rescan. Porting it
  surfaced a latent **decl-source-blob-lag** bug (the Inherit box and the saved map kept showing the
  placeholder even after the raw field updated) — root-caused and fixed backend-side; see that doc.
- **Save Timeline commits inline (`+0x290`), matching Qt** — no more crash on the next play/save/reload.
  WebView's Save Timeline had been using the deferred `+0xd0` schedule, the exact deferred-apply
  double-free Qt fixed in 2026-07-12; migrated to `poc_apply_sync_seh`.
- **Fresh Timelines now save immediately, with no copy/paste or map save+reload first** — closing the last
  gap vs Qt. This had been mis-documented (in [`fidelity.md`](fidelity.md), now retracted) as a pre-existing
  *engine* limitation. It was actually a **JavaScript `typeof null === 'object'` bug** in
  `tlBuildPatchedEntityJson`: a freshly-placed/reclassed Timeline serializes as `edit = NULL;`
  (`"state":{"edit":null}`), and the `typeof x !== 'object'` guard let the `null` through; the next line
  (`edit[compKey] = …`) then threw *uncaught*, silently aborting the entire Save — no toast, nothing
  reaching the backend. Fixed with explicit `=== null` checks. A JS→native `diag` tracer (since removed)
  pinpointed it: the trace showed the save entering the correct branch and the re-serialize succeeding, but
  the line immediately after `tlBuildPatchedEntityJson` never executing — an uncaught throw inside it.
- The Entity-State panel also gained a **per-field dirty exception** so an authoritative external inherit
  correction (from the normalize above) can land on the untouched Inherit box even mid-edit of another
  field, instead of the whole-panel dirty guard freezing stale placeholder text.

> **Architectural follow-up (tracked):** the SnapStack command logic (`snapstack.cpp`) is still **Qt-only**,
> and each frontend independently picks inline vs deferred at every decl-edit call site — a footgun that
> caused the WebView Save Timeline regression above (one reverted line silently dropped it back onto the
> crash path). The durable fix is to **port SnapStack into the backend** as shared handlers with the extra
> ops WebView lacks, so both frontends share one commit path. See "Known limitations / TODO" and
> [`backend-changes.md`](backend-changes.md).

### 2026-07-12 -- Contributor follow-ups: path-safety gate, pinned WebView2 SDK, malloc null-check

A contributor reviewing the WebView frontend PR flagged three low-risk, non-blocking items (plus two
more covered in [`backend-changes.md`](backend-changes.md) and [`qt-changes.md`](qt-changes.md)):

- **Path-safety gate.** `resolve_prefab_path` (+0xc0, backend) is a plain string concat with no
  rejection of its own -- a prefab/folder name containing `..` or a path separator would resolve
  outside the `prefabs\` tree before ever reaching `fopen`/`DeleteFileA`/`MoveFileA`/`CreateDirectoryA`/
  `RemoveDirectoryA`. Only the JS side guarded this before. Added a native `poc_valid_name()` check
  (rejects `..`, `/`, `\`, `:`, empty, and >200 chars) in `snaphak_ui_webview.cpp`, wired into the two
  choke-point helpers every prefab/folder file op already funnels through (`poc_prefab_dir`,
  `poc_prefab_file_path`) plus the one direct caller (`poc_apply_create_prefab`). The Qt frontend had
  the identical gap (`iface_resolve_prefab_path` in `sh_tabs.cpp`) and got the equivalent gate,
  `sh_valid_prefab_name()`, the same day -- see `qt-changes.md`.
- **Pinned the WebView2 SDK version.** `build-webview.ps1` fetched NuGet's `index.json` and took
  `$idx.versions[-1]` -- literally whatever NuGet listed last, prerelease or not. This wasn't
  theoretical: the SDK actually cached on disk from an earlier run was `1.0.4071-prerelease`. Replaced
  with a hardcoded `$wvPinnedVersion = "1.0.4078.44"` (the newest *stable* release at the time), to be
  bumped deliberately going forward -- needed before wiring the webview build into CI.
- **Null-checked the entity-list malloc.** `g_ents`/`g_tls` (`snaphak_ui_init`) were `malloc`'d with no
  null-check before use. In practice a null `g_ents` was already non-fatal -- the one write site
  (`poc_collect`) sits inside a `__try`/`__except`, so a null-deref got silently caught as a fault and
  returned an empty list -- but that's an accidental safety net, not an intentional one, and it doesn't
  cover every read site. Now `snaphak_ui_init` checks both allocations explicitly, logs, and aborts init
  cleanly on failure instead of relying on SEH to paper over it.

Also fixed as part of the same follow-up round: the "deferred applies run on the main-thread execution
point" doc wording above (now reads correctly) and a `docs/backend-changes.md` confirmation, by decompile,
that the Save-to-Decl setters (decl/classname/inherit/displayname) can't overflow on long input -- see
that doc for the write-up.

### 2026-07-08 -- Timeline event-arg widgets, Save Timeline shipped, crash root-cause + workaround found

> **CORRECTION (2026-07-13):** the crash root-cause and "freshly-placed needs save+reload / it's a
> pre-existing engine limitation" conclusions in this entry were later **disproven** — see the 2026-07-13
> Changelog entry above and the retraction in [`fidelity.md`](fidelity.md). The crash was our own
> deferred-apply double-free (fixed by the `+0x290` inline commit), and the "silently stops partway through
> the save handler, no exception... never pinned down" postmortem below was a JavaScript `typeof null ===
> 'object'` throw on a `null` `state.edit` (fixed 2026-07-13). Fresh Timelines now save with no workaround.
> The event-arg widget work in this entry is unaffected and still current; the investigative narrative is
> kept as-is for history.

- **Every event-arg kind now gets a real editable widget**, not just read-only text: bool (checkbox),
  float/int/text (plain input), decl (a dropdown constrained to real engine decl names, via
  `enum_decls_of_resclass` +0x110), enum (same slot, member names instead of decl names -- one shared
  engine call for both), vec3/angles/color (a per-component field row, structured `{x,y,z}` object, not
  a space-separated string -- the engine's reader rejects the string form), and per-entity asset
  dropdowns (model/anim/tag lists scoped to the "Runs on" entity's resolved class -- an
  "exceed-the-OG" nicety, the original doesn't do this). Every widget keeps the `"<name> (<type>)"`
  label even once editable, matching the original's own `tl_arg_label` convention, so the field always
  documents the exact engine type it expects.
- **Save Timeline shipped.** Rebuilds `componentTimeLine`/`encounterComponent` from the live UI model,
  fresh-reserializes the target entity (not the stale open-time snapshot -- something else may have
  edited it, e.g. moved it in the 3D view, while the panel was open), patches in the rebuilt component,
  and commits via `apply_edit` kind=0 -- the same path Save-to-Decl already uses, id-targeted instead of
  paste-targeted. No new backend "kind" needed, as anticipated in the previous Known-limitations entry.
- **Two real correctness bugs found and fixed** while building the commit path, both by comparing
  against the original's own decompiled commit logic (`sh_timeline.cpp`'s `tl_build_event_json` /
  `tl_resolve_entity_ref`):
  - An event with no Time value was committing `"eventTime":0` instead of omitting the field entirely.
    The original only emits `eventTime` when the typed value parses as a clean non-negative integer
    (`FUN_180011a88`'s `QString::toUInt` gate); `parseTimeline` was collapsing an absent value to `0`
    on read, and the commit side re-emitted that `0` on every save. Fixed by preserving absent-ness
    through the round-trip.
  - The "Runs on" field commits its resolvable entity ref (or, typed free-text, the empty string
    fallback) *correctly resolved*, not whatever display text happened to be sitting in the box. The
    box shows `"<id>: <displayname>"` for a picked entity (matching the Entities-tab list convention),
    and typing into it wrote that *label* straight into the model; committing it verbatim produced an
    unresolvable ref (the engine interns event targets by exact name). New `tlResolveCommitRef` maps
    the stored string back to its canonical id before commit, mirroring the original's own
    `tl_resolve_entity_ref` (which re-reads the combo's *data*, not its displayed text).
- **Save Timeline's success/failure reporting made honest.** The failure path used to be silent (a
  disabled-but-clickable button doing nothing) or wrong (a "Could not open this timeline" message that
  didn't cover every failure shape). Opening now disables Save Timeline too when it fails (previously
  only Revert was disabled, leaving Save clickable-but-inert); a save failure now says plainly *"Save
  failed -- if this entity was just placed or reclassed, save the map and reload it, then try again"*
  -- see the root-cause finding below for why that's the actual fix in most cases.
- **The open timeline panel now silently auto-refreshes** on returning to the Timelines tab or
  regaining window focus (`tlMaybeAutoRefresh`), but *only* if there are no unsaved local edits. Without
  this, the panel could keep showing stale content indefinitely after something changed the entity
  outside the panel (a play-mode round-trip, another edit path) -- risking a later Save silently
  recommitting stale data over whatever actually happened. Gated on a lightweight local dirty flag
  (every real edit site now calls `tlMarkDirty()`) so an edit-in-progress is never silently clobbered.
- **Root-caused a hard crash / hang** reported after editing a Timeline, saving, playing, saving the
  map, and reloading -- an extensive investigation (log analysis, live read-only Ghidra attach/disassemble
  of the running `DOOMx64vk.exe`, and direct comparison against the genuine, unmodified original SnapHak
  2 Beta and v1.3.1 tools) that repeatedly disproved its own working hypotheses before landing on the
  real one:
  - Ruled out: JSON key-reordering in the frontend's patch step (the engine's own dump format is
    already codepoint-sorted, so this can't matter); the specific `componentTimeLine` content shape
    (a plain float-arg save round-tripped the engine's own save file byte-for-byte); `sh_target_any`
    wiring a source entity to the timeline (the identical crash reproduced with wiring never touched);
    and a suspected blank class/inherit commit (`ae_apply_one`'s own diagnostic logging showed
    class/inherit were *always* correct at commit time -- see the `backend-changes.md` entry for the
    real, narrower bug that diagnostic surfaced instead).
  - **The actual trigger is copy/paste.** Every crash this session traced back to a Timeline that had
    been copy/pasted at some point (the previous workaround for "won't save without it"). Live
    disassembly of the engine's native Ctrl+V handler (`PasteInstantiate`) shows it doing a large,
    repetitive field-by-field clone of the whole entity -- a plausible site for a subtle per-field
    copy bug that leaves the result looking completely normal (same visible data, opens fine, plays
    fine) while carrying something that only breaks much later, whenever the entity is next freshly
    rebuilt from a saved map file. This reproduced identically in the **genuine, unmodified original**
    SnapHak 2 Beta (froze hard on opening the Timeline Editor right after a copy/paste), confirming
    it's a pre-existing engine/tool limitation, not something the clone introduced.
  - **The fix that actually works: skip copy/paste, use save+reload instead.** A freshly
    placed/reclassed Timeline needs *something* to happen before Save Timeline will accept data --
    copy/paste was one way to get there (the wrong one); a plain native Save Map + Reload is another,
    and was confirmed, repeatedly, to fully unblock Save Timeline with no crash or corruption across
    many subsequent edit/save/reload cycles. Full writeup and the exact repro in
    [`fidelity.md`](fidelity.md#a-freshly-placedreclassed-timeline-needs-a-map-savereload-before-it-accepts-data).
  - One real, narrow bug **was** found and fixed along the way (`ae_apply_one` could commit an empty
    class/inherit if the engine's own deserialize choked) -- see
    [`backend-changes.md`](backend-changes.md#2026-07-08--apply_enginec-ae_apply_one-could-commit-an-empty-classinherit).
    It's a correctness improvement, not the fix for the crash above (which reproduces in the original
    tool regardless), and is kept.
- **Postmortem -- a whole investigative branch built, tested, and reverted.** Attempted to make the
  Save Timeline result honest at a deeper level: instead of reporting success the instant the apply
  was *scheduled* (which nearly always "succeeds," regardless of whether the engine's own later,
  asynchronous drain of the request actually commits it), a new backend poll slot
  (`savetl_result`, a generation-counter pattern mirroring the existing `wire_edit_generation`) let the
  frontend wait for and report the engine's *real*, post-drain answer. This worked in principle
  (confirmed via the backend's own log showing correct `applied 1/1` results), but extensive checkpoint
  diagnostics through the entire JS save path -- reproduced identically, and completed successfully, in
  an isolated browser preview against the same code -- showed that against the real game, execution
  silently stopped partway through the save handler on a fresh entity's first attempt, before reaching
  even a trivial, heavily-guarded diagnostic line, with no thrown exception, no hang symptom, and no
  timeout ever firing. The exact mechanism was never pinned down. Given the validated save+reload
  workaround above makes this a non-blocking issue in practice, the whole branch (the new vtable slot,
  the generation-counter tracking, every `[DIAG]` checkpoint) was reverted rather than shipped
  half-understood; only the actual correctness fixes above were kept. **Flagged as a genuinely open
  question for a future session**, not a settled root cause -- see Known limitations.

### 2026-07-07 -- Timelines tab: list, open, entity tabs, event rows, Runs-on entity-picker

- **List**: any `idTarget_Timeline` / `idEncounterManager` entity is dual-added into the Timelines list
  (the same "OG quirk" `sh_tabs.cpp populate_one_entity` does), labeled by displayName (id fallback).
  The classname read is **change-gated**, not run on a fixed timer: the cheap per-poll entity scan
  (id-string + hidden flag only, unchanged from before this feature) computes a signature every
  ~330 ms as it always has, and the Timeline-specific classname rescan only runs when that signature
  actually changes -- mirroring the two-tier discipline the Qt `sh_dispatch_flagword` already uses
  (a cheap always-on check gating a rarer expensive one), not the naive "call it every poll forever"
  first attempt that broke the Entities list (see the postmortem below).
- **Open**: click a timeline -> `serialize_entity` (+0xc8) -> the raw JSON is shipped to the page ->
  `JSON.parse` + a walk of `entityDef.state.edit.componentTimeLine` / `.encounterComponent` ->
  `entityEvents.item[N]` becomes one tab per driven entity, `events.item[N]` becomes that tab's event
  rows. Mirrors `tl_collect_from_decl` (`sh_timeline.cpp`), but the parsing lives in JS against the
  same valid-JSON bytes Qt parses with `QJsonDocument` -- no hand-rolled C JSON parser needed.
- **Tabs**: labeled `Item N` (0-indexed, matching the decl's own `item[0]`/`item[1]`/... keys exactly --
  confirmed via a live, read-only Ghidra decompile of the original `snaphakui.dll`'s real tab-title code,
  which is the literal string `"item[%1]"`; ours drops the brackets for readability but keeps the
  0-indexing). A `+` tab appends a new blank entity-event-list; a small `x` on each tab removes it.
- **Events**: each row is a compact top line (Time + eventDef + a delete `x`) with every parameter
  stacked on its own line underneath -- also confirmed to match the OG's real structure (its per-row
  widget lands each arg in a `QFormLayout` *below* the eventDef combo, not beside it, per a live
  decompile of the row-construction chain). An "Add Eventcall" button per tab appends a blank event.
- **"Runs on" entity-picker**: an editable, free-text combobox backed by the live entity list --
  reimplemented from scratch after live-decompiling the OG's real composite widget (`FUN_180008180`,
  behavior-reference only, zero OG bytes copied) to see what was actually there: an entity combo, a
  "Use Display Names" checkbox, and a "Use ingame selection" button. The clone diverges deliberately:
  the checkbox is **dropped** -- the field always shows `<id>: <displayname>` together (matching the
  Entities-tab list convention), since the checkbox hid the id (which carries the module path and
  disambiguates same-named entities) and didn't persist its state across tab/timeline switches. The
  button is kept, renamed **"Use current selection"** (same idea, clearer wording); it's a visual
  placeholder for now (see Known limitations). Typing anything is accepted verbatim (`player1`, or any
  id not in the known list) -- there is no validation. Opening the dropdown (arrow click or focusing the
  text) always shows the *full* entity list regardless of what's already typed; only actually typing
  narrows it -- an early version conflated "has a value" with "is being filtered," so re-opening an
  already-picked field self-filtered down to just itself.
- **Revert** button: re-fetches the open timeline fresh (the same `openTimeline` round-trip a list click
  does), discarding every local edit. This is deliberately the *only* "undo" -- nothing in this tab
  persists to the actual decl until Save exists (see Known limitations), so any add/delete/edit is 100%
  session-local and safe to experiment with; Revert (or just re-clicking the timeline) always recovers
  the real, untouched data.
- **Postmortem -- three failed attempts before the list worked**: the very first version called
  `get_classname_copy` for every entity on every ~330 ms poll, unconditionally -- a much higher-frequency
  engine touch than the OG ever does, and it broke the Entities list on any map with a Timeline on it
  (list stayed empty, immune to Refresh, survived even a full DOOM restart). A live-debug session
  (the project's established read-only attach/read/detach method) chased this as an engine/threading
  problem and **fully exonerated the engine call** -- a direct memory read of the
  Timeline entity's classname chain (`entity->+0x158->+0x60`) came back clean both times. The actual bug
  was much simpler: a **missing closing quote** in the native JSON builder (`poc_emit_list`'s timelines
  array emitted `"name":"X}` instead of `"name":"X"}`), which made `PostWebMessageAsJson` silently
  reject the *entire* message whenever a Timeline was present -- explaining every symptom (empty list,
  Refresh-immune, survives a restart) without any engine involvement at all. Fixed with the one missing
  character; the change-gated rescan design (above) was kept anyway since it's still strictly better
  engine-touch discipline than the original attempt, even though it wasn't the actual root cause.

### 2026-07-06 -- Prefab create-from-selection crash fixes + Load/Place wired (stage-only)

- **Create from selection: two crash bugs, both fixed.** `serialize_selection` (+0xb0) used to hard-crash
  DOOM outright (an undersized `PREFAB_TEMP_SIZE` stack buffer), then, once that was fixed, crash
  intermittently inside the engine's `populate()` (a missing 3rd call argument). Full root-cause writeup:
  [`backend-changes.md`](backend-changes.md#2026-07-06--apply_enginec-prefab-create-from-selection-crashes).
  Fixing the second bug also surfaced a genuine, non-bug engine requirement: Create-from-selection needs
  the editor to be hovering a selected entity (see [`fidelity.md`](fidelity.md)). The webview UI now
  checks the hovered-id slot (+0x198) up front before attempting create, so it can show an accurate
  "hover over an entity first" result instead of the old misleading "nothing selected" toast or a crash
  -- the Create modal also has an inline tip to that effect.
- **Load / Place wired, stage-only.** `apply_edit` kind=1 (the same mkcmd path as the Qt frontend) stages
  the prefab into the paste slot, plus an auto-`clear_selection` first; the user presses Ctrl+V
  themselves to place it. Two attempts at automating that last step (a direct `PasteInstantiate` call,
  then a synthesized native Ctrl+V) each surfaced a different real side effect in DOOM -- a crash on the
  next Play/Editor transition, then an unwanted ESC-menu popup from the OS-level focus switch. Given
  three different failure modes across three attempts, this is deliberately left as stage-and-prompt
  rather than continuing to chase automation; see [`backend-changes.md`](backend-changes.md) for the
  full story. Delete/Rename/Folders are unaffected by any of the prefab crash history -- they're pure
  Win32 file ops through `resolve_prefab_path` (+0xc0) only, no `serialize_selection` or `populate()`
  involved.
- A silent-truncation bug in the shared apply pipeline (`APPLY_TEXT_CAP`, hit while stress-testing large
  prefabs through Load/Place) was also found and fixed this session -- backend-only, full writeup in
  [`backend-changes.md`](backend-changes.md).

### Baseline -- initial POC buildout (undated)

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
  - "Create from selection" works end-to-end. "Load / Place" reads the prefab file's raw JSON, clears the
    current editor selection, and stages it into `editor+0x209a8` (a plain `kind=1`/mkcmd apply item,
    identical to the pre-existing Qt `mkcmd` path) -- then prompts the user to press Ctrl+V themselves in
    the 3D view to actually place it. This is deliberately stage-only, matching the *original* SnapHak's
    own workflow exactly (confirmed by decompiling `snaphakui.dll`/`XINPUT1_3.dll` -- a prefab
    double-click there only ever stages, never auto-places either). Two earlier attempts at automating
    the placement step (calling `PasteInstantiate` directly, then synthesizing a native Ctrl+V ourselves)
    each surfaced a different real side effect -- see [`backend-changes.md`](backend-changes.md) for the
    full story. Available from both the "Load / Place" button and the prefab list's right-click menu. The
    generic engine "SnapStack: ..." toast is suppressed for this op specifically (`ae_toast_result` skips
    it for `op == "load-prefab"`) since the webview's own toast already tells the user what to do next;
    the Qt `mkcmd` command still gets the engine toast, since it has no toast of its own.
  - Live "Create from selection (N)" button count and the create modal's "From N selected entities" text
    both track the real editor selection continuously (two separate display bugs fixed: the count used to
    silently cap at 64 regardless of the real selection size, and the modal text never updated at all --
    it was stuck on its original static placeholder).

## Known limitations / TODO

Genuinely open items only -- fixed bugs and completed work live in the Changelog above, not here.

- **~~Save Timeline only works after a map save+reload~~ / ~~open question: zero feedback on a fresh
  entity~~ — both RESOLVED 2026-07-13.** These two former limitations were the same bug and are fixed. The
  "silently halts partway through the JS save handler, no exception, no hang, yet completes fine in the
  browser preview" mystery was an **uncaught `TypeError`** from `tlBuildPatchedEntityJson`: a fresh
  Timeline's `"edit":null` slipped past a `typeof` guard and the next property assignment threw (the browser
  preview never reproduced it because its sample data has a real `edit` object, never `null`). Plus the
  crash half was our deferred-apply bug, not the engine. Fresh placement *and* reclass now save immediately,
  no workaround, verified in-game. See the 2026-07-13 Changelog entry above and the retraction in
  [`fidelity.md`](fidelity.md).
- **"Use current selection"** (the Timeline "Runs on" picker's button) is a visual placeholder; needs a
  new native round-trip to read the live 3D-editor selection.
- **"Create New Timeline" stays disabled.** `sh_timeline.h` (the Qt port) documents that a clone-side
  create path is impossible (both a from-scratch spawn and a reclass-morph corrupted the map in that RE
  work) -- but a live decompile of the OG's `retranslateUi` this session found a real
  `"Create New Timeline"` button and string in the *original* binary, which contradicts that assumption.
  Not re-investigated yet; flagging it here so the disabled state doesn't get treated as settled.
- **Push to stack 0** is a stub, and more broadly **SnapStack must be ported to the backend** (TODO). The
  SnapStack subsystem (`snapstack.cpp`) is entirely Qt-bound: its command handlers (acctargets/accl/
  bss/bsi/bsf/bsb/bse/mkcmd/push-to-stack/…) live in the Qt frontend, so WebView has none of them. Beyond
  the missing features, the *architecture* is the real issue: **each frontend independently chooses inline
  (`+0x290`) vs deferred (`+0xd0`) at every decl-edit call site**, which is a latent-crash footgun — it bit
  this cycle when WebView's Save Timeline silently regressed to the deferred double-free path after one line
  was reverted (see the 2026-07-13 Changelog entry). The durable fix: **port the SnapStack command handlers
  down into the backend** (`XINPUT1_3.dll`) as shared handlers — adding the ops WebView currently lacks —
  so both frontends send high-level command intents through one commit path and can't diverge on the
  inline/deferred choice. A cheaper interim hardening (not a substitute): make the backend's `+0xd0`
  schedule commit `kind=0` decl-edits inline regardless, only truly deferring `kind=1` mkcmd staging, so no
  frontend call site can pick the crashing path. Full rationale in
  [`backend-changes.md`](backend-changes.md) (2026-07-13 entry).
- Editing an entity's decl does not re-present it live in the editor (a decl commit updates the definition
  but not the already-spawned instance -- same as Save-to-Decl in the Qt UI). A live in-editor re-present
  via the engine's per-entity refresh is a possible future experiment.
- Undo covers only unsaved edits (the Revert button + the textarea's native undo); undoing a committed Save
  is not implemented.
- Not wired into CI (CI builds the Qt path via `build.ps1`).

## Preview mode

Open `src/ui/webview/mockup.html` directly in a browser to see and click through the UI with fake data --
useful for iterating on layout/behavior without building or deploying. This preview branch only runs when
there is no WebView2 host, so it has no effect inside DOOM.
