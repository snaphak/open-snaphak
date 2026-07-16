# Porting open-snaphak to the current (post-April-2024) DOOM build

**Status doc + collaborator handoff.** Branch: `feature/doom-current-build-support`.
Last updated: 2026-07-16.

This document tracks the effort to make open-snaphak work on the **current Steam DOOM (2016) build**
(the one shipped after the **April 11, 2024** patch). It covers what the patch broke, what has been
fixed and *how*, what is still broken, and the exact method + addresses so this work can be picked up
by anyone.

> ⚠️ **Addresses in this doc are for the current test build.** DOOM can update again and shift them. The
> **method** of re-deriving each address matters more than the literal RVA. Where possible we resolve
> engine functions by **byte-signature** (auto-adapting) or by a **stable delta** off a signatured
> anchor, and hard-code only where unavoidable (data globals), always behind a validation guard.

---

## TL;DR — current state

**Working on the new build:**
- Backend DLL loads (as `XINPUT1_4.dll`), WebView UI opens.
- Editor is detected; the **Entities tab populates** with the real entity list + per-entity details.
- **Both selection sync modes** (Follow editor selection / Select in 3D editor).
- **Camera-origin lock** (read + write x/y/z).
- **Editing: classname, inherit, decl-source, displayname** — edit + Save works.

**Not working yet (needs re-derivation — see [punch list](#whats-still-broken--todo)):**
- **Overrides** (blocks placing timelines) · **Timelines** · **Prefabs** (create-from-selection / load-place)
- **`acctargets`** (SnapStack) toast error · **Delete** entity · **command unlock** (131 vs 312 commands)
- Live type registry (class/inherit dropdowns use a static fallback) · devmode toggle

---

## Background: what the April 2024 patch changed

1. **The DLL name changed.** Vanilla DOOM used to import `XINPUT1_3.dll` for controller input; the OG
   SnapHak (and our clone) took that app-dir slot so its `DllMain` runs. The post-April-2024 build
   imports **`XINPUT1_4.dll`** instead (confirmed via `dumpbin` — it pulls ordinals 2/3 =
   `XInputGetState`/`SetState`). A `XINPUT1_3.dll` in the game folder is therefore **never loaded** on
   the new build → our DllMain never ran → zero log, no UI. See [fixed #1](#1-dll-name-xinput1_4dll).
2. **Address drift.** The patch relocated code and data. Functions we resolve by **byte-signature**
   mostly re-locate themselves automatically (51/54 signatures still resolve). But:
   - **data globals** (the editor object, cvar system, decl manager, …) have no code byte-shape to sign,
     so they were hard-coded RVAs — **all stale** on the new build.
   - a few **signatures no longer match** (their function prologues changed) and self-disable.
   - some engine functions were reached by a **hard-coded RVA fallback** (never signatured) — stale.
3. **It is NOT an anti-mod block.** The community's DoomLegacyMod was likewise fixed by re-deriving
   addresses (see its v202407). The exe is also **not** SteamStub-packed on this build (Steamless
   refuses it; the on-disk `DOOMx64vk.exe` imports directly into Ghidra — 111,870 functions).

### The mental model for this port

> **Signature-resolved engine functions ≈ free.** They auto-adapt across builds.
> **Data globals + hard-coded RVAs = the porting debt.** Each must be re-derived per build.

Most of the remaining work is: *find the moved thing in Ghidra, re-derive it (portably where possible),
wire it, un-gate the feature, test.* Same playbook every time.

---

## Architecture recap (1 minute)

- **Backend** = our `XINPUT1_4.dll` / `XINPUT1_3.dll` (`src/backend/`). Proxies XInput to System32 so
  the controller keeps working, and hosts all engine-touch logic. One package ships **both** names so a
  single build serves old + new DOOM (each loads only the proxy whose name it imports).
- **Frontend** = `snaphakui.dll` — a WebView2 (HTML/JS) UI (`src/ui/webview/`), talking to the backend
  over a C ABI vtable (`src/common/snaphak_iface.h`). The Qt frontend has been retired.
- **Fault shield** — a VEH-based recover-in-place layer that logs faults to
  `%USERPROFILE%\snaphak\…` / `<DOOM>\snaphak_logs\`. Critical for diagnosing new-build crashes.

---

## What's been done (and how)

### 1. DLL name: `XINPUT1_4.dll`
`src/backend/build.ps1` builds a **second output** `XINPUT1_4.dll` from the same sources
(`xinput_proxy.c` already forwards to System32 `XInput1_4.dll` internally) + `src/backend/xinput1_4.def`
pinning 1.4's real export ordinals (2=GetState 3=SetState 4=GetCapabilities 5=Enable 7=BatteryInfo
8=Keystroke; ordinal 6 differs in 1.4 and is dropped). `package.ps1` ships both. **Committed** (`282f0a0`).

> The XInput proxy is a transparent pass-through — it hands DOOM's own buffer to the real XInput and
> copies nothing, so it does not affect controller axes. (Ruled out during a controller-glitch scare.)

> ⚠️ **Backward-compat with the PREVIOUS DOOM patch is UNTESTED.** Both `XINPUT1_3.dll` and
> `XINPUT1_4.dll` are now built from the **same** (new-build-modified) sources — note the two files now
> have the **same size**, where the `1_3` used to differ. So `XINPUT1_3.dll` is **no longer the old
> known-good binary**; it now carries all of this branch's new-build changes (editor global-pointer
> resolve, the `SH_NEWBUILD_*` gates, the typeinfo prologue guards, etc.). In principle those changes are
> additive/guarded and `XINPUT1_3.dll` *should* still run on the pre-April-2024 DOOM (the depot people
> pinned to), but **this has not been verified**. If we want to keep supporting the previous version, it
> needs a dedicated test pass (and possibly build-version detection so the new-build-only paths only
> engage on the new build). Tracked in the punch list as item **H**.

### 2. Boot crash — `IdListGrow`
The command-unlock path called through a build-locked RVA (`IDLIST_GROW_RVA` 0x699a60) that the patch
moved → AV on boot. Replaced with an **"IdListGrow" byte signature** in `signatures.c`;
`sh_commands_install` takes the resolved pointer, NULL falls through to a safe skip. **Committed**
(`9e62a44`). *Caveat:* the signature currently resolves **AMBIGUOUS** on the new build (templated
`idList<T>::grow` has near-identical copies) → it safely no-ops → this is the **131-vs-312 command drop**
(see TODO).

### 3. Editor detection — the big one
On old builds the editor (`idSnapEditorLocal`) was a fixed in-module object at a hard-coded RVA. That
moved, and worse, our first fix (a structural fingerprint scan) **adopted a decoy object** whose fields
*looked* like the editor's but weren't (its `+0x204d0` pointed at a menu-screen string table, not the
selection) → the UI opened but every panel was empty / crashed.

**Root cause:** the editor-level offsets never actually moved — we were reading them off the wrong object.

**Fix** (`src/backend/iface_engine.c`):
- Identify the editor by its **exact vtable**. `GetCameraOrigin` is `lea rax,[rcx+0x170]; ret`
  (byte-sig `48 8D 81 70 01 00 00 C3`) and sits at **editor_vtable + 0xd8** → editor vtable found.
- The editor object turned out to be **heap/8-byte-aligned** (a 16-byte-stride scan skipped it), but a
  **global pointer to it lives in `.data`**. `sh_scan_for_editor()` walks module data slots for a
  pointer whose target's vtable == the editor vtable (unique: 1 of ~1.5M). That yields both the editor
  and the slot RVA.
- `editor_base()` now resolves **instantly** from the hard-coded global-pointer slot
  (`EDITOR_GLOBAL_PTR_RVA`), with the vtable-scan as the portable fallback. (The slow scan was starving
  the WebView message pump → very slow UI.)

### 4. Entity / selection / map offsets — all unchanged from OG
Live-probed the *real* editor: **every OG offset still holds.** The only bug was our own
`entity_array()` validator, which wrongly required a C++ vtable at `entity+0` — but entities **start
with an int tag**, and the correct validity test is OG's `entity+0x08 != 0`. Fixed. See the
[address table](#key-addresses-current-build) for the confirmed layout.

### 5. Type-registry stale-RVA guards
The class/inherit dropdown enumerate + several console commands reach the engine via hard-coded RVAs in
`src/backend/typeinfo.c` (`declMgr` accessor `0x17F7030`, pure decl-find `0x18017A0`, resource-mgr ctx
`0x59BD8F0`). **All stale** — e.g. `0x17F7030` now lands inside `idImpactManager::Serialize`; calling it
corrupts the SEH frame and DOOM faults a frame later. **Guards added**: `sh_typeinfo_get_declmgr()`
validates the accessor's prologue bytes before calling; `sh_typeinfo_inherit_base()` is gated on it. On
mismatch they return NULL → the whole type registry **degrades to a static class list** instead of
crashing. (Live registry restoration = re-derive those RVAs; see TODO.)

### 6. Editing: classname / inherit / decl / displayname
- **classname / inherit / decl-source** use **signature-resolved** engine functions (`IdStrAssign`,
  `DeclSourceRebuild`) — correct on the new build. Gated behind `SH_NEWBUILD_WRITE_VERIFIED` (now `1`).
  **Confirmed working** ("Saved to decl" toast).
- **displayname** used a stale hard-coded `IdStrOpAssign` RVA (`0x19fd5f0`). Found `idStr::operator=` at
  **`IdStrCtor + 0x700`** — a delta stable across the patch — and wired it **portably** off the
  signature-resolved `IdStrCtor`, with a prologue-byte validation (NULL on mismatch → safe no-op).
  **Confirmed working.**

### Gating pattern (important for the collaborator)
Features whose engine calls aren't yet re-derived are **gated to a safe no-op** rather than left to
crash. Flags in `iface_engine.c`:
- `SH_NEWBUILD_SEL_VERIFIED` (1) — selection add (sig-resolved).
- `SH_NEWBUILD_WRITE_VERIFIED` (1) — classname/inherit/decl setters (sig-resolved).
- `SH_NEWBUILD_STALERVA_OK` (0) — **delete** (`RemoveFromSelection`, stale). Displayname was moved off
  this flag once re-derived.

Flip a flag / un-gate a slot only after its engine address is re-derived **and** validated.

---

## What's still broken / TODO

Roughly dependency-ordered. Each is the same shape of fix: re-derive a moved signature/RVA.

### A. Overrides — `ResProviderCtor` signature `NOT_FOUND`  *(blocks timelines)*
`B1: overrides file-shadow SKIPPED -- ResProviderCtor not resolved`. Overrides (serving
`%USERPROFILE%\snaphak\overrides\<name>` from disk) hooks the engine **resource-provider constructor**,
found by a byte signature that no longer matches (prologue changed). **The user needs this to place a
timeline.**
- **Fix:** find the resource-provider ctor on the new build (old RVA `0x1a51070`; its member[0] = the
  vtable at old engineBase `+0x27984a0`), capture a fresh prologue signature, update `signatures.c`
  (`"ResProviderCtor"`).

### B. Timelines (create / place / edit / save)
Depends on **A (overrides)** to place a timeline, plus its own decl/spawn engine calls. Untested on the
new build beyond listing. Verify after overrides is back.

### C. Prefabs — create-from-selection, load/place, delete/rename
These serialize a selection to a prefab and spawn it back. They go through the apply/serialize engine
functions (`apply_engine.c`) — verify each; likely one or more stale calls.

### D. `acctargets` (SnapStack) throws a toast error
A `sh` SnapStack subcommand. Reproduce, read `snaphak_logs`, and check whether it's a stale engine call
or a validation reject.

### E. Delete entity — `RemoveFromSelection` (stale RVA `0x59fda0`)  *(gated: `SH_NEWBUILD_STALERVA_OK 0`)*
The delete path calls a hard-coded `RemoveFromSelection` RVA that moved (crashed at `0x59fdbf` reading
`[ptr-8]`). **Not findable by static delta/pattern** — the deltas that worked for other clusters fail
here, and candidate functions don't match the array-shift semantics (search `sel+0x80` for the id,
shift-remove, decrement `sel+0x88`). **Options:** (1) route the webview delete through the editor's
**native delete console command** via `ExecuteCommandText` (sig-resolved cmdSystem) — cleanest, no RVA
hunt; (2) capture it live (a heap-data watchpoint on `sel+0x88` write during a native delete — blocked
by the current debugger tooling, which only watches module addresses); (3) keep hunting the array-shift
signature.

### F. Command unlock — `IdListGrow` AMBIGUOUS  → 131 vs 312 console commands
The templated `idList<T>::grow` has near-identical copies so the signature is ambiguous → safe no-op →
DEV commands don't unlock (cvars ~6582 vs ~6592 too). **Fix:** decode the grow helper from
`AddCommand`'s (uniquely-resolved) call site instead of a direct signature.

### G. Live type registry + devmode  *(nice-to-have; static fallback works)*
- Class/inherit dropdowns use a **static** class list because the `typeinfo` `declMgr`/decl-find RVAs are
  stale (guarded, see done #5). Restore by re-deriving `0x17F7030` (lazy-init singleton accessor,
  0-arg, returns declMgr in RAX), `0x18017A0` (pure decl-find), `0x59BD8F0` (resource-mgr ctx).
- **devmode toggle** — `SessionDevModeGetter` signature `NOT_FOUND` (old `0x18a31d0`); self-disabled.

### H. Previous-DOOM-version support is untested
`XINPUT1_3.dll` is now built from the same new-build-modified sources as `XINPUT1_4.dll` (same file size),
so it is no longer the old known-good binary. It *should* still work on the pre-April-2024 build (the
changes are additive/guarded) but has **not** been tested. **Decision needed:** do we keep supporting the
previous version? If yes: a dedicated test pass on the old depot, and likely **build-version detection**
so the new-build-only code paths (editor global-pointer slot, stale-RVA guards, gates) only engage on the
build they belong to — right now they run on whichever DOOM loads the DLL.

---

## Key addresses (current build)

> RVAs = offset from `DOOMx64vk.exe` image base. Confirmed live/decompiled this session. **Re-verify if
> DOOM updates.**

### Editor object + layout
| Thing | Value | Notes |
|---|---|---|
| Editor global-pointer slot | RVA `0x2F8B238` | `editor = *(base + this)` (`EDITOR_GLOBAL_PTR_RVA`) |
| Editor object | RVA `0x2F6AD18` | 8-byte-aligned; heap-ish |
| Editor vtable | RVA `0x2360588` | `GetCameraOrigin` at vtable+0xd8 (RVA `0x1183600`) |
| Camera origin (vec3) | `editor + 0x170` | read/write |
| Loaded map ptr | `editor + 0x204c8` | unchanged from OG |
| Selection ptr | `editor + 0x204d0` | unchanged from OG |
| Editor state/mode | `editor + 0x23618` | 1=ModuleMode 2=EntityMode |

### Map / entity / selection
| Thing | Value |
|---|---|
| Entity array ptr | `map + 0x6a0` |
| Entity count | `map + 0x6a8` (idList: num@+0x6a8, size@+0x6ac) |
| Entity validity | `entity + 0x08 != 0` (entity+0 is an **int tag**, not a vtable) |
| Entity def-subobject | `entity + 0x158` |
| Entity layer bits | `entity + 0x160` |
| Entity displayname (full idStr) | `entity + 0x170` |
| defsub inherit / classname | `defsub + 0x58` / `defsub + 0x60` |
| defsub decl-src len / ptr | `defsub + 0x138` / `defsub + 0x140` |
| Selection ids / count | `sel + 0x80` / `sel + 0x88` (capacity `+0x8c`) |
| Selection hovered id | `sel + 0x2c` |

### Engine functions (this build)
| Function | New | Old | How resolved |
|---|---|---|---|
| AddToSelection | `0x11fad50` | `0x59f210` | signature |
| ClearSelection | `0x11fb540` | `0x59fa00` | signature |
| IdStrCtor | `0x33A0F0` | `0x19fcef0` | signature |
| IdStrOpAssign (displayname) | `0x33A7F0` | `0x19fd5f0` | **IdStrCtor + 0x700** (portable) |
| RemoveFromSelection (delete) | *unknown* | `0x59fda0` | **stale — TODO E** |
| ResProviderCtor (overrides) | *unknown* | `0x1a51070` | **sig NOT_FOUND — TODO A** |
| declMgr accessor | *stale* | `0x17F7030` | **hard-coded — TODO G** |
| pure decl-find | *stale* | `0x18017A0` | **hard-coded — TODO G** |
| SessionDevModeGetter | *unknown* | `0x18a31d0` | **sig NOT_FOUND — TODO G** |
| IdListGrow | *ambiguous* | `0x699a60` | **sig ambiguous — TODO F** |

---

## Method: how to re-derive a moved address

1. **Import** the on-disk `DOOMx64vk.exe` into Ghidra (it's not packed; auto-analyze).
2. For an **engine function**: prefer a **byte signature** (a distinctive prologue with rip-relative
   spans wild-carded) → add to `src/backend/signatures.c`. If it's near a signatured anchor at a
   **stable delta** (like `IdStrOpAssign = IdStrCtor + 0x700`), derive it off the anchor at runtime and
   **validate the target's prologue** before use. Only hard-code an RVA as a last resort, always with a
   prologue-byte guard so a stale value self-disables instead of crashing.
3. For a **data global**: find a signatured accessor that loads it (`lea/mov reg,[rip+disp]`) and decode
   the disp; or (like the editor) scan module data for a pointer whose target has the known vtable.
4. **Always guard.** A call through a stale/wrong RVA runs unrelated code that corrupts the SEH frame,
   so a plain `__try` can't catch it → validate first (prologue bytes / vtable identity), fail to a
   NULL/no-op.
5. **Gate the feature** (see the `SH_NEWBUILD_*` flags) until the address is re-derived + verified.

### Build / deploy / test loop
```powershell
# build both backend DLLs (XINPUT1_3 + XINPUT1_4)
pwsh -File src/backend/build.ps1

# deploy — DOOM MUST be fully closed (the DLL is locked while running)
Copy-Item "build\XINPUT1_?.dll" "D:\SteamLibrary\steamapps\common\DOOM\" -Force
```
Then launch DOOM → SnapMap editor → reproduce. Logs: `<DOOM>\snaphak_logs\snaphak_backend.log` and
`shield_faults.log` (a `class=diag ... FAULT` line is an unrecovered crash; `class=fc` first-chance
lines are usually our own SEH-guarded probes and are benign).

### Gotchas
- **DOOM must be fully closed to deploy** (verify via tasklist) — no exceptions; the file is locked.
- **Read-only live debugging works** (attach → read memory → detach). Interactive **breakpoints /
  watchpoints are unreliable** (freeze input; heap watch unsupported by the tooling).
- A `DOOM C++ throw -> forced gate` in the fault log = the engine *rejected* a write (validation), not
  necessarily a stale address — investigate the semantics, not just the RVA.

---

## Related docs
- [`architecture.md`](architecture.md) · [`backend-changes.md`](backend-changes.md) ·
  [`webview-ui.md`](webview-ui.md) · [`packaging.md`](packaging.md) · [`fidelity.md`](fidelity.md)
