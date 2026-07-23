/* dllmain.c -- the Snapmap+ BACKEND DLL bootstrap (our clean-room XINPUT1_3.dll).
 *
 * This is the backend's OWN DllMain -- the OG SnapHak loader/spine lived in XINPUT1_3.dll, and our
 * clone occupies the same load vector. It is SEPARATE from the fault-shield (a distinct proxy DLL with
 * a different signature). NO fault-shield / VEH / recovery code here.
 *
 * Bootstrap = resolve the DOOM module base -> run the signature resolver -> run the
 * smoke proof (resolver + inline-detour installer self-test) -> emit the "PB0: ..." line. Later stages wire the
 * real ops AFTER this point: the rawmap save/load swap, unhide, overrides shadow, the strids injector,
 * and the sh apply chain -- all riding on the signature-resolved engine fns + this installer.
 *
 * Clean-room: ported from our own RE of the OG hook-install + DLL architecture
 * + the reference implementation's signature table. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>                       /* SHGetFolderPathA + CSIDL_LOCAL_APPDATA (ensure_user_dirs) */
#pragma comment(lib, "shell32.lib")
#include "signatures.h"
#include "hook.h"
#include "smoke.h"
#include "rawmap.h"
#include "palette_guard.h"
#include "strids.h"
#include "overrides.h"
#include "commands.h"
#include "cvars.h"
#include "entity.h"
#include "typeinfo.h"
#include "patch.h"
#include "algo.h"
#include "target_any.h"   /* sh_target_any editor-decl visibility toggle (OG FUN_180021EE0 port) */
#include "wiring_cleandirect.h" /* sh_target_any wire-any: force the stock clean-direct connect branch (bind to any target ENTITY, no input radial) */
#include "ui_bridge.h"
#include "config.h"
#include "iface_engine.h"
#include "apply_engine.h"
#include "cvar_unlock.h"   /* merged-in cvar-unlock (former standalone dinput8) */
#include "backend_log.h"
#include "../fault_shield/fault_shield.h"   /* the merged fault-shield (recover-in-place vs OG's terminate) */
#include "../fault_shield/fault_record.h"   /* shield_set_logpath_from_module -> shield_faults.log */
#ifdef SH_DIAG
#include "../fault_shield/shield_diag.h"    /* DIAGNOSTIC build (build.ps1 -Diag): catch-all crash + env logger */
#endif

#define DOOM_MODULE_NAME "DOOMx64vk.exe"

static uint8_t *g_doom_base = NULL;
static size_t   g_doom_size = 0;

static void resolve_doom(void)
{
    g_doom_base = (uint8_t *)GetModuleHandleA(DOOM_MODULE_NAME);
    if (g_doom_base) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_doom_base;
        IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(g_doom_base + dos->e_lfanew);
        g_doom_size = nt->OptionalHeader.SizeOfImage;
    }
}

/* Deferred-resolution poll knobs. DOOMx64vk.exe is SteamStub-wrapped: the module is MAPPED early (so
 * GetModuleHandle succeeds + the PE headers/section table are readable from the first DLL-init tick),
 * but its `.text` is ENCRYPTED on disk and only decrypted in-memory when the SteamStub stub runs at the
 * exe entry point -- which is AFTER the loader's DLL-init that runs our DllMain. A resolver pass during
 * (or right after) DLL-init therefore scans STILL-ENCRYPTED bytes and matches 0 signatures. So we don't
 * resolve once at load: we POLL sig_resolve_all until the whole DB resolves uniquely (proof the real
 * decrypted engine code is now mapped) or we give up after a generous budget (the SteamStub-wrapped .text must be decrypted before the DB resolves). */
#define PB0_POLL_INTERVAL_MS  75
#define PB0_POLL_TIMEOUT_MS   60000

/* Create the %LOCALAPPDATA%\snapmap-plus\ user-data tree if absent. The disk-backed features (overrides,
 * rawmap swap/save, strids) and the prefab resolver all read/write under this folder but historically
 * ASSUMED it existed. On a fresh profile it is absent, so every one of those features silently no-ops --
 * e.g. the "unknown entity" override served from overrides\ never appears in-game. Create the tree once
 * at startup so a clean install works out of the box. (The installer scaffolds the same tree and folds a
 * pre-rebrand %USERPROFILE%\snaphak\ tree -- the original tool's path, which our pre-rename releases
 * reused -- forward into it; this is the runtime backstop for a hand-deployed overlay.) CreateDirectoryA
 * is idempotent (ERROR_ALREADY_EXISTS is benign); any other failure is logged non-fatally. Subfolders
 * mirror every <data-root>\<sub> path the backend builds. */
static void ensure_user_dirs(void)
{
    static const char *subs[] = { "", "\\strings", "\\overrides", "\\prefabs" };
    char base[MAX_PATH], path[MAX_PATH];
    int i;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        backend_log("WARN: SHGetFolderPathA(CSIDL_LOCAL_APPDATA) failed -- snapmap-plus user dirs not created");
        return;
    }
    for (i = 0; i < (int)(sizeof subs / sizeof subs[0]); i++) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus%s", base, subs[i]);
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            char line[MAX_PATH + 64];
            _snprintf_s(line, sizeof line, _TRUNCATE, "WARN: CreateDirectory %s failed (err %lu)",
                        path, GetLastError());
            backend_log(line);
        }
    }
    backend_log("snapmap-plus user-data dirs ensured (root + strings/overrides/prefabs)");
}

static DWORD WINAPI bootstrap_thread(LPVOID p)
{
    (void)p;
    /* Wait for the DOOM module to be mapped (present very early, but be defensive ~60s @ 10ms). */
    for (int i = 0; i < 6000 && g_doom_base == NULL; i++) {
        resolve_doom();
        if (!g_doom_base) Sleep(10);
    }
    if (g_doom_base == NULL) {
        backend_log("FATAL: DOOMx64vk.exe not found");
        return 0;
    }

    char line[128];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "backend attached base=%p size=%zx", (void *)g_doom_base, g_doom_size);
    backend_log(line);

    /* Create %LOCALAPPDATA%\snapmap-plus\{,strings,overrides,prefabs} if a fresh profile lacks it, so
     * overrides / rawmaps / strids / prefabs work on a clean install instead of silently no-opping
     * (the reason an end-user couldn't see the "unknown entity" override served from overrides\). */
    ensure_user_dirs();
    sh_config_init(); /* nonfatal: the service retains defaults and status flags on failure */

    /* Poll the resolver until the SteamStub has decrypted .text (full DB resolves uniquely) or we time
     * out. sig_resolve_all returns the count of UNIQUE resolves; the bar is the whole DB. While .text is
     * still encrypted this is 0 (or a stray partial), so we retry on a short interval. */
    size_t total = sig_db_count();
    DWORD  t0 = GetTickCount();
    size_t last_ok = 0;
    for (;;) {
        last_ok = sh_resolve_count(g_doom_base);
        if (last_ok == total) break;                       /* decrypted -- the real code is mapped */
        if (GetTickCount() - t0 >= PB0_POLL_TIMEOUT_MS) break;
        Sleep(PB0_POLL_INTERVAL_MS);
    }
    DWORD elapsed = GetTickCount() - t0;

    /* prove the foundation (resolver + inline-detour installer) end-to-end. Emits "PB0: ...". The
     * resolve is re-run inside (it's cheap) so the emitted counts/RVAs come from the same final scan;
     * `elapsed` annotates how long past load the decrypt took. */
    sh_smoke_run(g_doom_base, elapsed);

    /* the reusable PATCH/DETOUR layer self-test. Runs at install like the smoke proof, in-DLL, with
     * NO engine side effects -- it patches a SCRATCH RX stub only (apply / call-through / restore + the
     * negative refuse-on-mismatch that proves the verify-before-write guard). This ships the LAYER
     * (code_patch/code_unpatch + the detour reuse from hook.c, all sig-anchored + SEH-guarded); the engine
     * patch consumers (devmode 0x18a31d0, render-logging, cs_dontuse) come later. Emits
     * "B2: patch-layer self-test PASS ..." or a specific FAIL. See patch.c. */
    sh_patch_selftest();

    /* snaphak_algo: the in-DLL math self-test (like the patch-layer self-test -- in-DLL, NO engine
     * state). Runs the 4 clean-room ops (matmul/inverse/colorpack/curve) on known inputs + checks the
     * results (color-pack BIT-EXACT to the OG hook; the others f64-tolerant), proving the math independent
     * of the live engine BEFORE cs_dontuse ever installs a detour. Emits "B2: snaphak_algo self-test PASS
     * ..." or a specific FAIL. See algo.c. */
    sh_algo_selftest();

    /* Resolve the engine fns the feature ops ride on (from a final resolve pass -- the poll above
     * already proved the whole DB resolves): the rawmap save/load swap, the strids injector, the
     * OVERRIDES file-shadow, and the cvar + console-command registration. */
    {
        sig_result results[64];
        sig_resolve_all(g_doom_base, results, 64);   /* fills results[0..sig_db_count) by DB index */
        size_t db = sig_db_count();
        if (db > 64) db = 64;

        /* the rawmap LOAD swap (the keystone feature). Install the DeserializeFromJson detour as
         * soon as the engine fn is resolved -- it does NOT depend on the editor being up (the detour
         * just sits in front of the engine deserialize; it only swaps when ARMED + a source reads). We
         * pass the resolve STATUS so the installer refuses to patch over an already-hooked prologue
         * (SIG_OK_HOOKED) -- e.g. when an external instrumentation tool has hooked the same fn during testing.
         * The gate starts DISARMED; the test harness arms it for testing. */
        void *deser = NULL;
        int   deser_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "DeserializeFromJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    deser = (void *)results[i].addr;
                deser_clean = (results[i].status == SIG_OK);   /* clean scan, not the hook-tolerant fallback */
                break;
            }
        }
        sh_rawmap_swap_install(deser, deser_clean);

        /* the rawmap SAVE shadow (the INVERSE of the LOAD swap). Install the SerializeToJson
         * detour as soon as the engine fn is resolved -- like the LOAD swap it does NOT depend on the
         * editor being up (the detour sits in front of the engine serialize; on every save it calls the
         * engine original to fill the out-idStr, then mirrors that JSON to rawmap.json). No arm gate: OG
         * writes the shadow on EVERY save. Same clean-scan-only policy as the LOAD swap -- refuse to patch
         * over an already-hooked prologue (SIG_OK_HOOKED, e.g. an external instrumentation tool). See
         * rawmap.c. */
        void *serialize = NULL;
        int   serialize_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "SerializeToJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    serialize = (void *)results[i].addr;
                serialize_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        sh_rawmap_save_install(serialize, serialize_clean);

        /* the RELOAD-crash GUARD (palette_guard.c). After a heavy edit session (repeated create/delete of logic
         * entities), one entry in the editor's entity palette is left with a freed name string. On the next full
         * map-load the palette is sorted by name at entry, and copying that entry during the sort dereferences the
         * freed pointer -> access violation (fault region 0x19fca40). The map loads clean first, then the post-load
         * sort detonates -- a use-after-free, not a bad deserialize. The guard detours the palette-migration entry
         * (0x5ec6c0) and resets any dangling palette name string to empty BEFORE the sort copies it, so a clean
         * empty string is copied instead of the freed one. A valid entry is untouched; nothing is freed (the stale
         * buffer is simply never read). No editor/sig dependency (recipe-tagged RVA off the module base). See
         * palette_guard.c. */
        /* TEMP-DISABLED (shield-off diagnostic 2026-07-05): the render-node guard is one of OUR injected detours
         * and it WRITES into the render-node array -- disabled here to prove our own code isn't the corruptor.
         * RE-ENABLE by uncommenting. */
        /* sh_palette_guard_install(g_doom_base); */
        backend_log("rendernode-guard: TEMP-DISABLED (shield-off diagnostic build)");

        void *get_decls = (void *)sig_addr_by_name(results, db, "GetDeclsOfType");
        /* GetDeclsOfType is resolved here and handed to the command layer below (sh_commands_install),
         * where sh_listres + the material-lookup handlers walk the typed decl-manager node it returns.
         * The editor-palette expansion is driven by the OVERRIDES file-shadow (manifest- + port-bit-driven
         * palette enumeration), not by any decl-visibility bit flip. */

        /* the strids #str_ INJECTOR. Detour the engine idLangDict sort body (StridsSortBody) so
         * the first top-level sort first appends our #str_<id> rows from strings/strids.json into the
         * live string table, then runs the real sort. Like the rawmap swap, refuse to install over an
         * already-hooked prologue (pass the clean-scan STATUS). The other engine fns (the table-global
         * LEA anchor, the idList Append, the idStr hash, and the idStr pool ctor [DB name IdStrAssign,
         * the 0x1a03e10 fn]) are resolved by name from the same scan. See strids.c. */
        void *sort_body = NULL;
        int   sort_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "StridsSortBody") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    sort_body = (void *)results[i].addr;
                sort_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        void *strids_lea   = (void *)sig_addr_by_name(results, db, "StridsTableLea");
        void *strids_ins   = (void *)sig_addr_by_name(results, db, "StridsInsert");
        void *strids_hash  = (void *)sig_addr_by_name(results, db, "StridsHash");
        void *idstr_ctor   = (void *)sig_addr_by_name(results, db, "IdStrAssign");  /* 0x1a03e10 ctor */
        sh_strids_install(sort_body, sort_clean, strids_lea, strids_ins, strids_hash, idstr_ctor);

        /* the OVERRIDES FILE-SHADOW (port of OG FUN_18000b370 vtable-slot swap). NOT an inline
         * code detour -- it swaps the engine resource-provider's open-by-name VTABLE SLOT (+0xf8) with
         * our override-open. The vtable is .data (not sig-scannable), so we resolve the provider CTOR
         * (ResProviderCtor) by signature and the install decodes its `LEA RAX,[rip+vtable]` to recover
         * the vtable build-portably, then saves the slot's original + writes our hook into the slot.
         * Pass the resolve STATUS: the ctor is only used to DECODE the vtable LEA, so a hooked prologue
         * (SIG_OK_HOOKED) would corrupt the decode -- refuse on the hook-tolerant fallback. The shadow
         * is always-live once installed (no arm gate) and resolves THREE-LAYER: a user's
         * overrides/<name> file under %LOCALAPPDATA%\snapmap-plus\ -> our built-in default decls from memory
         * (the "*Custom" tab set; never written to disk) -> the engine's packaged resource. The user
         * layer is gated by the sh_user_overrides cvar (registered below; the loader's read is
         * registration-aware so pre-flush opens see the default 1). See overrides.c. */
        void *res_ctor = NULL;
        int   ctor_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "ResProviderCtor") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    res_ctor = (void *)results[i].addr;
                ctor_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        sh_overrides_install(res_ctor, ctor_clean);

        /* cvar + console-command registration (clone of OG XINPUT1_3 FUN_1800229b1). Both ride the
         * signature-resolved engine fns; neither installs an inline detour. CVARS FIRST -- they have NO
         * cmdSystem dependency and FIRE as soon as CvarRegister resolves (we only CALL the engine fn, so
         * SIG_OK and SIG_OK_HOOKED are both fine). COMMANDS need the idCmdSystemLocal* global, decoded
         * build-portably from the CmdSystemLea accessor's RIP-relative MOV (sh_resolve_cmdsys); they
         * degrade gracefully (cmdsys==NULL -> log + skip, no crash). get_decls (fetched above) is
         * passed to the command layer for sh_listres + the material lookups. See cvars.c / commands.c. */
        void *cvar_reg = (void *)sig_addr_by_name(results, db, "CvarRegister");
        sh_cvars_install(cvar_reg, g_doom_base);

        void *add_cmd  = (void *)sig_addr_by_name(results, db, "AddCommand");
        void *printf_d = (void *)sig_addr_by_name(results, db, "Printf");
        void *cmdsys   = sh_resolve_cmdsys(results, db, g_doom_base);
        sh_commands_install(add_cmd, cmdsys, printf_d, get_decls, g_doom_base);

        /* backend touch: AFTER `sh` is registered (sh_commands above registers the "sh" command),
         * create the shared UI-interface object + LoadLibraryA(".\\snapmap-plus\\snapmap-plus-ui.dll") +
         * CreateThread(sh_ui_init, &argblock{argc,argv,out-slot,interface}). This is the OG spine
         * tail of FUN_1800229b1 (the interface is built + handed to the frontend right after the AddCommand
         * spine; the OG loaded .\snaphak\snaphakui.dll / snaphak_ui_init -- same mechanism, our names).
         * Once this runs, `sh` stops reporting "Ui interface doesnt exist yet!" (it gates on the
         * interface sh_ui_get_iface returns). The interface + its register/unregister/drain bodies are the
         * generic factory in ../common/snapmap_plus_iface.c. See ui_bridge.c. */
        sh_ui_bridge_install();

        /* backend touch: resolve the heavy 8-pass apply-chain engine fns (entity-clone /
         * struct-serialize / tree-render / struct-deserialize / lexer / parse-node ctor+dtor / entity-def
         * ctor+dtor / decl-source-rebuild / idstr-assign -- all sig-resolved) + cache cmdSystem +
         * BufferCommandText/AddCommand for the clone_bss_apply command-buffer routing (FIX B). MUST run
         * BEFORE sh_iface_engine_install (which folds the apply-engine's three slot bodies into the single
         * sh_iface_bind_engine_slots call). The declMgr accessor is reused from sh_typeinfo, so this also
         * relies on g_doom_base being set (it is). See apply_engine.c. */
        sh_apply_engine_install(results, db, g_doom_base, cmdsys);

        /* backend touch: bind the UI-interface's engine-touch vtable slots -- the LIGHT touches
         * the SnapStack STORE-ops need (selection read/write, hovered id, toast, class/inherit read, id
         * validity/count) PLUS the heavy serialize/apply/read-prefab slots (+0xc8/+0xd0/+0xb8, folded
         * in from sh_apply_engine). The editor singleton is a hardcoded data RVA (0x3056748, like cmdSystem);
         * the selection/toast/idStr engine FNS are resolved by name from `results` (signature-based). AFTER
         * sh_ui_bridge_install (the interface + its shared vtable must exist) + sh_apply_engine_install (its
         * slot bodies must be ready). See iface_engine.c. */
        sh_iface_engine_install(results, db, g_doom_base);

        /* wire the entity/spawn handler deps (gameMgr global decoded via GameMgrLea,
         * cmdSystem reused from the sh_resolve_cmdsys decode above, SpawnByEntityDef from the sig DB).
         * The handlers themselves are already registered by the sh_commands CMD_TABLE; this only caches
         * their engine deps. AFTER sh_commands_install. See entity.c. */
        sh_entity_install(results, db, g_doom_base, cmdsys);

        /* wire the type-introspection handler deps (FindTypeInfoByName / FindEnumByName from
         * the sig DB; the declMgr accessor is the hardcoded RVA 0x17F7030 off g_doom_base, NOT sig-able --
         * resolved internally by sh_typeinfo_install, then vtable+0x80). The cs_fieldinfo/sh_type handlers
         * are already registered by the sh_commands CMD_TABLE; this only caches their engine deps. AFTER
         * sh_entity_install. See typeinfo.c. */
        sh_typeinfo_install(results, db, g_doom_base);

        /* snaphak_algo (cs_dontuse [18] + sh_alginfo): cache the DOOM module base so the cs_dontuse
         * TOGGLE can resolve the 4 AlgoMatMul/AlgoInverse/AlgoPackRGBA/AlgoCurveEval sigs at FIRE and
         * FULL-replace the engine math fns with our f64 reimpl (color-pack bit-exact). OFF BY DEFAULT --
         * installs NOTHING here; the cs_dontuse / sh_alginfo handlers are already registered by the
         * sh_commands CMD_TABLE. The 2nd sanctioned divergence (after the fault-shield) -- ON-state diverges
         * from OG's x87-80-bit in the last ULPs by design.
         * AFTER sh_typeinfo_install. See algo.c. */
        sh_algo_install(g_doom_base);

        /* sh_target_any: hand GetDeclsOfType (resolved above) to the editor-decl visibility toggle so its
         * handler can walk the idDeclSnapEditorEntity registry on demand. The handler is registered by the
         * sh_commands CMD_TABLE. Pair-for-pair port of OG SnapHak's sh_target_any (FUN_180021EE0). */
        sh_target_any_install(get_decls);

        /* sh_target_any wire-any (clone improvement over the original): detour the editor wire tool's two
         * connect creators (cdbb40/cdb990) so, while sh_target_any is revealed, a target that would raise the
         * "which input?" radial picker instead takes the tool's own clean-direct branch -- binding the wire to
         * the target ENTITY, no picker, no node mediation. Transient-flag technique; forces no slots, frees no
         * node (so none of the placeholder / stray-wire / "(no module)" artifacts). Off until reveal. */
        sh_wiring_cleandirect_install(g_doom_base);
    }

    /* FAULT-SHIELD (merged 2026-06-22): install the recover-in-place shield -- a first-in-chain VEH +
     * the idCommonLocal::Frame recovery hook -- AFTER the decrypt-poll above, so the shield's engine
     * sigs resolve on DECRYPTED .text. Was a separate winmm.dll proxy that DOOM's loader rejected at
     * load; rides the backend's PROVEN XINPUT1_3 load now. The sanctioned divergence
     * (recover-in-place vs OG's TerminateProcess). Blocks briefly on the instrumentation-coexistence wait. */
    /* RE-ENABLED 2026-07-05 after the shield-off diagnostic PROVED the fault-shield innocent (the create-timeline AV
     * 0xd32a39 fired identically with the shield off -> the engine's own crash dialog = the "freeze"). The real root
     * (a reclassed node-less timeline keeping the pasted command's stale render-node +0x70) is now fixed at the source
     * in ae_apply_one. The shield stays as the normal recover-in-place safety net. */
    shield_install(g_doom_base, g_doom_size);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        backend_set_logpath_from_module(hinst);   /* sh_backend.log under <DOOM>\snapmap-plus\logs\ */
        shield_set_logpath_from_module(hinst);     /* shield_faults.log under <DOOM>\snapmap-plus\logs\ (shield's own log) */
#ifdef SH_DIAG
        /* DIAGNOSTIC build only: arm the catch-all crash + environment logger FIRST, so it captures a
         * crash ANYWHERE (incl. outside the recovery shield's DOOM-only VEH, and __fastfail/heap faults).
         * Log-only -- never alters control flow. Writes sh_diag.log under <DOOM>\snapmap-plus\logs\. */
        shield_diag_install(hinst);
#endif
        /* Don't do engine work in DllMain (loader lock). Spin the bootstrap onto its own thread. */
        HANDLE h = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        /* Spawn the cvar-unlock on its own thread too (merged from the former standalone dinput8).
         * It self-resolves cvarSys via its own CmdSystemLea sig scan, independent of the bootstrap. */
        sh_cvar_unlock_start();
    }
#ifdef SH_DIAG
    else if (reason == DLL_PROCESS_DETACH) {
        shield_diag_detach();   /* DIAGNOSTIC: record crash-vs-clean-exit in sh_diag.log */
    }
#endif
    return TRUE;
}
