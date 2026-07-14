# Backend changes — engine-call bugfix log

A running log of correctness bugs found and fixed in `src/backend/` — the shared engine-call layer
used by **both** the Qt frontend and the [experimental webview UI](webview-ui.md). These are cases
where our own reimplementation was wrong, not the original SnapHak's behavior; a divergence from
(or faithful reproduction of) the *original's* behavior belongs in [`fidelity.md`](fidelity.md)
instead. Entries are chronological, newest first.

## 2026-07-13 — SnapStack ported into the shared backend (`snapstack.c` + `json_patch.c`); `json_patch` empty-`edit` fix; new store-management commands

**What & why.** The entire SnapStack subsystem — the stack-of-stacks + named-group stores and all 20 `sh`
subcommand handlers (`psel`/`popsel`/`phov`/`pr`/`pg`/`pop2g`/`cstk`/`filtinh`/`filtcls`/`bss`/`bsi`/`bsf`/
`bsb`/`bse`/`bsin`/`bscls`/`bsincls`/`accl`/`acctargets`/`mkcmd`) — lived only in the Qt frontend
(`src/ui/snapstack.cpp`), registered at Qt startup. The Qt-free WebView host never called that registrar, so
`sh psel`/`sh acctargets`/etc. simply didn't exist under WebView. Ported the stores + all 20 handlers to
pure C in the shared backend (`src/backend/snapstack.c`), backed by a purpose-built dotted-path JSON mutator
(`json_patch.c`) that generalizes `apply_engine.c`'s existing raw-splice technique instead of pulling in a
JSON library (the backend deliberately carries none). Registered **additively** from `ui_bridge.c` before
any frontend loads: the cmd-map overwrites on duplicate name, so under a Qt build Qt's own registrar still
runs afterward and wins for all 20 names — Qt's behavior is unchanged; only a frontend with no registrar of
its own (WebView) ends up running the backend copy. This kills the inline-vs-deferred apply footgun for
WebView (each frontend previously chose the apply path at its own call sites; WebView's Save-Timeline
regressed once from exactly that) — the shared handlers commit through one path.

**Second attempt — the first port (commit `e7ee129`, 2026-07-09) was reset out.** That attempt predated two
fixes since established on the Qt/WebView side and carried both bugs: (1) every apply-op scheduled via the
**deferred `+0xd0`** path — the same split-commit-across-threads pattern that double-frees the decl-source
block on map teardown (see the 2026-07-12 deferred-apply writeup below); (2) several persistent `static`
scratch buffers (256 KB × multiple call sites, a 1 MB idstr table) — the same BSS-footprint pattern that
caused the controller-freelook regression on the `+0x298` slot. This re-port fixes both: every `kind=0`
decl-edit op now tries the **synchronous `+0x290` `apply_sync`** first (OG-faithful inline commit), falling
back to the deferred schedule only on an old backend without the slot; `mkcmd` (`kind=1` prefab paste, a
different operation that never crashed) stays on the deferred path, matching Qt's convention. All scratch
buffers are heap-allocated transiently per call (`malloc`/`free`), never `static`/BSS.

**The bug that broke every decl-edit under WebView — `json_patch` mishandled a non-object `edit`.**
Live-tested `sh bss`/`sh acctargets` both returned `applied 0/1` with no decl change. The engine's own
deserialize `Lexer` rejected the patched text. Root cause (found by dumping the exact bytes fed to the
lexer): an entity you haven't hand-edited serializes its `state.edit` **overrides** block as `null` (or `{}`)
via the `+0xc8` serialize — everything the editor *shows* on it is **inherited** from its decl, not an
explicit override. When `json_patch` had to create a path segment (`targets`, `renderModelInfo.model`, …)
under a non-object `edit`, it spliced a bare `"key":value` **member** where a braced **object value** was
required, producing invalid `"edit":"targets":{…}`. Qt never hit this because QJson silently normalizes a
null `edit` into `{}` before patching — the exact JS-`typeof null` / null-safety class of gap, one layer
down in C. Fixed in `json_patch.c`: (a) the missing-path branches in `json_walk_set` / `json_walk_upsert_
reflist` now wrap the built chain in braces (`{…}`) so it's a valid object value for both the splice-into-a-
non-object and insert-new-member cases; (b) `json_insert_member`'s empty-object test corrected from
`p == obj_close` (always false — `obj_close` points *past* the `}`) to `*p == '}'`, which had been appending
a trailing comma (`{"k":v,}`) on an empty `{}` edit. Verified offline against a captured real entity plus
null/empty/missing-`edit` variants, then live: `bss`/`acctargets` apply `1/1`.

**`+0x2A0` `push_to_stack` slot.** A new vtable ext slot (ext 7) lets an out-of-process frontend (the WebView
host) push onto the backend-owned stack a `sh <subcommand>` typed afterward will see — wiring WebView's
"Push to stack 0" context action to the real stores. (The reverted attempt used `+0x290` for this; that
offset is now `apply_sync`, so this is a fresh append, not a reuse — the append-only vtable discipline keeps
an older Qt `snaphakui.dll` ABI-compatible: no existing slot offset moved.)

**New backend-exclusive SnapStack+ commands** (`chkstk` / `chkgrp` / `clrgrp`) + toast polish. Store
inspection/management the OG set lacked: `chkstk [N]` lists a stack (or summarizes all), `chkgrp [name]`
lists a group (or all groups), `clrgrp <name>|*` deletes a group. They operate on the backend stores
(correct under WebView; under Qt the ops use Qt's own stores, so these read the backend's separate copy — a
known store-duplication limitation until Qt's copy is retired). Also added confirm toasts to previously
silent ops (`cstk`, `phov`, `pop2g`, `pg` receiver/stack naming, `accl`/`acctargets` receiver toast) since a
stack/group op is otherwise invisible without a `chk*`, and a `pg`/`pop2g` single letter-first arg now
implies stack 0 (`sh pg mygroup` == `sh pg 0 mygroup`), consistent with how the operand resolver treats a
letter-first arg as a group everywhere else.

**Verified in-game (WebView build):** `psel`, `popsel`, `phov`, `cstk`, `pr`, `pg`, `pop2g`, `filtinh`,
`filtcls`, `bss`, `bsi`, `bsf`, `bsb`, `bse`, `bsin`, `bscls`, `bsincls`, `accl`, `acctargets`, plus the new
`chkstk`/`chkgrp`/`clrgrp`/`snapstack_diag`, and Qt confirmed unaffected. **`mkcmd` is ported (a faithful C
port of Qt's handler + the byte-exact prefab template) but not live-verified** — flagged as a TODO.

**Next (Phase 2, follow-up):** retire Qt's own `snapstack.cpp` copy so the Qt frontend also runs the shared
backend handlers/stores — unifying both frontends on one implementation and making `chkstk`/`chkgrp`/`clrgrp`
correct under Qt too. Plus new commands the shared home enables (e.g. `chkstk`-adjacent tooling).

## 2026-07-13 — Palette-Timeline portable-inherit normalize moved into a shared backend slot (`+0x298`); the decl-source blob lags the raw inherit by one commit

**What & why.** A Timeline placed from the in-game SnapMap palette is spawned from a repurposed
`snapmaps/editor_only/placeholder_target` entityDef (the only way to make a Timeline selectable in the
palette — the clone can't fabricate one directly), so the fresh entity records *that* as its `inherit`. A
map saved with it only reloads where our override is installed — not portable. The Qt frontend had a
normalize (`sh_timeline_normalize_inherit`) that rewrites the inherit to the portable `snapmaps/unknown`,
but it lived **entirely in Qt** — WebView had no equivalent, so a palette Timeline authored in WebView
silently kept the non-portable inherit. Ported the logic into a shared backend vtable slot **`+0x298`
`normalize_timeline_inherit`** (`snaphak_iface.h`/`.c`, `apply_engine.c` `slot_normalize_timeline_inherit`,
exported via `sh_apply_engine_get_slots`); both frontends now call one implementation. Qt's function became
a thin wrapper over the slot (old local logic kept only as an old-backend fallback); WebView calls it from
its Timeline rescan.

**The bug the port exposed — the inherit blob lags one commit.** The normalize kept committing but the
Inherit box (and the *saved map*) stayed on the placeholder. Root cause is an ordering property of the
shared commit body `ae_apply_one`: it runs `DeclSourceRebuild` (which re-emits the decl-source blob at
`defsub+0x38`, baking in the inherit header from the *current* `defsub+0x58`) **before** it assigns the new
inherit to `defsub+0x58`. So after one commit the raw idStr reads `snapmaps/unknown` but the blob still
reads the placeholder — and `get_inherit` (`+0x50`) reads that blob, as does the map serializer. The
normalize's re-fire gate therefore **must read the blob (`defsub+0x38`), not the raw idStr (`defsub+0x58`)**:
gating on the raw field commits exactly once and leaves the blob stuck forever; gating on the blob keeps it
firing across rescans until a later commit's `DeclSourceRebuild` finally bakes `snapmaps/unknown` into the
blob. This is the self-correcting behavior Qt's original `sh_tabs` `get_inherit` gate relied on — the
"repeated commits" in the log are load-bearing, not waste. (A raw-field gate was tried first and confirmed
in-game to leave both frontends stuck on the placeholder; the DECL_BLOB_OFF gate fixed it.)

**Footprint note.** The slot heap-allocates its two 256 KB scratch buffers per call and frees them (matching
Qt's `SH_TL_JSON_CAP`); an earlier version used two persistent 1 MB static/BSS buffers and caused a
**controller-freelook regression** in-game (2 MB of resident BSS the Qt path never carried) — reverted, and
the reason it's heap-transient now.

> **This retires the "freshly-placed Timeline needs a save+reload" note in [`fidelity.md`](fidelity.md)**:
> that was never an engine limitation — it was this deferral crash plus a WebView JS bug (see below /
> `webview-ui.md`). Both fixed; fresh placement *and* reclass now save immediately in both frontends.

**TODO (architectural, tracked):** the SnapStack command handlers (`snapstack.cpp`) are still **Qt-only**.
Each frontend independently chooses inline (`+0x290`) vs deferred (`+0xd0`) at every decl-edit call site —
a footgun that already bit twice this cycle (WebView Save Timeline silently regressed to the deferred crash
path when one line was reverted). The durable fix is to **port SnapStack down into the backend** as shared
command handlers (with the additional ops WebView still lacks), so both frontends send high-level intents
through one commit path and can't diverge. Interim hardening option: make the backend's `+0xd0` schedule
commit `kind=0` decl-edits inline anyway (only genuinely deferring `kind=1` mkcmd staging), so no frontend
call site can pick the crashing path. See the convention note in the 2026-07-12 entry below.

## 2026-07-12 — Confirmed: the decl/classname/inherit/displayname Save setters cannot overflow (contributor follow-up)

A contributor asked us to confirm `set_classname` (+0x78), `set_inherit` (+0x80), `set_displayname`
(+0x128), and `rebuild_set_declsource` (+0x40) have headroom for arbitrarily long input — the same
question that, for `PREFAB_TEMP_SIZE`, turned out to be a real stack-buffer overflow (see the
2026-07-06 entry below). Worth checking rather than assuming.

Live-RE against OG SnapHak (Beta 2, `snaphakui.dll`): the **Save to Decl** button's own click handler
(`FUN_1800175a0` → `FUN_180017d00`) reads each Qt field (decl text, inherit, classname, displayname),
computes its exact length, and passes that length explicitly into `FUN_180004a3c` for every one of the
four fields. Decompiling `FUN_180004a3c` shows a standard **growable string assign** (SSO threshold at
15 bytes, ~1.5x geometric growth, allocate-copy-free-old on overflow of current capacity) — the same
shape as `std::string::assign`, not a fixed-size buffer. There is no length cap anywhere in this path;
OG's own Save handler relies entirely on the assign function growing to fit, for all four fields.

This is architecturally unrelated to the `PREFAB_TEMP_SIZE` bug, which was a fixed-size **stack struct**
overflowed by a ctor call sequence — a completely different code shape from a dynamic string assign.
Our clone's `set_classname`/`set_inherit`/`set_displayname`/`rebuild_set_declsource` call the engine's
real assign functions directly (the same functions OG's own Save path calls), so they inherit the same
safety. **No code change needed** — confirmed safe by decompile, not by assumption.

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
(`kind=1`), which never crashed and is left deferred. (During the hunt a `C2 SYNC apply: …` backend-log
marker confirmed the inline path ran; removed in the 2026-07-13 diagnostics cleanup.)

This supersedes the earlier "JSON round-trip vs in-memory node-tree edit" theory: our round-trip matched
OG's, so it was never the problem.

**Also migrated to `+0x290` the same day:** **Timeline Save** and the palette-timeline **inherit-normalize**
(`sh_timeline.cpp`, `tl_iface_schedule_apply`). The inherit-normalize is a one-shot fired by the sh_tabs
poll; on the deferred path it never persisted reliably, so the poll kept re-firing it (a `tl-inherit-portable`
toast on every logic-entity selection) — synchronous commit makes it a true one-shot. Fixing Timeline Save's
double-free also cleared the downstream timeline symptoms (copy/paste being wiped, needing to "save-backout"
before a timeline save).

> **Convention going forward — commit decl edits SYNCHRONOUSLY.** Any new operation that edits an entity's
> decl (serialize → patch → `ae_apply_one`) MUST commit inline via the `+0x290` `apply_sync` slot
> (`iface_apply()` in the Qt frontend), NOT the deferred `+0xd0` `clone_bss_apply` schedule. The command
> handlers already run on the UI/think-loop thread where reflect resolves; deferring to the main-thread
> command buffer splits the operation across frames and double-owns the decl-source block. The deferred
> `+0xd0` path is retained only as an old-backend fallback and for prefab/mkcmd staging (`kind=1`), which
> stages into the paste slot rather than rewriting a decl.

**`ae_schedule_target_write` (`kind=3`) migrated to inline too, though it's DORMANT.** It writes
`state.edit.targets` onto the source entity's decl — but it **never fires in normal use**: `sh_target_any`
targets via SnapMap's native input/output-node logic and writes nothing to the decl (only `acctargets` ever
produces a `targets` list). So it was never a live crash risk. It's the natural primitive for a *future*
UI-driven "add target" feature, so it was migrated the same day (`ae_schedule_target_write` now calls
`ae_apply_target_write` inline, SEH-guarded, no deferral) as zero-risk future-proofing — that future feature
is crash-correct by default. Prefab Load/Place + `mkcmd` (`kind=1`) stage into the paste slot (different
mechanism, never crashed, intentionally left on the deferred path). *(Update 2026-07-13: WebView's Save
Timeline was migrated onto `+0x290` too — see the 2026-07-13 entry above; it was on the deferred `+0xd0`
path when this entry was written.)*

**DONE (2026-07-13):** the `AE_APPLY_DIAG` / `AE_DESER_DIAG` flags are now `0`, and the `+0x40 rebuild` and
`C2 SYNC apply` markers plus the `normalize-timeline-inherit … committed` log were removed — the backend no
longer emits the hunt-time diagnostics.

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
