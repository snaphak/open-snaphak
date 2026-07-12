# Qt frontend changes — bugfix & behavior log

A running log of correctness fixes in the Qt frontend (`src/ui/`, the `snaphakui.dll` built by
`build-qt.ps1`) — the SnapStack command handlers, the data tabs, and how they drive the shared backend
engine layer. Engine-call bugs in `src/backend/` (used by *both* frontends) live in
[`backend-changes.md`](backend-changes.md); faithful-vs-divergent reproduction of the original's behavior
lives in [`fidelity.md`](fidelity.md). Entries are chronological, newest first.

## 2026-07-12 — SnapStack decl-edits crashed DOOM after Play (the deferred-apply double-free) — root cause & fix

### Symptom

Running any SnapStack **decl-edit** command — `acctargets`, `accl`, or the bulk-set family
`bss`/`bsi`/`bsf`/`bsb`/`bse` — then hitting **Play**, then triggering **any** map teardown would crash
DOOM with `"Memory corruption before block!"`. The teardown could be a reload of the *same* map, a **New
Map**, or loading a completely unrelated map — all of them. **No save was required**: the damage is in live
memory, not the saved file. The fault was always identical:

```
ACCESS_VIOLATION @ DOOM+0x1ab32ee
  ← DOOM+0x19fd162   (IdStrDtor)
  ← DOOM+0x17ad00a   (decl-source teardown)
  ...  load_state=3  (the old map being torn down before the new one loads)
```

The fault address landed one byte past the end of the acctargets-committed 306-byte decl-source block, and
`"Memory corruption before block"` is what the CRT/heap reports when a block's header is trashed at
`free()` — the classic signature of a **double-free**.

### What it was NOT (a long list of dead ends)

Weeks of the investigation chased the *commit* and its *content*, all of which turned out to be red
herrings — recorded here so nobody re-runs them:

- **Not the commit content.** We live-read the committed decl-source blob on both our clone and OG SnapHak
  (read-only debugger attach) — **byte-for-byte identical**, same 306 bytes, same hash, same `targets`
  list, no stray `class =` line.
- **Not the commit function.** We decompiled OG Beta 2's `+0xd0` commit (`FUN_180004b80`) — it is
  **byte-for-byte our `ae_apply_one`**: get `defsub`, deserialize onto a temp def, `DeclSourceRebuild`, two
  `IdStrAssign`, destroy temp. Same in OG 1.3.1 (`FUN_1800045a0`).
- **Not the "settle".** OG's handler does a pre-edit self-commit (`FUN_1800017f4`); we replicated it
  faithfully (two full commits, confirmed in the log) — the fault was byte-identical. Reverted.
- **Not a JSON-vs-node-tree architecture gap.** The standing theory was that OG edits an in-memory node
  tree while we round-trip JSON text. False — OG's commit round-trips through a temp def exactly like ours.
- **Not de-share / COW refcount, float formatting, the unsettled-decl marker, or registration state** —
  each was measured live and ruled out.

A divergent workaround *did* prove the edit could be made crash-free: building the decl **source text**
directly and committing via the synchronous `+0x40` `DeclSourceRebuild` (the same path the manual
Entity-State Save uses) never crashed. That was kept as `splice_decl_reflist` in `snapstack.cpp` for
reference — but it *diverges* from OG, so it wasn't the answer. It did, however, point straight at it: the
one thing every crash-free path had in common was that it committed **synchronously**.

### Root cause — the deferral (a threading bug), not the edit

The decisive comparison was between two paths *inside our own clone*:

| path | how it commits | result |
|---|---|---|
| manual Entity-State Save, class/inherit changes | `+0x40` `DeclSourceRebuild`, **synchronous, inline** | never crashed |
| `acctargets` / `bss` / `bse` (old) | `+0xd0` schedule → `ae_apply_one` **deferred to the DOOM main thread** | crashed |

Same engine `DeclSourceRebuild` underneath both. The only difference was **when and where** the commit ran.

OG SnapHak runs its SnapStack command handlers on the **UI / think-loop thread** (the manual 30 Hz pump at
RVA `0x15c04` that drains the `+0x1a0` work-queue), and commits `+0xd0` **inline, right there** — serialize
+ splice + commit all happen atomically on one thread in one frame. Our `snaphak_ui_init.cpp` is a direct
port of that same think-loop, so `do_acc` *also* runs on the UI thread — but instead of committing inline,
it **scheduled** the heavy commit onto the DOOM main thread (`clone_bss_apply` at `ExecuteCommandBuffer`, a
later frame). That "FIX B" deferral was added long ago on the belief that the structured deserialize AVs
off the main thread.

That belief was **stale**. Our `+0xc8` serialize already runs fine on the UI thread every time, and the
reflection context the deserialize needs is a *process-global singleton* (engine `0x17f7030` → vtable
`+0x80`), reachable from any thread — OG proves it by committing on exactly that thread. So the deferral
bought nothing and cost everything: splitting one atomic operation across **two threads and two frames**
left the freshly-committed decl-source block **double-owned**, and the next map teardown freed it twice.

### The fix

Commit **inline on the UI thread, exactly like OG** — no deferral. A new synchronous apply slot `+0x290`
(`apply_sync`) was added to the shared vtable ABI (see [`backend-changes.md`](backend-changes.md) for the
backend side); the Qt frontend routes every decl-edit op through it:

- `snapstack.cpp` gained `iface_apply()` — try the synchronous `+0x290` first, fall back to the deferred
  `+0xd0` schedule **only** on an old backend that lacks the slot.
- `do_acc` (`accl`/`acctargets`), `do_bulkset` (`bss`/`bsi`/`bsf`/`bsb`), and `do_bse` all now call
  `iface_apply()` instead of `iface_schedule_apply()`. The edit itself is unchanged — same serialize →
  JSON list/leaf patch → `ae_apply_one` — it just runs **now, on this thread**, so the committed block has
  a single clean owner.
- `mkcmd` / prefab-paste (`kind=1`) is left on the deferred path: it targets the editor paste slot, is a
  different operation, and never exhibited the crash.

**Verified:** `acctargets`, `accl`, `bss`, `bsi`, `bsf`, `bsb`, `bse` — including *chained* commands in one
session (e.g. `acctargets` then `bsb` on every entity) — all clean through both Play → New Map and
Play → Save → Reload. (Chaining was itself broken on the old deferred path, which kept only one pending
batch at a time and could drop the first edit; the synchronous path has no pending state.)

### Not affected

The **class / inherit** commands (`bscls`, `bsincls`, and the single class/inherit setters) were never on
the deferred path — they already commit synchronously via `+0x78`/`+0x80`/`+0x268` (set idStr) → `+0x40`
(`DeclSourceRebuild`) directly, the same crash-free path as the manual Save. They needed no change.

### Still TODO

- **Timeline Save** hits the same root cause but through its own path (the `|0x80` consumer in
  `sh_timeline.cpp`); migrate it to `iface_apply()` too.
- **Pre-release cleanup:** quiet the diagnostics left on for this hunt (`AE_APPLY_DIAG` / `AE_DESER_DIAG`
  in `apply_engine.c`, the `+0x40 rebuild` trace in `iface_engine.c`, the `C2 SYNC apply` marker), and
  decide whether to keep or delete the unused `splice_decl_reflist` reference implementation.
- **WebView UI:** the `+0x290` slot is frontend-agnostic (the WebView host drains the same `+0x1a0`
  work-queue on its own UI thread), so the fix carries over — but the SnapStack command *logic* is still
  Qt-only and would need porting, ideally into the backend so both frontends share one path.
