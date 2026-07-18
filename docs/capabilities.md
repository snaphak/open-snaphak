# Capabilities

What the clone does, grouped by kind. This is the user-facing feature inventory; for how the pieces
fit together see [`architecture.md`](architecture.md), and for the intentionally-faithful quirks
see [`fidelity.md`](fidelity.md).

## Console commands

Registered with the engine command system; run from the DOOM console.

| Command | What it does |
|---|---|
| `snapHak_rawmaps_on` | Enable raw map save/load (the `rawmap.json` substitution). |
| `snapHak_rawmaps_off` | Disable raw map save/load. |
| `sh` | The SnapStack dispatcher — routes the 20 SnapStack subcommands (below). |
| `sh_spawn` | Spawn `<entitydef> <name>` at the player and teleport to it. |
| `sh_dumpdef` | Print (and copy to clipboard) an existing entity's resolved entity definition. |
| `sh_spawninfo` | Generate `spawnOrientation` / `spawnPosition` from the current map position. |
| `sh_entlist` | List the editor entities. |
| `sh_listres` | List all resources of a given class (optionally copy to clipboard). |
| `sh_type` | Print the members of an idTech class or the values of an enum (runtime introspection). Add `-v` to also show each field's byte offset and size. |
| `sh_validclasses` | List the engine-valid classnames for a given inherit (the same enumerator that feeds the Classname dropdown). |
| `sh_target_any` | Reveal / re-hide the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette. |
| `sh_dumpmap` | Dump the current generated `.map` from memory (debugging). |
| `sh_genbmodel` | Generate a bmodel from a `.obj` / `.ase` / `.lwo`. |
| `sh_genmd6model` | Compile a `.md6model` into a `bmd6model`. |
| `sh_debugrender` | Renderer debug toggle (developer tool). |
| `sh_alginfo` | Report the math-acceleration status. |
| `sh_help` | List every SnapHak console command and cvar with its description in one place. |
| `sh_superscriptop` | Dump SuperScript / eventDef data (e.g. emit the eventDef table as a header). |
| `cs_dumpeventdefs` | Dump all eventDefs to a file. |
| `cs_fieldinfo` | Print field info for a type (developer tool). |
| `cs_dontuse` | Toggle the higher-precision engine-math overrides (a precision/perf tradeoff; off by default). |
| `cs_start_render_logging` | Set up the render-logging hook. |
| `snaphak_disable_devmode` | Turn developer features off while keeping Bethesda.net connectivity. |
| `snaphak_reenable_devmode` | Turn developer features back on. |
| `noClip` / `infiniteHealth` / `noPlayerDeath` / `noPlayerKill` / `noTarget` | The five player-cheat toggles for the local player: no-collision flight, infinite health, can't die, can't be killed, enemies ignore you. |

## Cvars

Console variables; defaults shown in parentheses.

| Cvar | What it does |
|---|---|
| `snaphak_copy_reslist_to_clipboard` (0) | Copy `sh_listres` output to the clipboard. |
| `snaphak_pretty_on` (0) | Pretty-print saved rawmap JSON. |
| `snaphak_show_rmcount` (0) | Draw the current number of active rendermodels. |
| `snaphak_user_overrides` (1) | When 0, your override files under `%USERPROFILE%\snaphak\overrides\` are ignored (built-in defaults and the game's own resources serve instead) — the bisect lever for a broken override set. Affects resources opened after the change; for boot-time coverage, rename the `overrides` folder instead. |
| `cs_dash_direction_multiplier` (1.0) | Scale dash direction. |
| `cs_dash_ground_velocity_multiplier` (2.0) | Scale dash direction when on the ground. |
| `cs_dash_time_seconds` (0.5) | Time period over which the dash is applied. |
| `cs_num_dash_slices` (120) | Number of slices used to apply dash velocity. |
| `cs_mh_direction_multiplier` (1.0) | Scale meathook direction. |
| `cs_mh_movement_multiplier` (10.0) | Scale meathook velocity. |

## SnapStack ops

Subcommands of `sh`, operating on numbered entity-id stacks and named groups. A numbered stack
is one-shot scratch — apply/use ops drain it; move a set into a named group (`pop2g`) to reuse it.

The SnapStack stores + all 20 handlers live in the **backend** (`src/backend/snapstack.c` +
`json_patch.c`) — the sole implementation, registered once before the frontend loads. See
[`backend-changes.md`](backend-changes.md) for the port's design + the deferred-apply/`json_patch` fixes.

**The 20 OG subcommands** (usage `sh <op> <stack> …`; a numbered stack index; several ops also accept a
letter-first **group name** in place of the stack index):

| Op | What it does |
|---|---|
| `psel` | Push the current editor selection onto the stack, then clear the selection. |
| `popsel` | Add the stacked ids (or a named group's) back into the editor selection. |
| `phov` | Push the hovered entity onto the stack (de-duplicated). |
| `pr` | Push every valid id in an inclusive `[lo..hi]` range. |
| `pg` | Push a named group's ids onto the stack. `sh pg <grp>` alone implies stack 0. |
| `pop2g` | Move the stack into a named group. `sh pop2g <grp>` alone implies stack 0. |
| `cstk` | Empty the stack. |
| `filtinh` | Keep only the stacked ids whose inherit matches the argument. |
| `filtcls` | Keep only the stacked ids whose classname matches the argument. |
| `bss` | Bulk-set a string property on the stacked entities. |
| `bsi` | Bulk-set an integer property. |
| `bsf` | Bulk-set a float property. |
| `bsb` | Bulk-set a boolean property. |
| `bse` | Pop the last id; set each remaining id's chosen property to that id's id-string. |
| `bsin` | Set the inherit of each stacked entity. |
| `bscls` | Set the classname of each stacked entity. |
| `bsincls` | Set the inherit, then the classname, of each stacked entity. |
| `accl` | Pop the last id as the receiver; reference-assign the remaining ids to it. |
| `acctargets` | Pop the last id; append the remaining ids to its `targets` list. |
| `mkcmd` | Synthesize a reusable command-entity macro from the stacked ids. |

**New SnapStack+ commands** (clone additions — not part of OG's 20). They read and manage the same
backend stores every SnapStack op runs against, so they always reflect live stack/group state. Output
goes to the `~` console with a summary toast.

| Op | What it does |
|---|---|
| `chkstk [N]` | Inspect stack `N` (ids + id-strings); omit `N` to summarize every non-empty stack. |
| `chkgrp [name]` | Inspect a group's ids; omit `name` to list all groups + counts. |
| `clrgrp <name>\|*` | Delete a named group entirely (`*` deletes all). |
| `snapstack_diag` | Report, per subcommand, which loaded DLL currently owns the handler. |

## The Studio window

The "SnapHak Studio" window — HTML/CSS/JS rendered in a WebView2 control, opened inside the SnapMap
editor (run `sh` in the console if it doesn't auto-open). Full detail: [`webview-ui.md`](webview-ui.md).

| Surface | What it does |
|---|---|
| Window shell | The Win32 host window + the manual 30 Hz think-loop; a menu bar with a light/dark theme toggle; the always-visible Camera-Origin bar (X/Y/Z track the live editor camera; "Lock Position" pins it). |
| Entities tab | A filterable entity list (multi-select, a hidden-entity toggle, two-way editor-selection sync; right-click for Copy ID / Delete / Push to stack 0 / Clear stack 0) plus the Entity State panel: classname / inherit / displayname fields and the Decl Text editor — line numbers, syntax coloring, structural lint, advisory schema checks, a distraction-free focus mode. "Save to Decl" commits the edits in memory. |
| Prefabs tab | Save and load selection prefabs as JSON files under `%USERPROFILE%\snaphak\prefabs\` — one folder level with rename/delete/drag-between-folders; per-prefab description + tags (stored in a `<name>.meta.json` sidecar; the filter box matches tags across folders); "Load / Place" stages the prefab and the user pastes it with Ctrl+V. |
| Timelines tab | The list of timeline entities; opening one edits its events and per-event parameters, with reference/decl/enum parameters constrained to valid choices, entity pickers for entity-typed args, and per-event documentation. |

## Hook behaviors

Engine detours and resource-loader shadows the backend installs.

| Hook | What it does |
|---|---|
| Rawmap load | When rawmaps are on, load the map from `%USERPROFILE%\snaphak\rawmap.json` instead of the engine's own save. |
| Rawmap save | On every editor save, mirror the serialized map JSON to `%USERPROFILE%\snaphak\rawmap.json`. |
| Overrides file-shadow | Three-layer resource resolution: your file under `%USERPROFILE%\snaphak\overrides\` wins, then the built-in default decls (served from memory — the "*Custom" tab set; never written to your folder, so they update with each release and deleting your file restores the default), then the game's packaged resource. Install logs an audit of your active overrides and reclaims untouched default copies written by earlier releases. |
| Strids injector | Inject custom `#str_` strings from `%USERPROFILE%\snaphak\strings\strids.json` into the engine string table (your key wins over a built-in default for the same id). |
| Fault handling | The clone replaces the original's two kill-detours (which terminate DOOM) with the recover-in-place fault-shield (see [`fidelity.md`](fidelity.md)). |
| SuperScript table | Merge the parked `cs_*` SuperScript objects into the engine's eventDef enumerate/lookup/dispatch paths. |
| Math acceleration | An optional SIMD/threading accelerator for engine math; not required for parity (a perf feature, not a user-facing one). |

## Also in the backend

Beyond the manifest entries above, the backend `XINPUT1_3.dll` also bundles **cvar-unlock**
(developer cvars and `+<cvar>` launch options), a fixed **XInput proxy** (controller input keeps
working, connected or not), and the resident **fault-shield**. See [`README.md`](../README.md)
and [`fidelity.md`](fidelity.md).

## Not user-facing

A few internals are present for parity but are not features you drive directly:

- The 5 parked `cs_*` SuperScript objects (`cs_invert_activator`, `cs_flyto_target`,
  `cs_test_equipped_weapon`, `cs_clone_activator`, `cs_dash`) are disabled gameplay-cheat objects
  in this build; their only live surface is the `cs_*` cvars above.
- `snaphak_ext` (a VFS / shader-compile library) is dormant, and `snaphak_algo` (SIMD + threading)
  is an optional performance accelerator — neither is required to match the original's behavior.
