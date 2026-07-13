# Fidelity — the original's quirks, what the clone reproduces, and the one divergence

The clone was built to a **faithful-reproduction** bar: match the original SnapHak's observable
behavior first — including the ugly, mislabeled, and unfinished bits — and leave *fixing* what was
broken to a separate, later pass. That later pass happened, so several of
the original's quirks are now **fixed** in the shipped clone rather than reproduced. This document
records, per quirk, what the original did and what the **current** clone source actually does, so
the two never drift apart in a contributor's mind.

One behavior is a deliberate divergence from the start: the fault-shield (last section).

## Still faithful (reproduced on purpose)

### The Editor-Lua tab is empty
The original's 6th tab (`Editor Lua`, internal name `lua_scripts_page`) is an empty widget — no
children, no layout, no behavior; the Lua VM host behind it is dead code with no callers. The clone
reproduces it as the same empty tab and ships **no Lua runtime**. A real Lua editor is still future work.

### The manual 30 Hz think-loop
The frontend runs its own `processEvents` + `Sleep(33 ms)` pump rather than `QApplication::exec()`.
This is faithful to the original *and* load-bearing — the deferred main-thread apply path rides on
this pump and its work-queue drain (see [`architecture.md`](architecture.md)). Reproduced exactly,
not modernized.

### Save-to-Decl does no *full* class/inherit compatibility check
The Entity-State "Save to Decl" commits the edited classname/inherit/displayname into the entity in
memory and does **no full compatibility check** — matching the original (whose own guide warns
"mismatch → crash"). The clone *does* add a narrow guard — a provably-fatal
class+inherit pair is **refused** ("Save refused: incompatible class+inherit combination") rather
than written, and any access violation that still slips through is caught by the fault-shield
(below). So the faithful "no full check" stance is preserved, but the catastrophic case is fenced off.

### Create-from-selection requires hovering a selected entity
The engine's own `populate()` (the `+0xb0` serialize-selection body) refuses to run unless the
editor is currently hovering an entity that's part of the selection — it prints its own message,
`"Failed to create prefab: not hovering entity in selection."`, and returns failure rather than
populating the temp prefab. This was initially mistaken for a clone bug (see
[`backend-changes.md`](backend-changes.md) for the actual crash root-causes it got tangled up with)
but is a genuine, faithfully-carried-over engine requirement, confirmed once those crashes were
fixed. The webview UI checks for it up front (the hovered-id slot, `+0x198`) and surfaces an
accurate "hover over an entity first" message instead of a crash or a misleading generic one.

### ~~A freshly placed/reclassed Timeline needs a map save+reload before it accepts data~~ — RETRACTED 2026-07-13: was two clone bugs, not an engine limitation

**This entry is retracted.** It was documented (2026-07-08) as a genuine pre-existing engine/tool
limitation, but that conclusion was wrong. The "won't save until you save+reload the map or copy/paste
first" behavior was **two separate clone bugs**, both since fixed — a freshly placed *or* reclassed
Timeline now saves immediately in **both** frontends with no workaround (verified in-game 2026-07-13,
placement and reclass, including play/save/reload persistence):

1. **The crash half** (the reason copy/paste was branded "unsafe" and pinned on the engine) was our own
   **deferred-apply double-free**: decl-edit commits were *scheduled* onto the DOOM main thread (`+0xd0`)
   instead of committed inline, double-owning the decl-source block and freeing it twice on the next map
   teardown. Fixed by the `+0x290` synchronous inline apply — Qt first (2026-07-12), then webview's Save
   Timeline (2026-07-13). See [`backend-changes.md`](backend-changes.md) / [`qt-changes.md`](qt-changes.md).
2. **The "won't save" half, webview-specific,** was a JavaScript `typeof null === 'object'` bug in
   `tlBuildPatchedEntityJson`: a fresh Timeline serializes as `edit = NULL;` (`"edit":null`), the `null`
   slipped past a `typeof x !== 'object'` guard, and the next property assignment threw *uncaught* — so
   the whole Save silently aborted (no toast, nothing reached the backend). Fixed 2026-07-13 with explicit
   `=== null` checks. See [`webview-ui.md`](webview-ui.md).

The lesson kept here: the original tools were never re-tested in a way that *isolated* these two bugs, and
the "reproduces in the unmodified original SnapHak 2 Beta" claim conflated a real-but-separate engine
copy/paste behavior with the clone's own save failures. A cautionary example of a wrong "it's the engine,
not us" conclusion — the discipline that eventually cracked it was a step-by-step JS→native chain trace,
not more RE of the engine.

## Fixed (the original was wrong; the clone is now right)

### `bsb` no longer pops `MessageBoxA("FUCK","fuckedy")`
The original's `bsb` (bulk-set-bool) handler had a leftover debug `MessageBoxA("FUCK","fuckedy")` on
its re-resolve-mismatch path — a development artifact that shipped. A re-resolve mismatch *is* a real
signal (a property/value that didn't round-trip), so the clone keeps the signal but surfaces it via a
**clean non-modal toast** ("bsb: some entities skipped …") instead of an intrusive debug box
(`src/ui/snapstack.cpp`). *(Some nearby code comments still describe the faithful reproduction of the original —
the behavior is the fixed one.)*

### `filtcls` reports the correct field
The original's `sh filtcls` (filter the stack by **classname**) reused the `filtinh` toast string and
mislabeled its output `"… had inherit %s"`. The clone **corrects the label** — `filtcls` reports
`"had class %s"` and `filtinh` reports `"had inherit %s"` — so the toast names the field actually
filtered (`src/ui/snapstack.cpp`).

## The one sanctioned divergence — the fault-shield

The original installs two fault detours (on the engine's `Error` and `FatalError`) that each format a
message, pop a `MessageBoxA`, and call `TerminateProcess` — destroying the engine's own recoverable
error path and killing DOOM.

The clone replaces those two kill-detours with a resident **fault-shield**: a vectored exception
handler (`src/fault_shield/`, **compiled into the backend `XINPUT1_3.dll`**) that catches the access
violation in DOOM's frame code, **reverts the bad edit, and shows a toast** instead of terminating the
process. This is the single deliberate behavioral departure from the original — it makes a class of
in-editor crashes recoverable rather than fatal — and it is what makes the "no full compat check"
stance above safe.
