# Backend changes — engine-call bugfix log

A running log of correctness bugs found and fixed in `src/backend/` — the shared engine-call layer
used by **both** the Qt frontend and the [experimental webview UI](webview-ui.md). These are cases
where our own reimplementation was wrong, not the original SnapHak's behavior; a divergence from
(or faithful reproduction of) the *original's* behavior belongs in [`fidelity.md`](fidelity.md)
instead. Entries are chronological, newest first.

## 2026-07-12 — SnapStack decl-edits double-freed the committed decl-source block; fixed with a synchronous inline apply (`+0x290`)

The long-standing SnapStack crash — run `acctargets` (or `bss`/`bsi`/`bsf`/`bsb`/`bse`), hit **Play**,
then trigger **any** map teardown (reload the same map, **New Map**, or load an unrelated map) and DOOM
dies with `"Memory corruption before block!"` — an access violation at
`DOOM+0x1ab32ee ← 0x19fd162` (`IdStrDtor`) `← 0x17ad00a` (decl-source teardown), `load_state=3`. No save
required; the corruption is in live memory, not the map file. See [`qt-changes.md`](qt-changes.md) for the
full investigation narrative — this entry documents the backend mechanism and the fix.

**Root cause — the deferral, not the commit.** Live reverse-engineering of OG SnapHak (Beta 2) proved our
commit body `ae_apply_one` is byte-for-byte OG's `+0xd0` commit (`FUN_180004b80`), and our committed
decl-source blob is byte-identical to OG's. The *only* difference was **when** it runs. OG's SnapStack
handlers commit **inline, synchronously, on the UI/think-loop thread** (the `+0x1a0` work-queue drain).
Our clone ran the command handler on that same UI thread but **deferred** the heavy commit to the DOOM
main thread via `clone_bss_apply` at `ExecuteCommandBuffer` (the old "FIX B", added on the belief that the
structured deserialize AVs off the main thread). That split one atomic operation across two threads and
two frames, and left the freshly-committed decl-source block **double-owned** — so the next map teardown
freed it twice → heap-header corruption → the fault above.

The deferral's premise was stale: our `+0xc8` serialize already runs successfully on the UI thread, and
the reflection context it needs is a process-global singleton (engine `0x17f7030` → vtable `+0x80`),
reachable from any thread — OG proves it by committing on exactly that thread.

**Fix — a synchronous inline apply slot, `+0x290` (`apply_sync`).** Added to the matched-pair vtable ABI
(`snaphak_iface.h`/`.c`, `apply_engine.c` `slot_apply_sync`, exported via `sh_apply_engine_get_slots` and
folded in by `iface_engine.c`). It runs the same per-item batch as the `clone_bss_apply` drain
(kind 0 = decl edit / 1 = mkcmd / 3 = target-write) but **inline on the calling UI thread**, so serialize +
commit are atomic and the committed block has a single clean owner — OG's exact flow. Because the commit
is now inline, callers pass their own item text with no deep-copy/pending store. Each `ae_apply_one` stays
SEH-guarded, so an off-main reflect gap (if it ever occurred) degrades to `applied 0`, never a crash. The
frontend routes all decl-edit ops through it (see [`qt-changes.md`](qt-changes.md)); the deferred `+0xd0`
schedule is kept only as a fallback for an old backend without `+0x290`, and for `mkcmd`/prefab-paste
(`kind=1`), which never crashed and is left deferred. A backend-log marker
`C2 SYNC apply: N item(s) INLINE on this thread (op)` confirms the inline path at runtime.

This supersedes the earlier "JSON round-trip vs in-memory node-tree edit" theory: our round-trip matched
OG's, so it was never the problem. **TODO:** migrate Timeline Save (its own `|0x80` path in `sh_timeline.cpp`)
to `+0x290` as well, and quiet the `AE_APPLY_DIAG` / `AE_DESER_DIAG` / `+0x40 rebuild` / `C2 SYNC apply`
diagnostics before release.

## 2026-07-08 — `apply_engine.c`: `ae_apply_one` could commit an empty class/inherit

Found while root-causing a hard crash-to-desktop / hang on returning to the SnapMap editor after
editing a Timeline, saving, and reloading the map (full investigation, including why the crash
turned out **not** to be caused by this bug, in [`webview-ui.md`](webview-ui.md)).

`ae_apply_one` (the shared `kind=0` commit body behind Save-to-Decl, Save Timeline, and
`wire-target`) deserializes the caller's full patched entity JSON onto a temp def, then copies that
temp def's normalized class/inherit/source back onto the live entity. The class/inherit copy only
null-checked the pointer (`if (clsPtr) ...`), not the string it pointed to — if the engine's own
`StructDeserialize` ever populated the temp def with a **non-null pointer to an empty string** (a
real, observed case, though not the one that turned out to matter for the Timeline crash — see
`webview-ui.md`), the guard let it through and committed `""` onto the live entity's classname or
inherit. A blank class/inherit is never valid; the next full map load fails with the engine's own
`"No class specified"` / `"Couldn't find map entity in entity palette '' inherit = "` and the entity
is unrecoverable.

**Fix:** the guard now also checks the first byte of the string
(`if (clsPtr && *(const char *)clsPtr) ...`, same for inherit) — an empty result is treated the same
as a null one and simply skipped (keep the live value), rather than committed. This degrades a choked
deserialize into "the edit didn't apply, entity intact" instead of "entity destroyed." Universal
across all three `kind=0` callers, not Timeline-specific — a blank class/inherit is never the right
outcome for any of them. Also confirmed to correctly *preserve* a non-standard inherit (e.g.
`snapmaps/editor_only/placeholder_target`) rather than requiring or defaulting to anything, unlike
the original's own Timeline-commit path, which hardcodes `inherit = "snapmaps/unknown"` on every
save (see `fidelity.md`) — the clone's keep-live behavior is strictly safer here.

## 2026-07-06 — `apply_engine.c`: `APPLY_TEXT_CAP` silently truncated large prefabs on Load/Place

Once Load/Place (staging via `kind=1`/mkcmd) was wired up and exercised against real prefab files,
some staged cleanly and some silently failed (backend log: `applied 0/1`, no crash, no fault-shield
entry -- nothing visibly wrong, the prefab just never showed up in the paste slot).

Root cause: `APPLY_TEXT_CAP` (a sanity ceiling on the JSON text carried in a scheduled apply item) was
`256 * 1024` -- sized for small per-entity edits, the only thing this pipeline used to carry. Real
prefab files can run well past that. `slot_schedule_apply`'s batch deep-copy silently truncated
anything over the cap with no error at all, so an oversized prefab got cut off mid-JSON before it ever
reached the deserializer, which then failed to lex the truncated text.

Diagnosed with a step-by-step trace added to `ae_deserialize_to_obj` (gated behind `AE_DESER_DIAG_ON`,
mirroring the existing `AE_SER_DIAG` pattern on the serialize side) -- it pinpointed the exact failing
prefab's text arriving already truncated to one byte under the old cap, confirming the truncation
happened upstream in scheduling, not in the deserialize call itself.

**Fix:** raised `APPLY_TEXT_CAP` to 4 MB, matching the scratch buffer already used elsewhere for
prefab content. Retested against every prefab that had been failing (all `applied 1/1` now) and
live-tested up to 2 MB with no issue -- comfortable headroom over anything hit so far. This cap is also
used as a sanity bound when reading a serialized result back out (`ae_read_idstr`), so raising it
benefits the Create-from-selection direction too, not just Load/Place.

## 2026-07-06 — `apply_engine.c`: prefab create-from-selection crashes

Two independent bugs, both in the `+0xb0` serialize-selection path (`slot_serialize_selection`),
found back to back while root-causing a hard DOOM crash on every "Create from selection" call. This
backend path had never been exercised by either frontend before the webview UI's first real call
into it — the Qt Prefabs tab has always been a "Coming soon" stub (see `sh_tabs.cpp`).

### 1. `PREFAB_TEMP_SIZE` undersized — stack buffer overflow

`PREFAB_TEMP_SIZE` (the scratch buffer for the temp `idSnapEntityPrefab`) was `0x220`, sized off the
original's `local_6d8` frame slot. Too small: the ctor at `+0x54d0a0` writes its own fields up to
`~+0x118`, then makes a small forward call into a second, larger ctor that keeps writing fields past
`+0x590` — the real object needs at least `~0x590+` bytes, about 2.6x the old allocation.

Every call overflowed the stack buffer. Because the overwritten bytes land on valid, mapped stack
memory (just not memory meant for this object), it was never a clean access violation, so neither
the fault-shield VEH nor our own SEH guard ever caught it — that's why it crashed DOOM outright
instead of failing gracefully.

**Fix:** bumped `PREFAB_TEMP_SIZE` to `0x2000` for comfortable headroom over the confirmed-required
size.

### 2. `PrefabPopulate` called with 2 args instead of 3 — uninitialized out-param

With the overflow fixed, a second, intermittent crash remained: two distinct locations inside the
engine's `populate()` function (base `+0x54e410`), both a `c0000005 ACCESS_VIOLATION` writing to
near-null address `0x10`, both caught cleanly by the fault-shield (recovers by exiting the editor to
the menu, not crashing DOOM outright): `+0x54e6e7` (`populate()+0x2D7`) and `+0x54f2a1`
(`populate()+0xE91`).

Root cause: `populate()` is actually a **3-argument** function — the 3rd (`R8`, per the Windows x64
calling convention) is an out `int*` status/reason code the engine writes through — but
`apply_engine.c`'s `prefab_populate_fn` typedef and call site only ever supplied 2 args. `R8` held
whatever was left over from the prior call in the sequence (garbage/unmapped, e.g. observed `0x10`),
so the engine's own write through it faulted.

This initially looked hover-state-dependent (an early, wrong theory pinned it on selection
size/complexity instead). That correlation was real but coincidental to the crash: whatever
DOOM's own hover-detection code happens to leave sitting in `R8` beforehand, not anything
`populate()` itself was reading for hover/placement purposes.

**Fix:** added the missing `int *outStatus` parameter to the `prefab_populate_fn` typedef and the
call site, passing a real local variable's address so the write always lands somewhere harmless.

### A confirmed, separate finding: the hover requirement is real

Fixing bug 2 surfaced the engine's *own* validation, previously masked by the crash: status code `2`
means "not hovering entity in selection," and the engine prints that exact message itself
(`"Failed to create prefab: not hovering entity in selection."`) before returning. Create-from-selection
genuinely requires hovering a selected entity in the 3D view — see
[`fidelity.md`](fidelity.md#create-from-selection-requires-hovering-a-selected-entity) — this is not
a clone bug. The webview UI now checks the hovered-id slot (`+0x198`) up front (see
[`webview-ui.md`](webview-ui.md)) instead of relying on the engine's status code, so it can show an
accurate message instead of a generic "nothing selected" one.

## 2026-07-06 — `apply_engine.c`: Load/Place, calling `PasteInstantiate` directly was wrong

First implementation of `kind=2` (Load/Place) staged the prefab (`ae_mkcmd_one`, same as `kind=1`)
then called `PasteInstantiate` (`+0x54f950`) directly, mirroring how `PrefabPopulate`/`PrefabCtor`
are called elsewhere. This looked reasonable but was wrong in a way the crash didn't immediately
reveal: the placed entity came out selected but not draggable, and returning to the editor from Play
afterward crashed DOOM hard (`ACCESS_VIOLATION` reading `0x0` at `+0x5a516b`, a large call stack
through several engine frames).

Root cause: the engine's real native Ctrl+V handler (`+0xce1810` in `DOOMx64vk.exe`) calls
`PasteInstantiate`, then calls a *second* function (`+0xcf35e0`) that sets grab-tool state (bit
manipulation at `obj+0x2d0`/`+0x2d1`, `obj+0x1ac = 4`, `obj+0xbb8 = 1`) on some object passed into the
handler as its 2nd argument. Calling `PasteInstantiate` alone skips that second call entirely, leaving
the engine in whatever inconsistent state that omission causes — undraggable placement now, a crash
on world transition later.

Identifying *which* object needs those writes turned out to be a dead end via live debugging:
interactive breakpoint-based verification (attach, set a breakpoint, `debugger_continue`, wait for a
user action to hit it) is **confirmed broken** in this environment — it reliably freezes DOOM's input
even though the debugger reports the process as running, regardless of what triggers the breakpoint
(tried 4 times, different breakpoints, different triggers). A static hypothesis (the object is the
same selection object behind `hovered_id`) didn't hold up against a read-only memory check either —
the candidate field region held array-like data, not simple flags.

**Fix — sidestep the problem entirely:** confirmed against the *original* `snaphakui.dll` +
`XINPUT1_3.dll` (static Ghidra decompilation, no live debugging needed) that neither ever calls
`PasteInstantiate` directly either. A prefab double-click in the original UI
(`snaphakui.dll!FUN_180017538` → `FUN_180013878`) only reads the file and stores its text; the actual
"deserialize into `editor+0x209a8`" step is a separate function (`XINPUT1_3.dll!FUN_180006bf0` →
`FUN_1800094f0`) — the same operation as our own `ae_mkcmd_one`. Placement always happens through a
real, native Ctrl+V; nothing in either original DLL calls `PasteInstantiate` at all. So `kind=2` no
longer calls it either — it just stages (`ae_mkcmd_one`, identical to `kind=1`), and something else
finds the game's own top-level window (`EnumWindows`, same-process, skipping our own
`"SnapHakStudioWebView"` companion window), brings it to the foreground, and synthesizes a real Ctrl+V
via `SendInput`, letting the engine's own already-correct native handler do the actual instantiate +
grab-tool setup, exactly as it would for a real user paste — no need to ever identify the mystery
object at all.

**First attempt at the synthetic-input step was on the wrong thread.** Put the `SetForegroundWindow` +
`SendInput` call directly inside `ae_mkcmd_and_place`, which runs on DOOM's own main thread (inside the
engine's `clone_bss_apply` command-buffer drain). Staging succeeded (confirmed via the toast, "applied
1/1") but no paste ever happened, even though the user could then paste manually with Ctrl+V — proving
the staged data was valid the whole time. `SetForegroundWindow` requests a focus change but doesn't
apply it synchronously; the window that needs it processes the change on its own message pump. Calling
it from DOOM's main thread and then immediately (same thread, no yield) calling `SendInput` fires
before that thread ever gets to pump the message that would complete the focus switch — so the
keystroke landed nowhere. **Fix attempt 2:** moved the whole window-find + focus + `SendInput`
sequence out of `apply_engine.c` entirely and into the webview UI's own think-loop
(`poc_synthesize_native_paste` in `snaphak_ui_webview.cpp`), which has its own message pump and isn't
blocking DOOM's simulation. Also deliberately delayed ~6 loop iterations (~200ms) after a successful
schedule, plus a 50ms sleep between the focus request and the keystroke, so the engine's command-buffer
drain has actually staged the prefab, and the focus change has actually landed, before Ctrl+V fires.

**That fix moved the keystroke correctly but surfaced a third side effect.** Staging still succeeded,
and this time the focus switch itself visibly worked -- but forcing `SetForegroundWindow` on DOOM's
window made the game pop its own ESC/pause menu, almost certainly a "regained focus after being
alt-tabbed away" safety behavior (our webview panel is a separate top-level window, so from the game's
perspective, forcing focus back to it looks exactly like the player alt-tabbing back in). The
synthesized Ctrl+V then likely lands on that menu instead of the 3D view, so nothing pastes -- but
again, the staged data is untouched and a manual Ctrl+V still works once the menu is dismissed.

**Decision: stop automating this step.** Three attempts, three different side effects (a hard crash, a
silent no-op, an unwanted pause menu), and each only surfaced by actually testing in DOOM. Given how
consistently fragile this specific corner of the engine has been, `kind=2` was removed entirely --
Load/Place is back to plain `kind=1` (identical to the Qt `mkcmd` path): stage the prefab, then tell the
user to press Ctrl+V themselves. This matches the *original* SnapHak's own actual workflow exactly (see
the double-click investigation above) and needs zero window-focus tricks, zero risk of the side effects
above, and zero remaining need to ever identify the mystery grab-tool object.
