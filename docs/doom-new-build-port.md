# Porting open-snaphak to the current (post-April-2024) DOOM build

**Status doc + collaborator handoff.** Branch: `dev`.
Last updated: 2026-07-16.

> **This document is temporary.** It tracks one port. When the punch list is empty and both builds are
> verified, the durable parts (the [method](#method-how-to-re-derive-a-moved-address)) move into the
> project's own docs and this file goes away. Per-build addresses do not belong in prose at all — they are
> observations of a resolver's output, not settings.

This document tracks the effort to make open-snaphak work on the **current Steam DOOM (2016) build**
(the one shipped after the **April 11, 2024** patch). It covers what the patch broke, what has been
fixed and *how*, what is still broken, and the exact method + addresses so this work can be picked up
by anyone.

> ⚠️ **Addresses in this doc are for the current test build.** DOOM can update again and shift them. The
> **method** of re-deriving each address matters more than the literal RVA. Where possible we resolve
> engine functions by **byte-signature** (auto-adapting) or by a **stable delta** off a signatured
> anchor, and hard-code only where unavoidable (data globals), always behind a validation guard.

---

## ⚠️ READ FIRST — a large batch of changes has landed and NONE of it has been run

Everything in the "recently addressed" section below was derived by comparing the two DOOM
executables offline and is verified only *statically*: signatures were checked to resolve uniquely on
**both** builds, the tree builds clean, unit tests pass. **No DOOM process has executed a line of it.**

"The bytes match" is not "the editor opens". Please treat the claims below as *needing testing*, not as
done, and report what actually happens. The most valuable thing anyone can do with this branch right
now is run it — on the current build **and** on the pre-April-2024 one.

---

## TL;DR — current state

**Working on the new build (as previously reported, unchanged):**
- Backend DLL loads (as `XINPUT1_4.dll`), WebView UI opens.
- Editor is detected; the **Entities tab populates** with the real entity list + per-entity details.
- **Both selection sync modes** (Follow editor selection / Select in 3D editor).
- **Camera-origin lock** (read + write x/y/z).
- **Editing: classname, inherit, decl-source, displayname** — edit + Save works.

**Recently addressed — implemented, NOT yet tested** (details per item in the punch list):
- **Overrides**: the ctor is found again, *and* the hook no longer assumes a vtable slot index — the
  slot moved (see [the vtable finding](#the-vtable-finding-the-one-real-behaviour-change)). Unblocks timelines.
- **Prefabs**, **Delete**: their engine functions are signature-resolved; delete is un-gated.
- **SnapStack**: a likely common root cause fixed — a second, stale copy of the editor pointer.
- **Previous-build support**: was not merely untested, it was **broken by construction**; fixed.

**Still open:** command unlock (131 vs 312) · live type registry + devmode · the save-path C++ throw.

**All `SH_NEWBUILD_*` gates are gone** — there is nothing left gated off pending a re-derivation.

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

### How the code approach changed (pre-patch → now)

If you know the pre-patch code, this is the important part — the *code structure*, not just the numbers,
is different now, on purpose.

| Concern | Pre-patch code | This branch (new-build) | Why the change |
|---|---|---|---|
| **Editor object** | one hard-coded RVA (`EDITOR_SINGLETON_RVA`) to the in-module singleton | **resolved at runtime** — identify by exact vtable (`GetCameraOrigin` at vtable+0xd8) + scan `.data` for the global pointer to it (`sh_scan_for_editor` / `editor_base`) | the object moved *and* went heap/8-aligned; a runtime identity has **no RVA to maintain** across future patches |
| **Engine functions** | signatures where present, else a hard-coded RVA "fallback" that was assumed correct | **signature first → stable-delta-off-a-signatured-anchor → hard-coded RVA only as last resort, always prologue-guarded** | signatures auto-adapt; a delta off a sig anchor (e.g. `IdStrOpAssign = IdStrCtor+0x700`) stays portable; a bare RVA does not |
| **Calling a resolved address** | called directly; assumed valid | **validate before calling** (prologue bytes / vtable identity), and if it doesn't match, **degrade to NULL/no-op** | a call through a *wrong* address runs unrelated code that **corrupts the SEH frame**, so a plain `__try` can't even catch it — you get a hard crash-to-desktop. Validating first is the only safe option (see `sh_typeinfo_get_declmgr`, the `IdStrOpAssign` derive) |
| **A feature whose address isn't re-derived yet** | n/a (everything was assumed working) | **explicitly gated to a safe no-op** behind an `SH_NEWBUILD_*` flag | lets the mod stay stable + shippable while features are ported one at a time, instead of one stale call taking down the whole editor |

**The through-line:** pre-patch was "hard-code the addresses and assume they're right." Because a DOOM
update shifts everything at once, that approach means the *next* update breaks (and crashes) everything
again. This branch deliberately moves toward **self-adapting resolution where possible, and fail-soft
(validate → no-op) where not** — so a future patch degrades gracefully (empty panel / inert button)
instead of crashing, and buys time to re-derive. The offsets *inside* the editor/entity/selection structs
did **not** change (see [#4](#4-entity--selection--map-offsets--all-unchanged-from-og)); the breakage was
almost entirely stale **top-level addresses** + one of our own validators.

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

### Gating pattern — served its purpose; the flags are now gone
While addresses were unknown, features whose engine calls weren't yet re-derived were **gated to a safe
no-op** rather than left to crash (`SH_NEWBUILD_SEL_VERIFIED`, `SH_NEWBUILD_WRITE_VERIFIED`,
`SH_NEWBUILD_STALERVA_OK`). That was the right call at the time and it is why this branch stayed usable
while half-ported.

**All three are now removed** — the last one (`STALERVA_OK`, gating delete) went when
`RemoveFromSelection` stopped being an address at all and became a signature.

The gates are replaced by something strictly better: a compile-time flag encodes a belief about *which
build you will run on*, whereas a **null check on a resolved pointer** reacts to the build actually
running. An engine function that does not resolve is NULL and its op no-ops on its own — on either build,
with no flag to remember to flip. If you add a new engine dependency, give it that shape rather than a
new flag.

---

## The vtable finding (the one real behaviour change)

Most of this port is addresses moving. **One thing is not**, and it is the kind of bug that never
crashes — it just makes a feature quietly do the wrong thing.

The overrides hook swaps the file system's **open-by-name** vtable slot. That slot was `+0xf8`. On the
current build the class (`idFileSystemLocal`, per its RTTI name) **gained 10 virtuals**, and the same
method now sits at **`+0x148`**. Patch a fixed `+0xf8` there and you replace some *other* virtual: no
fault, no log, overrides simply doesn't shadow anything.

So the code no longer carries a slot index. The method is byte-identical on both builds, so it is
resolved **by signature**, and install **searches the vtable for it** and patches whichever slot holds
it. That is build-agnostic: a future DOOM that reorders these virtuals again needs no change here.

**This generalises, and it is worth knowing when reading the rest of this doc.** Every *engine vtable
slot index* is per-build data, exactly like an RVA. All the ones this project uses were swept against
both builds (spawn dispatch `+0x450`, entity `+0x20`/`+0x168`, cmdSystem `+0x50`/`+0x78`,
`GetCameraOrigin` `+0xd8`): **only the overrides slot moved**. But three vtables did gain virtuals
(file system +10, cvar system +1, common +3), so treat any *new* slot use on those as unverified.

---

## What's still broken / TODO

Roughly dependency-ordered.

### A. Overrides — **ADDRESSED, needs testing** *(unblocks timelines)*
Two separate faults, both fixed:
1. `ResProviderCtor`'s prologue genuinely changed, so no byte pattern could find it. It was located
   instead through **RTTI**: the ctor's only rip-relative operand is its own vtable, and MSVC stores a
   class's identity as a mangled **name string** — which survives a recompile when bytes do not.
   `vtable[-8]` → COL → TypeDescriptor → `.?AVidFileSystemLocal@@`, then walk that chain forward on the
   new build. The name occurs exactly once per build, giving a unique vtable and a single LEA site
   whose containing function (via `.pdata`) is the ctor: **`0x2A28C0`** (it also grew `0xBC` → `0xEF`,
   which is why the signature missed). Incidentally this is what the class actually is — the "resource
   provider" naming here is a misnomer; overrides hooks the **file system's** open-by-name.
2. The slot index — see [the vtable finding](#the-vtable-finding-the-one-real-behaviour-change).

**Test:** does `snaphak_backend.log` show `B1: overrides file-shadow installed (... open-by-name found
at slot+0x148 ...)`? Then: does a file in `%USERPROFILE%\snaphak\overrides\` shadow?

### B. Timelines (create / place / edit / save) — **unblocked, needs testing**
Depended on A. Placing a timeline should now work; the rest of the timeline surface is untested on this
build beyond listing.

### C. Prefabs — **ADDRESSED, needs testing**
`PrefabCtor` / `PrefabPopulate` / `PrefabDtor` / `EntityDeshare` were each reached by a hard-coded RVA,
documented as "jumptable/inline-prone leaves the byte-sig scanner cannot reliably anchor". That turned
out not to be a property of these functions — **all four sign uniquely at 20–38 bytes on both builds**;
no one had pointed the extractor at them. All four are now signature-resolved.
**Test:** create-from-selection, then load/place.

### D. SnapStack — **likely root cause fixed, needs testing**
The suspicion that `acctargets` was not isolated looks right, and the shared dependency was not a
stale engine *call* — it was a stale **editor pointer**. `apply_engine.c` kept its *own*
`module_base + 0x3056748` and never used the resolver that was fixed in `iface_engine.c`, so on this
build it pointed at unrelated memory. `snapstack.c` reaches the editor *only* through `apply_engine`'s
slots, so every `sh` op inherited it. Its "editor up" check (`map != NULL`) could not catch this: off a
wrong base that is a coincidence test on unrelated memory, not an identity check.
There is now one accessor (`sh_iface_editor()`) and the private copies are deleted.
**Test:** `sh psel`, `acctargets`, `bss`, `bse`, `push`.

### E. Delete entity — **ADDRESSED + un-gated, needs testing**
`RemoveFromSelection` was reported as "not findable by static delta/pattern". It is findable — it signs
uniquely at **25 bytes** on both builds (**`0x11FB8E0`** here; the delta that finds it is the one shared
by its immediate neighbours `AddToSelection`/`ClearSelection`, not a global one). The gate is removed;
`g_remove_sel` being NULL is now the guard.
**Test:** Entities right-click → Delete.

### F. Command unlock — `IdListGrow` AMBIGUOUS → 131 vs 312 console commands  *(still open)*
Now quantified: the masked body of `idList<T>::grow` matches **1,560 places** on the old build and 1,554 on
the new. This is not "a signature that needs improving" — it is **1,560 template instantiations**, so no
byte pattern can *ever* single one out. The originally-suggested fix is the only correct one: decode the
grow helper from `AddCommand`'s (uniquely-resolved) call site. Nothing else will do.

### G. Live type registry + devmode  *(partly addressed; still open)*
- **The pure decl-find is now signature-resolved** (`0x18017A0` → `0x1572A80`, 31 bytes, unique on both).
  That removes one of *two* blockers, not both: `sh_typeinfo_inherit_base` also needs the **resource-mgr
  ctx** (`0x59BD8F0`), which is a **data global** — no byte shape, so it cannot be signed and stays
  build-locked. The gate therefore stays. To finish this, re-derive the ctx by decoding the validator's
  rip-relative `LEA` (`@ 0x17ad682` on the old build) rather than hard-coding it.
- **`declMgr` accessor (`0x17F7030`) is unsignable in principle** — worth knowing before anyone tries: its
  prologue is shared with ~47 other functions and is unique only via a build-volatile rip displacement.
  It needs a rip-decode or a string/RTTI anchor, never a byte pattern.
- **devmode toggle** — `SessionDevModeGetter` is `NOT_FOUND` **by design**, and should be left that way:
  the pattern deliberately embeds the struct displacement `0x34c89`, so a build where that field moved
  **refuses rather than mis-patches**. That is the fail-safe working. And the field *did* move: `0x34c89`
  exists nowhere in the current build, and the nearest equivalent getters sit ~`0x2CD` lower — i.e.
  `idSessionLocal` shrank by ~717 bytes (plausibly the `steam-no-bnet` change). Two candidate offsets fit
  and there is no unique positional mapping, so this needs a caller-side derivation. Low priority: the
  toggle only patches the getter's head, which is offset-agnostic — it just needs the right address.

### H. Previous-DOOM-version support — **it was BROKEN, not untested. Fixed; needs testing**
This was the assumption worth checking, and it did not hold. `editor_base()`'s fast path reads the editor
global-pointer slot and, if the target isn't an editor, `return NULL` — treating *"this slot isn't what I
expected"* as *"there is no editor"*. But that RVA (`0x2F8B238`) is a **current-build constant that still
sits below the pre-patch build's `SizeOfImage`**, so on the old build the read **succeeds**, yields
unrelated data, and the function returns NULL forever — the boot-RVA and fingerprint paths beneath it were
never reached. The editor would never resolve there and the whole UI would be dead. (Neither accepted
vtable constant is the old build's either.)

Fixed: NULL only for a genuinely *empty* slot (the cheap "editor not open yet" case); otherwise fall
through to the shape-based paths, which identify the editor by structure rather than a constant and so
work on either build.

**And note the anticipated fix — "build-version detection so new-build-only paths only engage on the right
build" — turned out to be mostly unnecessary, which is the better outcome.** Nearly everything is now
resolved by *identity* (signature, RTTI name, vtable search, shape fingerprint), and identity is
build-agnostic by construction. The fewer build constants exist, the smaller "support both builds" gets as
a problem. What remains build-specific is data globals, and those already fail soft.

**Test (the real one):** run the pre-April-2024 build. Does the UI populate?

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
> These are recorded for reference, but **none of them is a maintained constant** — every line marked
> "signature" is *found* at runtime on whatever build is loaded, so the numbers are an observation, not a
> configuration. The only rows that need attention on a future patch are the ones that are not signatures.

| Function | New | Old | How resolved |
|---|---|---|---|
| AddToSelection | `0x11fad50` | `0x59f210` | signature |
| ClearSelection | `0x11fb540` | `0x59fa00` | signature |
| IdStrCtor | `0x33A0F0` | `0x19fcef0` | signature |
| IdStrOpAssign (displayname) | `0x33A7F0` | `0x19fd5f0` | **IdStrCtor + 0x700** (portable delta, prologue-validated) |
| RemoveFromSelection (delete) | **`0x11FB8E0`** | `0x59fda0` | **signature** (25 B, unique on both) |
| ResProviderCtor (overrides) | **`0x2A28C0`** | `0x1a51070` | **RTTI-anchored** (recompiled; no pattern can find it) |
| FileSystemOpenByName (overrides slot) | **`0x2A9B00`** | `0x1a57a60` | **signature** (20 B) — its *slot* is searched for, not assumed |
| PrefabCtor | **`0x11AC8D0`** | `0x54d0a0` | **signature** (20 B) |
| PrefabPopulate | **`0x11ADB30`** | `0x54e410` | **signature** (38 B) |
| PrefabDtor | **`0x117DC40`** | `0x51d870` | **signature** (30 B) |
| EntityDeshare | **`0x118C2C0`** | `0x52c920` | **signature** (29 B) |
| pure decl-find | **`0x1572A80`** | `0x18017A0` | **signature** (31 B) — but its *ctx* is still stale, see G |
| declMgr accessor | *stale* | `0x17F7030` | hard-coded — **unsignable in principle**, see G |
| SessionDevModeGetter | *moved* | `0x18a31d0` | refuses by design, see G |
| IdListGrow | *ambiguous ×1554* | `0x699a60` | **no pattern can work** (1,560 template copies), see F |

### The overrides vtable (the one slot that moved)

| Thing | Old | New |
|---|---|---|
| `idFileSystemLocal` vtable | `0x27984a0` | `0x1FF4CA0` |
| virtuals in it | 43 | **53 (+10)** |
| open-by-name slot | `+0xf8` | **`+0x148`** |

Also confirmed unchanged (swept against both builds): spawn dispatch `+0x450`, entity `+0x20` / `+0x168`,
cmdSystem `+0x50` / `+0x78`, `GetCameraOrigin` `+0xd8`. Editor vtable: `0x2049FD8` → `0x2360588`.

---

## Method: how to re-derive a moved address

**What the April 2024 patch actually is**, because it sets expectations: the same source **rebuilt**, not
rewritten. Same linker and toolset; 25 of 27 imports identical; `.text` grew **0.18%**. All 1.3 MB of
growth is `.rdata` — ~24k strings the old build *stripped* and this one keeps (the old build has **zero**
`L:\zion\code\…` assert paths; this one has 585). The wholesale address movement is **LTCG**: under
whole-program optimisation, changing any input re-lays-out the program while leaving each translation
unit's internals intact. That is why the deltas come in clusters with different signs, and why *bytes*
barely changed while *addresses* all did.

The practical consequence: **almost every function is still byte-identical, so it can be found rather than
re-derived.** Prefer finding.

### The ladder — try these in order

1. **Signature** (masked byte pattern). Works for anything whose bytes didn't change — which is nearly
   everything. Prefer the **shortest instruction-aligned prefix that is unique on every build you have**,
   not the longest: extra fixed bytes are extra surface for the next rebuild to break. But don't go *too*
   short either — a bare minimal-unique can be a generic prologue that is unique by luck rather than by
   identity (the prefab ctor is unique at 12 bytes of `mov r11,rsp; push rbx; sub rsp,0x30` — true today,
   and no basis for tomorrow).
2. **Stable delta off a signatured anchor**, *within the same translation unit* (`IdStrOpAssign =
   IdStrCtor + 0x700` holds on both builds). Cross-cluster deltas do **not** hold — the clusters move by
   different amounts and even opposite signs. Always validate the target's prologue.
3. **RTTI anchor** — the only rung here proven to survive a **recompile**. If a function's bytes changed,
   no pattern can find it; but if it touches a class, MSVC stores that class's identity as a mangled
   **name string**, and strings survive. `vtable[-8]` → COL → `+0x0C` → TypeDescriptor → `+0x10` → name.
   Find the name on the new build, walk it forward to the vtable, find who references the vtable, and map
   that site to its containing function via `.pdata`. This is how `ResProviderCtor` was recovered.
4. **Data global** — no byte shape, so never a signature. Decode it from a *signatured accessor* that
   loads it (`lea/mov reg,[rip+disp]`), or identify the object it points at (an exact vtable). Never
   hard-code one you can decode.
5. **Vtable slot** — a slot index is per-build **data**, exactly like an RVA. Don't hard-code it: resolve
   the *method* and search the vtable for it.
6. **Hard-coded RVA** — last resort, and only with a validation guard.

### The two rules that matter more than the ladder

**A wrong resolution is worse than none.** A call through a wrong address runs whatever function now lives
there and corrupts the SEH frame — not even `__try` catches it. So an unresolved thing must be **NULL and
declined**, never substituted with a guess.

**A guard is not a validation.** `__try` catches an access violation. It cannot catch a *successful* read
or write at a wrong-but-mapped address — and on a relinked build that is the *common* case, because every
RVA below `SizeOfImage` still reads fine. It just returns plausible garbage. This has bitten this codebase
three times: a `map != NULL` "editor up" check that was really a coincidence test on unrelated memory; an
SEH-wrapped byte write that would simply land somewhere else; and a signature fallback that substituted a
stale address, making every downstream null-check unreachable. **Only identity validates** — prologue
bytes, an exact vtable, a name.

### Verifying a signature before you trust it

A pattern is a claim. Check it against **both** builds — it must resolve *uniquely* on each, and on the
old build it must land on the RVA you extracted it from. "Unique on the build I'm looking at" is how you
ship a pattern that silently matches the wrong function elsewhere.

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
