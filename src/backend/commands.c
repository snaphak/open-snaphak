/* commands.c -- see commands.h. The console-command surface (clone of OG FUN_1800229b1's
 * AddCommand spine + handlers).
 *
 * Register the command NAMES; Tier B/C handlers are faithful "not yet implemented in
 * clone" stubs that print the OG help. snapHak_rawmaps_on/off are wired to the SHIPPED
 * sh_rawmap_swap_arm gate. sh_target_any is the editor-decl visibility toggle (target_any.c ->
 * h_target_any), a pair-for-pair port of OG SnapHak's own sh_target_any (FUN_180021EE0).
 * snaphak_algo (cs_dontuse [18] + sh_alginfo) now lives in algo.c -- cs_dontuse toggles the 4 f64
 * engine-math overrides, sh_alginfo reports the reimpl present; both extern-declared near CMD_TABLE.
 *
 * Clean-room: ported from our own RE (the verbatim command names/help read from the
 * OG XINPUT1_3.dll string table). Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "cvars.h"
#include "clipboard.h"
#include "typeinfo.h"          /* sh_typeinfo_get_declmgr() -- shared declMgr accessor for [12] */
#include "entlist_classes.h"
#include "patch.h"
#include "signatures.h"
#include "rawmap.h"
#include "ui_bridge.h"   /* sh_ui_get_iface() -- the `sh` dispatcher gates on the interface */
#include "hook.h"        /* install_inline_hook -- the AddCommand detour for the command unlock */
#include "backend_log.h"

/* ------------------------------------------------------------------------ engine fn typedefs ------ */

/* idCmdSystemLocal::AddCommand(cmdsys, name, handler, help, argComp, flags). DIRECT shape from
 * the AddCommand decompile @0x1aa3630 + the registrar @0x229b1; we pass argComp=NULL, flags=2 (see register_cmd: 2 -> stored 6
 * -> the command lands in the FULL *and* DEV tables + is dev-cheat-exempt, so the `~` console finds it even
 * in dev mode; the OG left param5/6 as register garbage = effectively 0 = FULL-only = dev-gated).
 *
 * SLOT MAP (engine 0x1aa3630 stores: cmd[0]=name=param_2, cmd[1]=handler=param_3, cmd[2]=param_5,
 * cmd[3]=param_4, cmd[4]=flags). We register help in param_4 (-> cmd[3]) -- this is FAITHFUL to OG SnapHak:
 * its own registrar (@0x229b1) calls AddCommand(cmdsys, name, handler, help) with the help string
 * as param_4 and nothing for param_5/param_6, and chrispy's commands display their help in-game -> cmd[3]
 * (param_4) IS the help-display slot the engine reads. (the reference implementation's clone_bss_apply uses the other order --
 * help in param_5, NULL in param_4 -- but it is an INTERNAL command never shown to users, so its help-slot
 * placement is immaterial; apply_engine.c matches the reference implementation there byte-for-byte and stays as-is.) So this
 * (cmdsys, name, handler, help, argComp=NULL, flags) order is correct for the user-facing OG command set;
 * load-bearing arg = handler=param_3, correctly placed everywhere. */
typedef void (*add_command_fn)(void *cmdsys, const char *name, void *handler,
                               const char *help, void *argComp, unsigned int flags);

/* idCommon message dispatch (Printf sig 0x1A08E80). OG's wrapper FUN_180006380 calls it as
 * (level=1, fmt, &va) -- a POINTER to the spilled varargs. We pre-format with _vsnprintf then call the
 * safe fixed-arg form dispatch(1, "%s", &bufptr) so we never re-derive the engine va layout. */
typedef void (*printf_dispatch_fn)(int level, const char *fmt, void *vaptr);

/* GetDeclsOfType(typeName) -> the typed decl-manager node (same engine fn sh_listres uses; sig
 * "GetDeclsOfType" @0x1800D20). Returns NULL for an unknown type. */
typedef void *(*get_decls_fn)(const char *type_name);

/* idCmdArgs + the shared SEH accessors + sh_printf + the global-decode scanner are declared in
 * commands.h (entity.c's moved handlers reuse them). */

/* ------------------------------------------------------------------------- module state ----------- */

static add_command_fn     g_add_command = NULL;
static void              *g_cmdsys      = NULL;
static printf_dispatch_fn g_printf      = NULL;
static void              *g_get_decls   = NULL;   /* cached for sh_listres + the material lookups */
static const uint8_t     *g_module_base = NULL;   /* DOOM module base (devmode resolves its sig at FIRE) */
static volatile LONG      g_installed   = 0;      /* one-shot install latch */

/* [15][16] devmode: ONE static restore-handle, gated on g_devmode_handle.live (static zero-init => .live==0
 * => "not currently disabled"). disable_devmode code_patch_sig's the SessionDevModeGetter head to
 * `xor eax,eax; ret`; reenable_devmode code_unpatch's it. The patch layer owns all the SEH/verify. */
static sh_patch_handle    g_devmode_handle;       /* zero-init: .live == 0 (not patched) */

/* ------------------------------------------------------------------------- Printf wrapper ---------
 * sh_printf(fmt, ...) -- format into a stack buffer, then dispatch(1, "%s", &bufptr). The engine's
 * idCommon::dispatch reads a POINTER to the args, so we pass the address of a single (char*) holding
 * our pre-formatted buffer -- exactly one %s consumed, no engine-va guesswork. */
void sh_printf(const char *fmt, ...)
{
    if (!g_printf) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    const char *p = buf;
    __try {
        g_printf(1, "%s", &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* a console dispatch fault must never take down the editor */
    }
}

/* SEH-guarded idCmdArgs accessors (the engine hands us the args object; never trust its shape).
 * Non-static -- declared in commands.h so entity.c's moved handlers share the SAME accessors. */
int cmd_argc(idCmdArgs *a)
{
    __try { return a ? a->argc : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
const char *cmd_argv(idCmdArgs *a, int n)
{
    __try { return (a && n >= 0 && n < a->argc) ? a->argv[n] : NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* ----------------------------------------------------------------- cmdSystem-global decode --------
 * The CmdSystemLea accessor (sig "CmdSystemLea" = the engine bot_add/bot_remove registrar) loads the
 * idCmdSystemLocal* global in its prologue via `MOV RCX,[rip+cmdSystem]` (48 8B 0D). Decode the FIRST
 * RIP-relative load opcode (the four forms 48 8D 0D / 48 8B 0D / 48 8D 05 / 48 8B 05 -- the cmdSystem
 * one is the MOV form, NOT the LEA sh_strids decodes) to the global SLOT, then DEREFERENCE ONCE to get
 * the live object (the MOV form loads *(slot); OG's *(engineBase+0x55b7280) does the same single deref).
 * Build-portable: no hardcoded RVA. Fallback: *(module_base + 0x55b7280) (the known offset). */
#define CMDSYS_KNOWN_RVA   0x55b7280u

/* SHARED with entity.c (declared in commands.h). The gameMgr-global decode reuses the EXACT same
 * 4-opcode RIP-relative scanner so the two globals decode through ONE code path (no duplicate-and-drift). */
int sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Decode a build-specific .data global SLOT address from a sig'd accessor fn whose prologue carries a
 * RIP-relative MOV/LEA to that global. Scans the first B2_RIP_SCAN_WINDOW bytes for the FIRST of the four
 * decode-target opcodes (48 8B 0D / 48 8B 05 / 48 8D 0D / 48 8D 05) and returns rip_next + disp32. Used by
 * BOTH sh_resolve_cmdsys (CmdSystemLea, the MOV-RCX form 48 8B 0D at offset 6) and sh_resolve_gamemgr
 * (GameMgrLea, the MOV-RAX form 48 8B 05 at offset 0). Returns the SLOT, NOT the dereferenced object. */
const uint8_t *sh_decode_rip_slot(const uint8_t *accessor_fn)
{
    uint8_t b[B2_RIP_SCAN_WINDOW];
    if (!sh_safe_read(accessor_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= B2_RIP_SCAN_WINDOW; i++) {
        /* 48 = REX.W; 8B = MOV r64,r/m64; 8D = LEA; modrm 0x0D = [rip+disp32]->RCX, 0x05 = ->RAX */
        if (b[i] == 0x48 && (b[i + 1] == 0x8B || b[i + 1] == 0x8D) &&
            (b[i + 2] == 0x0D || b[i + 2] == 0x05)) {
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = accessor_fn + i + 7;
            return rip_next + disp;
        }
    }
    return NULL;
}

void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base)
{
    /* Primary: decode the slot from the sig'd accessor, then deref once for the live object. */
    void *accessor = (void *)sig_addr_by_name(results, n, "CmdSystemLea");
    if (accessor) {
        const uint8_t *slot = sh_decode_rip_slot((const uint8_t *)accessor);
        if (slot) {
            void *obj = NULL;
            if (sh_safe_read(slot, (uint8_t *)&obj, sizeof obj) && obj) {
                char line[128];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cmdSystem decoded slot=%p -> obj=%p (portable)", (void *)slot, obj);
                backend_log(line);
                return obj;
            }
        }
        backend_log("B2: cmdSystem portable decode failed -- trying known-offset fallback");
    }
    /* Fallback (hook-tolerance philosophy: portable primary + known_rva fallback). */
    if (module_base) {
        const uint8_t *slot = module_base + CMDSYS_KNOWN_RVA;
        void *obj = NULL;
        if (sh_safe_read(slot, (uint8_t *)&obj, sizeof obj) && obj) {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cmdSystem fallback *(base+0x55b7280)=%p", obj);
            backend_log(line);
            return obj;
        }
    }
    backend_log("B2: cmdSystem UNRESOLVED -- commands cannot fire");
    return NULL;
}

/* ----------------------------------------------------------------------------- handlers ----------
 * The trivial handlers (wired to shipped ops); all others are faithful stubs. Each is __fastcall with
 * a single idCmdArgs* arg. Handlers run as a Cbuf callback on the engine main thread (console exec). */

/* [1] snapHak_rawmaps_on -> the SHIPPED sh_rawmap_swap_arm(1) gate (single source of truth). Prints the
 *     OG RUNTIME message "Enabling raw snapmap save/load." (the OG handler @0x21050), NOT the AddCommand help. */
static void h_rawmaps_on(idCmdArgs *a)
{
    (void)a;
    sh_rawmap_swap_arm(1);
    sh_printf("Enabling raw snapmap save/load.\n");
}
/* [2] snapHak_rawmaps_off -> sh_rawmap_swap_arm(0). Prints OG RUNTIME "Disabling raw snapmap save/load."
 *     (the OG handler @0x21070), NOT the AddCommand help. */
static void h_rawmaps_off(idCmdArgs *a)
{
    (void)a;
    sh_rawmap_swap_arm(0);
    sh_printf("Disabling raw snapmap save/load.\n");
}
/* [3] sh_alginfo -> algo.c (h_alginfo: reports our snaphak_algo reimpl PRESENT). [18] cs_dontuse ->
 * algo.c (h_cs_dontuse: the toggle that installs/uninstalls the 4 f64 math overrides). Both are
 * extern-declared near the CMD_TABLE (like the sh_entity / sh_typeinfo handlers). */

/* ----------------------------------------------------------------- decl-walk SEH helpers --------
 * sh_listres walks the decl-manager node layout (LIVE-VERIFIED: the decl-ptr array @
 * node+0x20, the count @ node+0x28. Each decl's NAME is a char* @ *decl+8 (the generic idDecl name slot
 * -- DIRECT from OG behavior: sh_listres passes *(*decl+8) as the Printf %s arg; it is an engine RUNTIME
 * offset not in the source-of-record, so LIVE-CONFIRM at FIRE: `sh_listres idMaterial` must print real
 * names). Every read is SEH-guarded; a wrong/garbage node degrades to a clean no-op, never a crash. */
#define LISTRES_ARRAY_OFF   0x20    /* decl-manager node -> decl-pointer array */
#define LISTRES_COUNT_OFF   0x28    /* decl-manager node -> decl count (uint) */
#define LISTRES_NAME_OFF    0x08    /* decl object -> name char* (*decl + 8) */
#define LISTRES_COUNT_CAP   (1u << 20)  /* stale-node guard */

static int lr_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int lr_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Read the decl name (*decl + 8), SEH-guarded; returns NULL if either hop is unreadable. */
static const char *lr_decl_name(const void *decl)
{
    __try { return *(const char *const *)((const uint8_t *)decl + LISTRES_NAME_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* A tiny growable byte buffer for the snaphak_copy_reslist_to_clipboard accumulation (each matched name
 * + '\n'). Heap-backed; freed by the caller. On any OOM the buffer goes "failed" and silently stops
 * accumulating (the console print still happens -- the clipboard copy just won't include the overflow). */
typedef struct lr_buf {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} lr_buf;

static void lr_buf_append(lr_buf *b, const char *s)
{
    if (b->failed || s == NULL) return;
    size_t add = strlen(s) + 1;                 /* the name + a '\n' */
    if (b->len + add + 1 > b->cap) {            /* +1 for the final NUL */
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + add + 1) ncap *= 2;
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) { b->failed = 1; return; }
        b->data = nd;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, s, add - 1);
    b->len += add - 1;
    b->data[b->len++] = '\n';
    b->data[b->len]   = '\0';
}

/* [14] sh_listres <type> [filter] -- GetDeclsOfType(type), walk the decl array, print each name; if
 * argv[2] is present, substring-filter; if snaphak_copy_reslist_to_clipboard is set, accumulate the
 * matched names and copy the list to the clipboard at the end. Clone of OG FUN_180022000
 * (its decompile @0x22000 + our listres-mechanism notes). */
static void h_sh_listres(idCmdArgs *a)
{
    const char *type   = cmd_argv(a, 1);
    const char *filter = cmd_argv(a, 2);        /* NULL => no filter (OG: argc<3) */

    if (type == NULL) {                          /* OG silently returns; we add a usage line */
        sh_printf("usage: sh_listres <resource classname (ex:idMaterial)> [filter]\n");
        return;
    }
    if (!g_get_decls) {
        sh_printf("sh_listres: GetDeclsOfType unresolved -- cannot list.\n");
        return;
    }

    void *list = ((get_decls_fn)g_get_decls)(type);
    if (list == NULL) {
        sh_printf("sh_listres: no decls of type '%s'.\n", type);
        return;
    }

    void    *array = NULL;
    uint32_t count = 0;
    if (!lr_read_ptr((const uint8_t *)list + LISTRES_ARRAY_OFF, &array) ||
        !lr_read_u32((const uint8_t *)list + LISTRES_COUNT_OFF, &count)) {
        sh_printf("sh_listres: decl list array/count unreadable.\n");
        return;
    }
    if (array == NULL || count == 0) {
        sh_printf("sh_listres: 0 decls of type '%s'.\n", type);
        return;
    }
    if (count > LISTRES_COUNT_CAP) {            /* stale/garbage node guard */
        sh_printf("sh_listres: decl count implausible (stale manager node?).\n");
        return;
    }

    int clip = sh_cvar_value_int(B2_CVAR_SNAPHAK_COPY_RESLIST_TO_CLIPBOARD, 0);
    lr_buf buf = { NULL, 0, 0, 0 };

    uint32_t printed = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl = NULL;
        if (!lr_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl)) break;  /* array tail AV */
        if (decl == NULL) continue;

        const char *name = lr_decl_name(decl);
        if (name == NULL) continue;
        if (filter != NULL && strstr(name, filter) == NULL) continue;            /* substring filter */

        sh_printf("%s\n", name);
        printed++;
        if (clip) lr_buf_append(&buf, name);
    }

    if (clip && buf.data != NULL && buf.len > 0) {
        if (sh_clipboard_set(buf.data))
            sh_printf("sh_listres: copied %u name(s) to the clipboard.\n", printed);
    }
    free(buf.data);
}

/* [5] sh_entlist [filter] -- list every idEntity-derived class NAME; if argv[1] is present, substring-filter.
 * Clone of OG FUN_180021b50. OG walked a STATIC ptr-table (its own hardcoded snapshot of the idEntity subclass
 * set); we RE'd that this snapshot IS the engine's idEntity-derived reflection walk (our idEntity-derived live
 * set reproduces OG's 892 STRING-FOR-STRING). So the
 * clone enumerates the LIVE type registry (sh_typeinfo_collect_classnames) and keeps every class that derives
 * from idEntity -- byte-identical to OG's list on this build BUT portable (auto-tracks DOOM patches) AND it
 * surfaces any decl-less idEntity class OG's frozen snapshot happened to miss. Two deliberate divergences from
 * OG: (1) we do NOT skip idTarget_Command (OG hid it; the user wants it listed -- it is a real, makeable
 * idEntity class); (2) a trailing count line. Fallback: if the live registry is unreachable (pre-boot), walk
 * the static B2_ENTLIST_CLASSES snapshot. The idEntity-derive filter naturally excludes non-entity types
 * (components / managers / structs) the raw registry also holds. */
#define SH_ENTLIST_MAX  16384   /* candidate-buffer cap (this build ~10,190 registered types) */
static void h_sh_entlist(idCmdArgs *a)
{
    const char *filter = cmd_argv(a, 1);        /* NULL => no filter (OG: argc<=1) */

    static const char *names[SH_ENTLIST_MAX];   /* main-thread-serial console handler -> static is safe */
    int printed = 0;
    int k = sh_typeinfo_collect_classnames(names, SH_ENTLIST_MAX);
    if (k > 0) {                                 /* LIVE registry: keep idEntity subclasses (excl. idEntity base) */
        for (int i = 0; i < k; i++) {
            const char *name = names[i];
            if (name == NULL || strcmp(name, "idEntity") == 0) continue;          /* list SUBCLASSES (OG-faithful) */
            if (sh_typeinfo_class_derives(name, "idEntity") != 1) continue;       /* keep only idEntity-derived */
            if (filter != NULL && strstr(name, filter) == NULL) continue;         /* substring filter */
            sh_printf("%s\n", name);
            printed++;
        }
        if (k >= SH_ENTLIST_MAX)
            sh_printf("(registry list truncated at %d -- raise SH_ENTLIST_MAX)\n", SH_ENTLIST_MAX);
    } else {                                     /* fallback: the static idEntity-subclass snapshot */
        for (int i = 0; i < B2_ENTLIST_CLASS_COUNT; i++) {
            const char *name = B2_ENTLIST_CLASSES[i];
            if (filter != NULL && strstr(name, filter) == NULL) continue;         /* substring filter */
            sh_printf("%s\n", name);
            printed++;
        }
    }
    sh_printf("(%d entity classes%s%s)\n", printed,
              filter ? " matching " : "", filter ? filter : "");
}

/* ----------------------------------------------------------------- [15][16] devmode -------------
 * FIRST live engine-code patch. SnapHak's snaphak_disable_devmode stomps the idSessionLocal devmode bool
 * getter (engine 0x18a31d0: movzx eax,[rcx+0x34c89]; ret) so it always returns 0; reenable restores it.
 * We ride the sh_patch layer EXACTLY (code_patch_sig / code_unpatch + the static restore-handle), and
 * resolve the SessionDevModeGetter site by SIGNATURE at FIRE (not a hardcoded RVA) -- version-portable, and
 * a sig miss/ambiguity makes code_patch_sig REFUSE (no write) on a shifted build rather than mis-patch.
 *
 * code_patch overwrites only the 3-byte HEAD (0F B6 81 -> 31 C0 C3 = `xor eax,eax; ret`); bytes 3-7 of the
 * original getter are never touched, so code_unpatch restores the full original instruction and the sig
 * re-resolves on the restored bytes -> repeatable disable/reenable. */
#define DEVMODE_SIG_NAME   "SessionDevModeGetter"

/* Resolve a named engine site from the shipped sig DB (mirrors sh_cvars' NameHash resolve: iterate
 * BACKEND_ENGINE_SIGNATURES, sig_resolve_one over g_module_base). Fills *out; returns 1 if the entry was
 * found in the DB (then *out carries the resolve status, which the code_patch_sig / install_detour_sig
 * gates check), 0 if the name isn't in the DB or no module base is cached. Used by BOTH the [15][16]
 * devmode patch (SessionDevModeGetter) and the [11] render-logging detour (RenderLogStub). */
static int resolve_sig_by_name(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* [15] snaphak_disable_devmode -- patch the session devmode getter to return 0 (devmode off). */
static void h_disable_devmode(idCmdArgs *a)
{
    (void)a;
    if (g_devmode_handle.live) {
        sh_printf("devmode already disabled\n");
        return;
    }
    sig_result r;
    if (!resolve_sig_by_name(DEVMODE_SIG_NAME, &r)) {
        sh_printf("snaphak_disable_devmode: %s not in the signature DB -- cannot patch.\n", DEVMODE_SIG_NAME);
        return;
    }

    /* expect = the 3-byte head the sig already verified (0F B6 81); new = `xor eax,eax; ret` (31 C0 C3).
     * code_patch_sig REFUSES unless the resolve was a clean unique SIG_OK hit. */
    const uint8_t expect[3]    = { 0x0F, 0xB6, 0x81 };
    const uint8_t new_bytes[3] = { 0x31, 0xC0, 0xC3 };
    sh_patch_status st = code_patch_sig(&r, expect, new_bytes, 3, &g_devmode_handle);
    if (st == B2_PATCH_OK)
        sh_printf("snaphak_disable_devmode: %s -- devmode disabled (session getter -> 0)\n",
                  sh_patch_status_str(st));
    else
        sh_printf("snaphak_disable_devmode: %s -- patch refused, devmode unchanged\n",
                  sh_patch_status_str(st));
}

/* [16] snaphak_reenable_devmode -- restore the session devmode getter (undo the disable patch). */
static void h_reenable_devmode(idCmdArgs *a)
{
    (void)a;
    if (!g_devmode_handle.live) {
        sh_printf("devmode not currently disabled\n");
        return;
    }
    sh_patch_status st = code_unpatch(&g_devmode_handle);
    if (st == B2_PATCH_OK)
        sh_printf("snaphak_reenable_devmode: %s -- devmode re-enabled (getter restored)\n",
                  sh_patch_status_str(st));
    else
        sh_printf("snaphak_reenable_devmode: %s -- restore failed\n", sh_patch_status_str(st));
}

/* ----------------------------------------------------------------- [11] cs_start_render_logging ---
 * FIRST live engine-code DETOUR (the detour layer's first real consumer). SnapHak's
 * cs_start_render_logging (OG FUN_1800224c0) opens renderlog.txt and detours the engine's render-debug
 * TRACE SINK (RenderLogStub @0xd99dc0 = `mov [rsp+0x20],r9; ret`, a no-op when logging is off) with a hook
 * that writes the engine's printf trace lines to the file. The engine hands the sink a fully-formatted
 * printf fmt + varargs ("Source stages %s -> Dest stages: %s\n", etc.), so the hook reads ZERO renderer
 * internals -- it just vfprintf's fmt+va to the log. The original sink was a no-op, so the hook does NOT
 * trampoline. Start-only, process-lifetime (mirrors OG: no stop command; teardown at DLL detach is
 * optional). We resolve RenderLogStub by SIGNATURE at FIRE (version-portable) and ride the
 * sh_install_detour_sig (SIG_OK-gated, SEH-guarded, reversible). */
#define RENDERLOG_SIG_NAME   "RenderLogStub"
#define RENDERLOG_STOLEN     14   /* hook.c writes a 14-byte FF25 abs-jmp + requires stolen>=14; 14<=16 room */

static FILE *g_renderlog_fp    = NULL;
static void *g_renderlog_tramp = NULL;

/* SEH-guarded vfprintf to the render log -- factored out of our_renderlog_hook because MSVC forbids
 * mixing va_start/va_end with __try/__except in the SAME function (it inserts a frame-unwind filter the
 * varargs prologue conflicts with). This helper takes the already-started va_list; a fault while the
 * engine's varargs/fmt are malformed degrades to a clean no-write, never a crash that takes down the
 * renderer. */
static void renderlog_write(FILE *fp, const char *fmt, va_list ap)
{
    __try {
        vfprintf(fp, fmt, ap);
        fflush(fp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* a malformed engine trace line must never fault the render thread */
    }
}

/* The detour hook installed over RenderLogStub. The engine calls the sink as
 * (ctx=RCX, channel=RDX, fmt=R8, ...va); we IGNORE ctx+channel and write fmt + the varargs to
 * renderlog.txt. Reads ZERO renderer internals (the engine supplies a fully-formatted fmt+varargs); does
 * NOT call a trampoline (the original sink was a no-op). */
static void our_renderlog_hook(void *ctx, void *channel, const char *fmt, ...)
{
    (void)ctx;
    (void)channel;
    if (!g_renderlog_fp || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    renderlog_write(g_renderlog_fp, fmt, ap);
    va_end(ap);
}

/* [11] cs_start_render_logging -- open renderlog.txt + detour the engine render-debug trace sink so its
 * printf lines are logged. Start-only (mirrors OG: no stop command). */
static void h_cs_start_render_logging(idCmdArgs *a)
{
    (void)a;
    if (g_renderlog_fp) {
        sh_printf("render logging already started\n");
        return;
    }
    if (fopen_s(&g_renderlog_fp, "renderlog.txt", "w") != 0 || g_renderlog_fp == NULL) {
        g_renderlog_fp = NULL;
        sh_printf("could not open renderlog.txt\n");
        return;
    }
    sh_printf("Opening renderlog renderlog.txt\n");

    sig_result r;
    if (!resolve_sig_by_name(RENDERLOG_SIG_NAME, &r)) {
        sh_printf("cs_start_render_logging: %s not in the signature DB -- cannot hook.\n", RENDERLOG_SIG_NAME);
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
        return;
    }

    g_renderlog_tramp = sh_install_detour_sig(&r, (void *)our_renderlog_hook, RENDERLOG_STOLEN);
    if (g_renderlog_tramp == NULL) {
        sh_printf("cs_start_render_logging: detour install refused/failed (%s status=%d) -- logging off.\n",
                  RENDERLOG_SIG_NAME, (int)r.status);
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
        return;
    }
    sh_printf("cs_start_render_logging: render-log hook installed.\n");
}

/* ---- Tier B/C STUBS: register the NAME, print the OG help + a "not yet implemented" note. ----
 * Each closes over its own help via a small per-command wrapper produced by the X-macro below. The
 * stub is faithful surface (the command STOPS returning "Unknown command") without claiming behavior. */
#define STUB_HANDLER(fn, help_text)                                              \
    static void fn(idCmdArgs *a) {                                              \
        (void)a;                                                                \
        sh_printf("%s\n(not yet implemented in clone)\n", help_text);          \
    }

/* ============================================================ WS-C deferred dev/asset commands ====
 * [20] sh_genmd6model / [19] sh_genbmodel / [17] sh_debugrender -- OG chrispy dev/asset tools. Real ports
 * of OG XINPUT1_3 FUN_18000b560 / FUN_18000b4a0 / FUN_18001ffe0. Every engine fn is resolved by SIGNATURE
 * off the live DOOM module (resolve_sig_by_name over g_module_base, the same path devmode/renderlog use) --
 * NO hardcoded base+RVA. Every engine touch is SEH-guarded (these are heavy/faultable asset compilers + a
 * runtime renderWorld vtable). [17] ports only the SAFE READ-ONLY sub-ops; its 2 genuinely-harmful sub-ops
 * (loadimg_n_break = INT3 debugger trap; dump_megatex = hardcoded fwrite to C:\Users\Chris\megatex.raw) are
 * routed to a clear refusal, NOT reproduced bug-for-bug. */

/* ---- engine fn typedefs for the asset-gen call-targets (resolved by sig at FIRE) ---------------- */
typedef void *(*default_idstr_ctor_fn)(void *self);                    /* DefaultIdStrCtor 0x19fd040 */
typedef void *(*md6_ctor_fn)(void *md6);                               /* Md6Ctor 0x149b8d0 */
typedef void  (*md6_setoutput_fn)(void *md6, void *output_idstr);      /* Md6SetOutput 0x149c450 */
typedef void  (*md6_build_fn)(void *md6);                              /* Md6Build (final call) 0x149bee0 */
typedef void  (*idstr_assign2_fn)(void *dstField, const char *cstr);   /* IdStrAssign 0x1a03e10 */
typedef void *(*idstr_ctor2_fn)(void *self, const char *cstr);         /* IdStrCtor 0x19fcef0 */
typedef void  (*idstr_dtor2_fn)(void *self);                           /* IdStrDtor 0x19fd120 */
typedef void  (*bmodel_builder_fn)(void *out208, const char *input,    /* BModelBuilder 0x14cf550 */
                                   const char *output, void *opts);

/* ---- SEH-guarded single-call wrappers (the engine fns are heavy/faultable; never let a fault out) --
 * Each resolves the named sig at FIRE (build-portable) and invokes under __try. Returns 1 on a ran call,
 * 0 if the sig is missing/unresolved or the call faulted. MSVC forbids mixing C++ object unwinding with
 * __try in one fn, but these are plain C fn-ptr calls so the guard is clean. */
static void *eng_default_idstr_ctor(void *self)
{
    sig_result r;
    if (!resolve_sig_by_name("DefaultIdStrCtor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return NULL;
    __try { return ((default_idstr_ctor_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *eng_idstr_ctor_copy(void *self, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrCtor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return NULL;
    __try { return ((idstr_ctor2_fn)r.addr)(self, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int eng_idstr_assign(void *dstField, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrAssign", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((idstr_assign2_fn)r.addr)(dstField, cstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void eng_idstr_dtor(void *self)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrDtor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return;
    __try { ((idstr_dtor2_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* dtor fault -> leak, never crash */ }
}
static int eng_md6_ctor(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Ctor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_ctor_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_setoutput(void *md6, void *output_idstr)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6SetOutput", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_setoutput_fn)r.addr)(md6, output_idstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_build(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Build", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_build_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_bmodel_builder(void *out208, const char *input, const char *output, void *opts)
{
    sig_result r;
    if (!resolve_sig_by_name("BModelBuilder", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((bmodel_builder_fn)r.addr)(out208, input, output, opts); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* idStr / md6 / bmodel-result stack-object sizes -- over-allocated vs the OG's exact frame slots so a
 * build-shifted object layout can never overrun. OG frames: [20] opts idStr 288B, input idStr 88B, output
 * idStr 48B, md6 ctx; [19] opts idStr 48B, out208 result 208B. We round each generously. */
#define GEN_IDSTR_BYTES   128     /* an idStr (inline buf cap 0x14 + header) -- OG slots 48..288B; 128 covers it */
#define GEN_MD6_BYTES     1024    /* the idMd6Builder ctx (ctor inits md6+0x38..+0x110; build writes +0xf8) */
#define GEN_BMODEL_BYTES  256     /* the BModelBuilder out208 result struct (OG memsets/uses 208 = 0xD0 bytes) */
#define GEN_BOPTS_BYTES   0xD0    /* the bmodel options struct: OG memset(opts,1,0xD0). RECIPE-TAG (build-
                                   * specific): re-confirm 0xD0 on a build bump by tracing how 0x14cf600 reads
                                   * R9 (BModelBuilder sig comment). The clone memsets a 0xD0 buffer to 0x01. */

/* [20] sh_genmd6model <input> <output> -- compile a .md6model into a bmd6model. Real port of OG
 * FUN_18000b560. OG ORDER (DIRECT, from its decompile @0xb560): gate argc>2 (>=3); DefaultIdStrCtor(opts);
 * Md6Ctor(&md6); IdStrAssign(input, argv[1]); IdStrCtor(output, argv[2]); Md6SetOutput(&md6, output);
 * IdStrDtor(output); Md6Build(&md6); IdStrDtor(opts). The OG ctor's a default opts idStr it never passes
 * to the call chain (a scratch the md6 ctx already owns at md6+0x60) -- we ctor+dtor it faithfully to
 * mirror the OG frame lifecycle. Md6Build is destructor-shaped (compile-then-release, the bmd6 buffer
 * write folded into the dtor -- see the Md6Build sig comment). Every engine call SEH-guarded. */
static void h_sh_genmd6model(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {     /* OG gate: 2 < argc */
        sh_printf("sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model\n");
        return;
    }

    /* Zero-init the stack objects so a sig miss / partial ctor leaves defined (dtor-safe) memory. */
    unsigned char opts[GEN_IDSTR_BYTES];   memset(opts,   0, sizeof opts);
    unsigned char md6 [GEN_MD6_BYTES];     memset(md6,    0, sizeof md6);
    unsigned char inS [GEN_IDSTR_BYTES];   memset(inS,    0, sizeof inS);
    unsigned char outS[GEN_IDSTR_BYTES];   memset(outS,   0, sizeof outS);

    if (!eng_default_idstr_ctor(opts)) {
        sh_printf("sh_genmd6model: idStr ctor unresolved -- cannot compile.\n");
        return;
    }
    if (!eng_md6_ctor(md6)) {
        sh_printf("sh_genmd6model: md6 builder unresolved -- cannot compile.\n");
        eng_idstr_dtor(opts);
        return;
    }
    /* input/output idStrs. IdStrAssign sets the input field; IdStrCtor copies the output C-string. */
    if (!eng_idstr_assign(inS, input) || !eng_idstr_ctor_copy(outS, output)) {
        sh_printf("sh_genmd6model: idStr assign/ctor unresolved -- cannot compile.\n");
        eng_idstr_dtor(opts);
        return;
    }
    if (!eng_md6_setoutput(md6, outS)) {
        sh_printf("sh_genmd6model: md6 SetOutput unresolved/faulted.\n");
        eng_idstr_dtor(outS);
        eng_idstr_dtor(opts);
        return;
    }
    eng_idstr_dtor(outS);                  /* OG dtors output right after SetOutput */

    int built = eng_md6_build(md6);        /* the final md6 call (compile-then-release) */
    eng_idstr_dtor(opts);                  /* OG dtors opts last */

    if (built)
        sh_printf("sh_genmd6model: compiled '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genmd6model: md6 build unresolved/faulted ('%s').\n", input);
}

/* [19] sh_genbmodel <input> <output> -- generate a bmodel from a .obj/.ase/.lwo. Real port of OG
 * FUN_18000b4a0. OG ORDER (DIRECT, from its decompile @0xb4a0): gate argc>2; memset(opts,1,0xD0); DefaultIdStrCtor(s);
 * BModelBuilder(out208, argv[1]=input, argv[2]=output, &opts); IdStrDtor(s). The default idStr `s` is a
 * scratch the OG ctor's + dtor's around the call (not passed to BModelBuilder) -- mirror its lifecycle.
 * Every engine call SEH-guarded; the out208 result + the 0xD0 opts buffer are stack-local + zero-init. */
static void h_sh_genbmodel(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {     /* OG gate: 2 < argc */
        sh_printf("sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file.\n");
        return;
    }

    /* OG (cmd_0xb4a0): ONE 0xD0 struct memset to 0x01 is BModelBuilder arg1/RCX (the out/result struct); ONE
     * default-ctor'd idStr is arg4/R9. There is NO separate options buffer -- the 0x01-memset IS arg1. (The
     * first port inverted arg1/arg4: it passed a 0-memset buffer as arg1 + the 0x01 buffer as arg4, leaving
     * the idStr unused. Fixed to match OG byte-for-byte.) */
    unsigned char out208[GEN_BOPTS_BYTES]; memset(out208, 0x01, sizeof out208);  /* OG: memset(auStack_e8, 1, 0xd0) -> arg1 */
    unsigned char optsS [GEN_IDSTR_BYTES]; memset(optsS,  0,    sizeof optsS);    /* OG auStack_118: the idStr -> arg4 */

    if (!eng_default_idstr_ctor(optsS)) {
        sh_printf("sh_genbmodel: idStr ctor unresolved -- cannot generate.\n");
        return;
    }
    int built = eng_bmodel_builder(out208, input, output, optsS);  /* OG: BModelBuilder(auStack_e8, input, output, auStack_118) */
    eng_idstr_dtor(optsS);                  /* OG dtors the idStr after the build */

    if (built)
        sh_printf("sh_genbmodel: generated bmodel '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genbmodel: bmodel builder unresolved/faulted ('%s').\n", input);
}

/* ----------------------------------------------------------------- [17] sh_debugrender -------------
 * Real port of OG FUN_18001ffe0 (dispatches argv[1] across 9 sub-ops). The OG reads renderWorld =
 * *(engineBase+0x57216f0) -- a .data SLOT. We resolve it BUILD-PORTABLY: the RenderWorldGetter sig anchors a
 * unique engine window carrying `LEA RCX,[rip+slot]`; sh_decode_rip_slot decodes it to the slot RVA, then we
 * deref once for the live idRenderWorld*. Fallback: *(g_module_base + 0x57216f0). The editor singleton (for
 * showcursor) is the SAME recipe-tagged data RVA sh_iface_engine uses (module_base + 0x3056748).
 *
 * PORTED (safe, read-only): dumprenderinfo (=OG dumpmodelinfo: walk the rendermodel list, Printf each name),
 * showcursor (write byte[editor+0x23624]=0), togglefpsupdate (cosmetic flag toggle -- clone-local state),
 * showmaterial / drawmatarg (GetDeclsOfType("idMaterial") lookups -- decl reads, no mutation).
 * REFUSED (clear toast, NOT bug-for-bug -- genuinely harmful): loadimg_n_break (ends in INT3 = a debugger
 * trap that halts the game), dump_megatex (hardcoded fwrite to C:\Users\Chris\megatex.raw, chrispy's box).
 * NOT-AVAILABLE (heavy dev-only mutators, faithfully surfaced but not ported): test_rm_commit, test_sum_shit,
 * testnewgui (render-commit / geoworld-build / GUI-alloc -- out of the safe read-only scope). */
#define RW_SLOT_KNOWN_RVA         0x57216f0u  /* renderWorld .data slot (OG *(engineBase+RVA)); fallback only */
/* RE-DERIVE RECIPE for the 4 BUILD-SPECIFIC offsets below (do per DOOM build -- portability discipline; these
 * are vtable-slot/struct-field offsets, NOT sig-resolvable). All four come from TWO command-handler decompiles:
 *   - The renderWorld vtbl slots + the model-name offset: decompile the OG `dumpmodelinfo` handler (find via the
 *     AddCommand("dumpmodelinfo") registration xref, or its Printf format string). It does
 *     `n = rw->vtbl[RW_VSLOT_MODEL_COUNT]()` then loops `m = rw->vtbl[RW_VSLOT_GET_MODEL](i);
 *     name = *(char**)(m + RW_MODEL_NAME_OFF)` -> read the two `call qword[rax+0xNN]` vtbl offsets + the
 *     `mov rcx,[model+0xNN]` name offset straight off the decompile.
 *   - ED_SHOWCURSOR_OFF: decompile the OG `showcursor` handler -> `*(uint8*)(editor + 0xNN) = 0`; the editor base
 *     is EDITOR_SINGLETON_RVA below (already recipe-tagged). Re-derive by decompiling the handler (<handlerRVA>) on the new build.
 * A wrong offset here degrades to a bad read on dev-only console cmds (SEH-guarded), never a crash. */
#define RW_VSLOT_MODEL_COUNT      0x188       /* renderWorld vtbl -> GetActiveRenderModelCount() -> uint (BUILD-SPECIFIC) */
#define RW_VSLOT_GET_MODEL        0x190       /* renderWorld vtbl -> GetRenderModel(idx) -> model* (=400; BUILD-SPECIFIC) */
#define RW_MODEL_NAME_OFF         0x10        /* render model -> name char* (model+0x10) (BUILD-SPECIFIC) */
#define ED_SHOWCURSOR_OFF         0x23624u    /* editor -> showcursor byte (OG writes 0) (BUILD-SPECIFIC) */
#define RW_MODEL_COUNT_CAP        1000000u    /* stale-renderWorld guard on the model count */
#define EDITOR_SINGLETON_RVA      0x3056748u  /* inline idSnapEditorLocal object = module_base + this (SAME recipe
                                               * as iface_engine.c EDITOR_SINGLETON_RVA; in-place ctor 0x51A8E0;
                                               * re-derive per build). showcursor writes byte[editor+0x23624]=0. */

/* GetDeclsOfType typedef already declared above (get_decls_fn); reuse it for the material lookups. */

/* Resolve the live idRenderWorld* build-portably: decode the RenderWorldGetter sig's RIP slot, deref once;
 * fallback to *(module_base + 0x57216f0). Returns NULL if neither yields a readable non-NULL pointer. */
static void *dr_resolve_renderworld(void)
{
    sig_result r;
    if (resolve_sig_by_name("RenderWorldGetter", &r) &&
        (r.status == SIG_OK || r.status == SIG_OK_HOOKED) && r.addr != 0) {
        const uint8_t *slot = sh_decode_rip_slot((const uint8_t *)r.addr);
        if (slot) {
            void *rw = NULL;
            if (sh_safe_read(slot, (uint8_t *)&rw, sizeof rw) && rw) return rw;
        }
    }
    if (g_module_base) {                     /* fallback: the known data RVA (OG *(base+0x57216f0)) */
        void *rw = NULL;
        if (sh_safe_read(g_module_base + RW_SLOT_KNOWN_RVA, (uint8_t *)&rw, sizeof rw) && rw) return rw;
    }
    return NULL;
}

/* SEH-guarded renderWorld vtable-slot calls (the rw shape is engine-owned; never trust it). */
static unsigned dr_model_count(void *rw)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)rw;
        if (!vtbl) return 0;
        unsigned (*fn)(void *) = *(unsigned (* const *)(void *))(vtbl + RW_VSLOT_MODEL_COUNT);
        return fn ? fn(rw) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void *dr_get_model(void *rw, unsigned idx)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)rw;
        if (!vtbl) return NULL;
        void *(*fn)(void *, unsigned) = *(void *(* const *)(void *, unsigned))(vtbl + RW_VSLOT_GET_MODEL);
        return fn ? fn(rw, idx) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static const char *dr_model_name(void *model)
{
    __try { return *(const char * const *)((const uint8_t *)model + RW_MODEL_NAME_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
/* SEH-guarded write of byte[editor+0x23624]=0 (showcursor). Returns 1 if the write ran. */
static int dr_showcursor(void)
{
    if (!g_module_base) return 0;
    __try {
        *(volatile unsigned char *)(g_module_base + EDITOR_SINGLETON_RVA + ED_SHOWCURSOR_OFF) = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* SEH-guarded GetDeclsOfType("idMaterial") + a no-op presence probe (the OG also did a name lookup we do
 * not need to mutate). Returns the decl-list ptr, or NULL. */
static void *dr_material_decls(void)
{
    if (!g_get_decls) return NULL;
    __try { return ((get_decls_fn)g_get_decls)("idMaterial"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* clone-local cosmetic toggle states (the OG flips engine-side debug flags DAT_18003e789/e78b/e78c; the
 * clone keeps the user-facing toggle behavior without poking undocumented engine globals -- faithful surface,
 * no engine mutation beyond the editor showcursor byte). */
static int g_dr_fps_update = 0;

static void h_sh_debugrender(idCmdArgs *a)
{
    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Not for users, for chrispy to test renderer stuff\n");
        return;
    }

    /* ---- SAFE READ-ONLY sub-ops ---- */
    if (strcmp(sub, "dumprenderinfo") == 0 || strcmp(sub, "dumpmodelinfo") == 0) {
        void *rw = dr_resolve_renderworld();
        if (rw == NULL) { sh_printf("sh_debugrender: renderWorld not available (no live render).\n"); return; }
        unsigned count = dr_model_count(rw);
        if (count > RW_MODEL_COUNT_CAP) { sh_printf("sh_debugrender: rendermodel count implausible (stale).\n"); return; }
        sh_printf("Total active rendermodels: %u\n", count);
        for (unsigned i = 0; i < count; i++) {
            void *m = dr_get_model(rw, i);
            if (m == NULL) continue;
            const char *nm = dr_model_name(m);
            sh_printf("Rendermodel idx %u: %s\n", i, nm ? nm : "(unnamed)");
        }
        return;
    }
    if (strcmp(sub, "showcursor") == 0) {
        if (dr_showcursor()) sh_printf("sh_debugrender: showcursor toggled.\n");
        else                 sh_printf("sh_debugrender: showcursor unavailable (editor not live).\n");
        return;
    }
    if (strcmp(sub, "togglefpsupdate") == 0) {
        g_dr_fps_update = !g_dr_fps_update;
        sh_printf("sh_debugrender: fps update %s.\n", g_dr_fps_update ? "ON" : "OFF");
        return;
    }
    if (strcmp(sub, "showmaterial") == 0 || strcmp(sub, "drawmaterial") == 0 ||
        strcmp(sub, "drawmatarg") == 0) {
        void *decls = dr_material_decls();
        if (decls == NULL) { sh_printf("sh_debugrender: idMaterial decls unavailable.\n"); return; }
        sh_printf("sh_debugrender: idMaterial decls resolved%s.\n",
                  cmd_argv(a, 2) ? " (lookup OK)" : "");
        return;
    }

    /* ---- REFUSED (genuinely harmful; clear toast, NOT bug-for-bug) ---- */
    if (strcmp(sub, "loadimg_n_break") == 0) {
        sh_printf("sh_debugrender: '%s' not available -- it ends in a debugger INT3 trap (halts the game). "
                  "Refused by the clone.\n", sub);
        return;
    }
    if (strcmp(sub, "dump_megatex") == 0) {
        sh_printf("sh_debugrender: '%s' not available -- it hardcodes a write to C:\\Users\\Chris\\megatex.raw "
                  "(a dev path). Refused by the clone.\n", sub);
        return;
    }

    /* ---- NOT-AVAILABLE (heavy dev-only mutators, faithfully surfaced, out of the safe read-only scope) ---- */
    if (strcmp(sub, "test_rm_commit") == 0 || strcmp(sub, "test_sum_shit") == 0 ||
        strcmp(sub, "testnewgui") == 0) {
        sh_printf("sh_debugrender: '%s' is a chrispy-internal render mutator -- not ported to the clone.\n", sub);
        return;
    }

    sh_printf("sh_debugrender: unknown sub-op '%s'.\n", sub);
}

/* ----------------------------------------------------------------- [22] sh -- the SnapStack dispatcher
 * Port of OG XINPUT1_3 FUN_180007620 (the `sh` console command). GATES on the shared UI-interface object
 * (sh_ui_get_iface): if it doesn't exist yet, report "Ui interface doesnt exist yet!" (the OG exact no-UI
 * behavior -- when the frontend hasn't loaded, `sh` faithfully says this). Otherwise look the subcommand
 * up in the interface's runtime cmd-map (interface+0x58) and, on a hit, ENQUEUE {handler,args} onto the
 * work-queue for main-thread execution (the think-loop's +0x1a0 drain runs it).
 *
 * The GATE + the real map-lookup + the work-queue enqueue (faithful to the OG 0x7620 dispatch).
 * The 20 SnapStack subcommands are registered by the snaphakui registrar (FUN_180003c80 port) via the
 * interface's REGISTER slot once the UI thread inits. On a HIT we parse argv into a string vector (argv[1]
 * = the subcommand, argv[2..] = its args -- the OG passes the SUBCOMMAND's args, i.e. the tail starting at
 * the subcommand name, faithful to the OG cmdArgs forwarding) and enqueue {handler,args} onto the work-
 * queue; the think-loop's +0x1a0 drain runs it on the MAIN (UI) thread (the DRIVE CONVENTION -- the heavy
 * editor/engine work must run on the main thread, never the console thread). A MISS reports the OG message
 * "Command %s has not been registered yet". With no subcommand, mirror the OG usage hint. */
static void h_sh_dispatch(idCmdArgs *a)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface == NULL) {
        sh_printf("Ui interface doesnt exist yet!\n");
        return;
    }

    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Dispatches a snaphak command\n");   /* OG usage when no subcommand given */
        return;
    }

    /* Look the subcommand up in the interface's runtime cmd-map (obj+0x58, the registrar-populated map). */
    sh_cmd_handler handler = NULL;
    void          *ctx     = NULL;
    if (!sh_iface_lookup_cmd(iface, sub, &handler, &ctx) || handler == NULL) {
        sh_printf("Command %s has not been registered yet\n", sub);   /* OG miss path */
        return;
    }

    /* Parse argv into a string vector for the queued handler. The OG forwards the SUBCOMMAND's argv (the
     * tail from the subcommand name onward), so argv[0] = the subcommand, argv[1..] = its args. Build that
     * vector from the console idCmdArgs (skip argv[0]="sh"). The enqueue DEEP-COPIES these strings, so the
     * transient engine-owned argv need not outlive this call. */
    int total = cmd_argc(a);
    int sub_argc = total > 1 ? total - 1 : 0;          /* drop the leading "sh" */
    const char *sub_argv[64];
    if (sub_argc > 64) sub_argc = 64;
    for (int i = 0; i < sub_argc; i++) {
        const char *v = cmd_argv(a, i + 1);            /* a->argv[1..] = the subcommand + its args */
        sub_argv[i] = v ? v : "";
    }

    /* ENQUEUE {handler, ctx, sub_argv} onto the work-queue for MAIN-THREAD exec (the +0x1a0 drain runs it).
     * Faithful to OG 0x7620: the dispatch does NOT run the handler inline on the console thread. */
    if (!sh_iface_enqueue_work(iface, handler, ctx, sub_argc, sub_argv))
        sh_printf("sh %s: could not enqueue (out of memory)\n", sub);
}

/* ----------------------------------------------------------------- [12] sh_superscriptop ----------
 * Real port of OG XINPUT1_3 FUN_180026450 (cmd_0x26650 gates it on argv[1]=="genevents"). Dumps the
 * engine's event-definition table to the clipboard as a block of C #defines, so a "superscript" author
 * has every EV_<name>/<eventnum> + its arg-spec on the clipboard. DIRECT RE of the engine event-manager
 * vtable (this DOOMx64vk build; base 0x140000000) -- all hops ride sh_typeinfo's ONE declMgr accessor, no
 * new signature:
 *   declMgr = sh_typeinfo_get_declmgr();                  // accessor @ base+0x17F7030 (0x1417f7030: lazy-init singleton)
 *   evMgr   = (*(*declMgr + 0x90))(declMgr);              // declMgr vtbl slot +0x90 (0x17f70d0: lea rax,[rcx+0x1a0]) -> evMgr sub-object @ declMgr+0x1A0
 *   count   = (*(*evMgr  + 0x28))(evMgr);                 // evMgr vtbl slot +0x28 (0x17f75b0: mov eax,[count]) -> event count
 *   for i in 0..count-1:
 *     name  = (*(*evMgr + 0x20))(evMgr, i);               // evMgr vtbl slot +0x20 (0x17f7550) -> *getByIndex(i) == rec+0x00 == the event NAME char*
 *     rec   = (*(*evMgr + 0x10))(evMgr, name);            // evMgr vtbl slot +0x10 (0x17f7320: findByName) -> the eventDef record (OG DISCARDED this -> the bug)
 *     fspec = *(rec + 0x10);                              // eventDef rec+0x10 == the ';'-delimited arg-spec char* (slot 7 / FUN_140748650 splits it on ';')
 *     emit  "#define EV_<name> <i>\n#define FSPEC_<name> \"<fspec>\"\n"
 *
 * eventDef record layout (DIRECT, the registrar @0x17f7140 + slot accessors): +0x00 name char*,
 * +0x10 fspec char*, +0x2c fspec strlen, +0x30 numArgs, +0x34 eventnum (== the array index i, since the
 * registrar stores rec at array[eventnum] and eventnum increments per registration). So the OG's use of
 * the loop index i as the %d number is correct (i == eventnum).
 *
 * THE OG sprintf BUG (in the OG decompile @0x26450, L45): FUN_180025130(buf, "#define EV_%s %d\n#define FSPEC_%s \"%s\"\n",
 * name, i) passes 4 conversions but only 2 varargs -> the 2nd FSPEC_%s and the "%s" read register garbage.
 * THE CLONE EMITS THE INTENDED OUTPUT, not bug-for-bug: all 4 fields resolved (name, i, name, fspec). If a
 * record's fspec is unreadable/empty (a NULL rec+0x10 -- e.g. a no-arg event), we emit the EV_ line +
 * FSPEC_<name> "" faithfully (an empty arg-spec), never register garbage.
 *
 * BUILD-PORTABILITY (the reference-entity-layout trap): the declMgr-accessor RVA + the declMgr-vtbl +0x90
 * and evMgr-vtbl +0x28/+0x20/+0x10 SLOTS + the rec+0x10 fspec offset are this-build event-manager layout
 * -- RE-DERIVE PER BUILD (disassemble the declMgr ctor FUN_1417f6c70's two vtables
 * PTR_FUN_14270b958 / PTR_FUN_14270c2c8). Every hop is SEH-guarded + non-null gated; a wrong slot degrades
 * to "event manager unavailable" / a clean per-event skip, never a crash (same discipline as sh_type). */
#define SS_EVMGR_ACCESSOR_VSLOT   0x90    /* declMgr vtbl -> evMgr sub-object accessor (BUILD-SPECIFIC) */
#define SS_EV_COUNT_VSLOT         0x28    /* evMgr   vtbl -> event count                (BUILD-SPECIFIC) */
#define SS_EV_GETNAME_VSLOT       0x20    /* evMgr   vtbl -> name-by-index (char*)       (BUILD-SPECIFIC) */
#define SS_EV_FINDBYNAME_VSLOT    0x10    /* evMgr   vtbl -> record-by-name              (BUILD-SPECIFIC) */
#define SS_REC_FSPEC_OFF          0x10    /* eventDef record -> fspec char* (arg-spec)   (BUILD-SPECIFIC) */
#define SS_EV_COUNT_CAP           65536u  /* stale/garbage-evMgr guard (registrar caps the table at 0x1000) */
#define SS_DUMP_CAP               0x40000 /* accumulation buffer (~256 KiB; ~1k events * ~200 B each) */

typedef void *(*ss_evmgr_acc_fn)(void *declmgr);            /* (*declMgr+0x90)(declMgr) -> evMgr */
typedef unsigned (*ss_count_fn)(void *evmgr);              /* (*evMgr+0x28)(evMgr) -> count */
typedef const char *(*ss_getname_fn)(void *evmgr, unsigned i); /* (*evMgr+0x20)(evMgr,i) -> name char* */
typedef void *(*ss_findbyname_fn)(void *evmgr, const char *nm);/* (*evMgr+0x10)(evMgr,name) -> record */

/* SEH-guarded single vtable-slot call wrappers (the evMgr/declMgr shape is engine-owned; never trust it).
 * Each reads *obj (the vtable), then the fn ptr at vtbl+slot, calls it; NULL/0 on any fault. */
static void *ss_call_evmgr_acc(void *declmgr)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;
        if (!vtbl) return NULL;
        ss_evmgr_acc_fn fn = *(ss_evmgr_acc_fn const *)(vtbl + SS_EVMGR_ACCESSOR_VSLOT);
        return fn ? fn(declmgr) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static unsigned ss_call_count(void *evmgr)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return 0;
        ss_count_fn fn = *(ss_count_fn const *)(vtbl + SS_EV_COUNT_VSLOT);
        return fn ? fn(evmgr) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static const char *ss_call_getname(void *evmgr, unsigned i)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return NULL;
        ss_getname_fn fn = *(ss_getname_fn const *)(vtbl + SS_EV_GETNAME_VSLOT);
        return fn ? fn(evmgr, i) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *ss_call_findbyname(void *evmgr, const char *nm)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return NULL;
        ss_findbyname_fn fn = *(ss_findbyname_fn const *)(vtbl + SS_EV_FINDBYNAME_VSLOT);
        return fn ? fn(evmgr, nm) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
/* SEH-guarded read of the fspec char* at rec+0x10 (the ';'-delimited arg-spec). NULL on any fault. */
static const char *ss_read_fspec(void *rec)
{
    __try { return *(const char * const *)((const uint8_t *)rec + SS_REC_FSPEC_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* The same truncating SEH-safe buffer-append sh_typeinfo's dump uses (never overruns). */
static void ss_dump_append(char *buf, size_t cap, size_t *len, const char *s)
{
    if (s == NULL || *len >= cap - 1) return;
    size_t room = cap - 1 - *len;
    size_t add  = strlen(s);
    if (add > room) add = room;
    memcpy(buf + *len, s, add);
    *len += add;
    buf[*len] = '\0';
}

/* [12] sh_superscriptop -- dump the engine's event definitions to the clipboard as C #defines. Real
 * port of OG FUN_180026450 (sprintf bug fixed: all 4 fields resolved -- name, eventnum, name, fspec). */
static void h_sh_superscriptop(idCmdArgs *a)
{
    (void)a;   /* OG gates on argv[1]=="genevents"; the clone wires this handler directly to the command */

    void *declmgr = sh_typeinfo_get_declmgr();
    if (declmgr == NULL) {
        sh_printf("sh_superscriptop: declMgr unavailable.\n");
        return;
    }
    void *evmgr = ss_call_evmgr_acc(declmgr);
    if (evmgr == NULL) {
        sh_printf("sh_superscriptop: event manager unavailable.\n");
        return;
    }
    unsigned count = ss_call_count(evmgr);
    if (count == 0) {
        sh_printf("sh_superscriptop: no event definitions.\n");
        return;
    }
    if (count > SS_EV_COUNT_CAP) {            /* stale/garbage-evMgr guard */
        sh_printf("sh_superscriptop: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];

    /* The OG opened with an eventdef_ss_t struct-comment header (in the OG decompile @0x26450, L38-40); keep a header
     * that documents the emitted #define pair (intended output, not the OG's struct decl which the OG
     * never actually filled in). */
    ss_dump_append(dump, sizeof dump, &dlen,
        "// snaphak sh_superscriptop -- engine event definitions\n"
        "// EV_<name> = the event number; FSPEC_<name> = its ';'-delimited arg-spec\n");

    unsigned emitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;   /* a hole in the table -> skip (no garbage) */

        const char *fspec = NULL;
        void *rec = ss_call_findbyname(evmgr, name);      /* the record the OG fetched but discarded */
        if (rec != NULL) fspec = ss_read_fspec(rec);      /* rec+0x10 -> the arg-spec char* */
        if (fspec == NULL) fspec = "";                    /* no-arg / unreadable -> empty spec (faithful) */

        /* INTENDED output -- all 4 conversions resolved (name, eventnum==i, name, fspec). The OG passed
         * only (name, i) for 4 specifiers; we pass all four properly. */
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "#define EV_%s %u\n#define FSPEC_%s \"%s\"\n", name, i, name, fspec);
        ss_dump_append(dump, sizeof dump, &dlen, line);
        emitted++;
    }

    if (sh_clipboard_set(dump))
        sh_printf("sh_superscriptop: %u event defs copied to clipboard.\n", emitted);
    else
        sh_printf("sh_superscriptop: %u event defs generated (clipboard copy failed).\n", emitted);
}

/* ----------------------------------------------------------------- [21] cs_dumpeventdefs ----------
 * REAL port of the INTENT of OG XINPUT1_3 FUN_18000a1b0 (cmd thunk FUN_180022460). The OG walked a
 * SnapHak-INTERNAL eventDef std::vector (DAT_18003e4d8..e4e0, stride 0x210) -- a table the clone NEVER
 * builds -- formatting each record (FUN_18000a4e0) into a newline-joined string, then fputs'ing it to a
 * HARDCODED "C:\Users\Chris\eternalevents.txt" (chrispy's machine) via fopen_s(...,"w"). We instead source
 * the SAME eventDef data from the ENGINE (exactly the [12] sh_superscriptop walk: sh_typeinfo_get_declmgr
 * -> evMgr via declMgr vtbl+0x90 -> count/getByIndex/findByName) and write it to a FILE.
 *
 * FILE FORMAT (the OG event-def file format, RE-confirmed from the sibling FUN_180026450 header L38-40 --
 * the OG's own eventdef-table declaration; the [21] internal-cache formatter FUN_18000a4e0 was not
 * decompiled, so we reproduce the OG's documented eventdef_ss_t table, whose 5 members map 1:1 onto the
 * engine record fields we have):
 *   header  "struct eventdef_ss_t {const char* m_evname;int m_rettype;const char* m_fspec;"
 *           "unsigned m_numargs; unsigned m_eventnum;};\n\tstatic const eventdef_ss_t ALLEVENTS[]={\n"
 *   per ev  "\t{\"<name>\", <rettype>, \"<fspec>\", <numargs>, <eventnum>},\n"
 *   footer  "};\n"
 * fopen mode = "w" (faithful to the OG fopen_s mode). The OG joined records with '\n' and fputs'd once;
 * we fputs the whole accumulated buffer once (same single-write shape), SEH-guarded.
 *
 * ENGINE-record field map (DIRECT, the [12] eventDef layout): m_evname  <- rec+0x00 (name, via getByIndex)
 *   m_fspec <- rec+0x10 (';'-delimited arg-spec); m_numargs <- rec+0x30; m_eventnum <- rec+0x34. m_rettype
 *   is NOT sourceable from the four record fields the engine walk exposes -- we emit 0 (the faithful
 *   closest equivalent: a placeholder return-type, exactly as the OG's struct reserved the slot). Note the
 *   record's eventnum (rec+0x34) is the AUTHORITATIVE event number (== the loop index i for a dense table,
 *   but we emit the record's own field so a sparse table stays correct).
 *
 * SANE PATH: "eventdefs.txt" in the DOOM cwd (adapts the hardcoded chrispy path; mirrors how [11]
 * cs_start_render_logging writes "renderlog.txt"). The full walk + the file IO are SEH-guarded: a garbage
 * evMgr/record degrades to a clean per-event skip or an "unavailable" line, never a crash. Reuses the [12]
 * declMgr accessor + vtable-slot wrappers -- NO new signature. */
#define CDE_REC_EVENTNUM_OFF  0x34    /* eventDef record -> eventnum (uint)   (BUILD-SPECIFIC, [12] layout) */
#define CDE_OUT_PATH          "eventdefs.txt"   /* sane path (DOOM cwd); adapts OG's hardcoded chrispy path */

/* SEH-guarded read of the eventnum record field cs_dumpeventdefs emits (the [12] clipboard path never needed
 * it). NOTE m_numargs is NOT read from the record: rec+0x30 reads 0 on this build (the engine derives the arg
 * count from the fspec at use-time), so cs_dumpeventdefs derives m_numargs from the fspec instead. */
static unsigned cde_read_eventnum(void *rec, unsigned fallback)
{
    __try { return *(const unsigned *)((const uint8_t *)rec + CDE_REC_EVENTNUM_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

/* SEH-guarded single fputs of the accumulated buffer (a malformed buffer/fp must never fault the editor). */
static int cde_write_file(FILE *fp, const char *buf)
{
    __try { fputs(buf, fp); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* [21] cs_dumpeventdefs -- dump the engine event definitions to a FILE as the OG eventdef_ss_t table. Real
 * port of the OG INTENT (engine-sourced, sane path), not the OG's SnapHak-internal-cache walk. */
static void h_cs_dumpeventdefs(idCmdArgs *a)
{
    (void)a;

    void *declmgr = sh_typeinfo_get_declmgr();
    if (declmgr == NULL) {
        sh_printf("cs_dumpeventdefs: declMgr unavailable.\n");
        return;
    }
    void *evmgr = ss_call_evmgr_acc(declmgr);
    if (evmgr == NULL) {
        sh_printf("cs_dumpeventdefs: event manager unavailable.\n");
        return;
    }
    unsigned count = ss_call_count(evmgr);
    if (count == 0) {
        sh_printf("cs_dumpeventdefs: no event definitions.\n");
        return;
    }
    if (count > SS_EV_COUNT_CAP) {            /* stale/garbage-evMgr guard (same cap as [12]) */
        sh_printf("cs_dumpeventdefs: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];            /* ~256 KiB; ~1.6k events * ~100 B/row fits */
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];

    /* The OG eventdef-table header (verbatim from FUN_180026450 L38-40 -- the OG's own declaration). */
    ss_dump_append(dump, sizeof dump, &dlen,
        "struct eventdef_ss_t {const char* m_evname;int m_rettype;const char* m_fspec;"
        "unsigned m_numargs; unsigned m_eventnum;};\n"
        "\tstatic const eventdef_ss_t ALLEVENTS[]={\n");

    unsigned emitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;    /* a hole in the table -> skip (no garbage) */

        const char *fspec   = NULL;
        unsigned    numargs = 0;
        unsigned    eventnum = i;                          /* dense-table fallback if rec unreadable */
        void *rec = ss_call_findbyname(evmgr, name);       /* the record the OG cache walk formatted */
        if (rec != NULL) {
            fspec    = ss_read_fspec(rec);                 /* rec+0x10 -> the ';'-delimited arg-spec */
            eventnum = cde_read_eventnum(rec, i);          /* rec+0x34 (authoritative event number) */
        }
        if (fspec == NULL) fspec = "";                     /* no-arg / unreadable -> empty spec (faithful) */
        /* m_numargs: derive from the fspec (count the ';'-delimited arg tokens) -- rec+0x30 reads 0 on this
         * build (the engine derives it from the fspec at use-time), and the fspec is authoritative. */
        for (const char *cp = fspec; *cp; cp++) if (*cp == ';') numargs++;

        /* m_rettype: not sourceable from the engine record fields the walk exposes -> 0 (faithful
         * placeholder, as the OG struct reserved the slot). */
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "\t{\"%s\", 0, \"%s\", %u, %u},\n", name, fspec, numargs, eventnum);
        ss_dump_append(dump, sizeof dump, &dlen, line);
        emitted++;
    }
    ss_dump_append(dump, sizeof dump, &dlen, "};\n");      /* footer (close the ALLEVENTS[] table) */

    FILE *fp = NULL;
    if (fopen_s(&fp, CDE_OUT_PATH, "w") != 0 || fp == NULL) {    /* "w" -- faithful to the OG fopen mode */
        sh_printf("cs_dumpeventdefs: could not open %s for writing.\n", CDE_OUT_PATH);
        return;
    }
    int wrote = cde_write_file(fp, dump);
    fclose(fp);

    if (wrote)
        sh_printf("cs_dumpeventdefs: %u event defs -> %s\n", emitted, CDE_OUT_PATH);
    else
        sh_printf("cs_dumpeventdefs: %u event defs generated (file write failed).\n", emitted);
}

/* The entity/spawn handlers live in entity.c -- they need the gameMgr global + the
 * FindEntity/GetOrigin/ExecuteCommandText vtable slots + SpawnByEntityDef (all cached by
 * sh_entity_install). Extern-declared here so CMD_TABLE can reference them without drift; they share
 * sh_commands' idCmdArgs/cmd_argv/sh_printf via commands.h. */
void h_sh_dumpdef(idCmdArgs *a);
void h_sh_spawninfo(idCmdArgs *a);
void h_sh_spawn(idCmdArgs *a);
void h_sh_dumpmap(idCmdArgs *a);   /* T5 -- real port in entity.c (MapGetter+MapWriter, reuses gameMgr) */
/* The 5 player-cheat commands (OG/DLM parity -- DLM's dinput8 adds them; stock SnapMap lacks them) live in
 * entity.c: each toggles one runtime bit on the local idPlayer (FindEntity("player1")). */
void h_noclip(idCmdArgs *a);
void h_infinitehealth(idCmdArgs *a);
void h_noplayerdeath(idCmdArgs *a);
void h_noplayerkill(idCmdArgs *a);
void h_notarget(idCmdArgs *a);

/* The type-introspection handlers live in typeinfo.c -- they reach the reflection/type-info
 * manager via the hardcoded declMgr accessor RVA 0x17F7030 (+vtable+0x80) + FindTypeInfoByName /
 * FindEnumByName (all cached by sh_typeinfo_install). Extern-declared here so CMD_TABLE can reference them
 * without drift; they share sh_commands' idCmdArgs/cmd_argv/sh_printf via commands.h. */
void h_cs_fieldinfo(idCmdArgs *a);
void h_sh_type(idCmdArgs *a);
void h_sh_validclasses(idCmdArgs *a);

/* snaphak_algo handlers live in algo.c -- h_cs_dontuse [18] toggles the 4 f64 math overrides on/off;
 * h_alginfo (sh_alginfo) reports the reimpl PRESENT. Their engine deps (the module base for resolving the
 * 4 AlgoMatMul/Inverse/PackRGBA/CurveEval sigs at FIRE) are cached by sh_algo_install (dllmain). Extern-
 * declared here so CMD_TABLE references them without drift; they share sh_commands' idCmdArgs/sh_printf. */
void h_cs_dontuse(idCmdArgs *a);
void h_alginfo(idCmdArgs *a);

/* sh_target_any: the editor-decl visibility toggle (target_any.c -> h_target_any), a pair-for-pair port of
 * OG SnapHak's own sh_target_any (FUN_180021EE0) -- it flips the visibility pair (bits 7-6 of decl+0x3CD)
 * over every idDeclSnapEditorEntity decl to reveal / re-hide the normally-hidden placeable entity decls.
 * GetDeclsOfType is handed to it by sh_target_any_install (dllmain). Extern-declared here (matching
 * target_any.h) so CMD_TABLE references it without drift; it shares sh_commands' idCmdArgs/sh_printf. */
void h_target_any(idCmdArgs *a);

/* ------------------------------------------------------------------------ the command table -------
 * VERBATIM from the OG XINPUT1_3.dll string table (read 2026-06-21). sh_target_any carries lightly
 * reworded help but the OG behavior (the editor-decl visibility toggle, target_any.c).
 * Order mirrors the [1]-[22] command numbering. sh_help (at the end) is OUR OWN addition. */
typedef struct cmd_entry {
    const char *name;
    void       *handler;
    const char *help;
} cmd_entry;

static void h_sh_help(idCmdArgs *a);   /* defined after CMD_TABLE (it walks the table) */

static const cmd_entry CMD_TABLE[] = {
    { "snapHak_rawmaps_on",  (void *)h_rawmaps_on,  "Switches from the normal doom snapmap format to raw JSON maps for saving and loading." },
    { "snapHak_rawmaps_off", (void *)h_rawmaps_off, "Switches from the raw JSON map format to the normal doom format for snapmaps." },
    { "sh_type",             (void *)h_sh_type,     "Dumps a types (enum/class) fields to the console and copies the text to your clipboard." },
    { "sh_validclasses",     (void *)h_sh_validclasses,"sh_validclasses <inherit> -- lists the engine-valid classNames for an inherit (the classes deriving from its base type Y; the class-dropdown enumerator)." },
    { "sh_entlist",          (void *)h_sh_entlist,  "Dumps the list of idEntity types in the engine" },
    { "snaphak_disable_devmode",  (void *)h_disable_devmode,  "disable devmode" },
    { "snaphak_reenable_devmode", (void *)h_reenable_devmode, "re-enable devmode" },
    { "sh_dumpmap",          (void *)h_sh_dumpmap,  "sh_dumpmap <file path> dumps the current mapfile, even the generated snapmap mapfile to the given path" },
    { "sh_spawn",            (void *)h_sh_spawn,    "sh_spawn <entitydef> <entity name after spawning>" },
    { "sh_dumpdef",          (void *)h_sh_dumpdef,  "sh_dumpdef <entity name>, dumps the entitydef of an existing ingame entity" },
    { "cs_fieldinfo",        (void *)h_cs_fieldinfo,"for chrispy only, you dont need this" },
    { "sh_genbmodel",        (void *)h_sh_genbmodel,"sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file. " },
    { "sh_genmd6model",      (void *)h_sh_genmd6model,"sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model" },
    { "sh_target_any",       (void *)h_target_any,  "Toggles targetting for entities. Reveals / re-hides the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette." },
    { "sh_listres",          (void *)h_sh_listres,  "<resource classname (ex:idMaterial)> <optional: filter> list all resources of a given type" },
    { "sh_alginfo",          (void *)h_alginfo,     "Prints CPU dispatcher info for snaphak_algo." },
    { "sh_debugrender",      (void *)h_sh_debugrender,"Not for users, for chrispy to test renderer stuff" },
    { "cs_dontuse",          (void *)h_cs_dontuse,  "Overrides some calculations in the engine to be more precise, just for shiggles. probably degrades performance and breaks stuff." },
    { "sh_superscriptop",    (void *)h_sh_superscriptop,"For chrispy, dump stuff for superscript" },
    { "cs_dumpeventdefs",    (void *)h_cs_dumpeventdefs,"For chrispy only otherwise you crash, dumps all eventdefs to a file for the wiki" },
    { "cs_start_render_logging", (void *)h_cs_start_render_logging, "Sets up the renderlog hook " },
    { "sh_spawninfo",        (void *)h_sh_spawninfo,"Generate spawnOrientation/spawnPosition from current position in map" },
    { "sh",                  (void *)h_sh_dispatch, "Dispatches a snaphak command" },
    /* The 5 player-cheat commands (OG/DLM parity): DLM's dinput8 adds these to SnapMap; we reproduce them
     * clean-room (toggle one runtime bit on the local idPlayer -- entity.c). Match OG's names exactly. */
    { "noClip",              (void *)h_noclip,         "Toggle noclip (no-collision flight) for the local player." },
    { "infiniteHealth",      (void *)h_infinitehealth, "Toggle infinite health for the local player." },
    { "noPlayerDeath",       (void *)h_noplayerdeath,  "Toggle no-death (the player cannot die) for the local player." },
    { "noPlayerKill",        (void *)h_noplayerkill,   "Toggle no-kill (the player cannot be killed) for the local player." },
    { "noTarget",            (void *)h_notarget,       "Toggle notarget (enemies ignore the local player)." },
    /* OUR OWN addition (no OG counterpart): one place that lists the whole SnapHak console surface. */
    { "sh_help",             (void *)h_sh_help,        "Lists every SnapHak console command and cvar with its description." },
};
#define CMD_COUNT ((int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))

/* sh_help -- print the full SnapHak console surface: every CMD_TABLE command (name + help) and every
 * cvar table row (name + default + description). The help strings are the same ones registered with
 * the engine; this just puts them in ONE listing (the engine's own listCmds buries them among
 * thousands of engine commands). */
static void h_sh_help(idCmdArgs *a)
{
    (void)a;
    sh_printf("SnapHak commands (%d):\n", CMD_COUNT);
    for (int i = 0; i < CMD_COUNT; i++)
        sh_printf("  %-28s %s\n", CMD_TABLE[i].name, CMD_TABLE[i].help);
    int ncv = sh_cvar_table_count();
    sh_printf("SnapHak cvars (%d):\n", ncv);
    for (int i = 0; i < ncv; i++) {
        const char *nm = NULL, *df = NULL, *ds = NULL;
        if (sh_cvar_table_row(i, &nm, &df, &ds))
            sh_printf("  %-28s (default %s) %s\n", nm, df, ds);
    }
}

/* ====================================================================== command unlock ===========
 * Make EVERY console command usable once a developer command (e.g. `god`) flips developer mode on.
 *
 * THE PROBLEM. DOOM splits commands across a two-table developer gate exactly like cvars: a fresh
 * console scans the FULL list (cmdSys+0x08), but the instant dev mode turns on the console scans the
 * DEV list (cmdSys+0x20) AND applies a cheat guard (`ExecuteCommandText` 0x1aa4950: throws unless
 * cmd->flags@+0x20 & 2). The engine's native cheats (noclip/give/...) and the clone's own commands are
 * registered without the dev flag, so they read "Unknown command" right after `god` -- the regression
 * vs the original SnapHak (whose bundled mod flagged every command).
 *
 * THE FAITHFUL FIX (what the original mod's dinput8 does). It detours the engine AddCommand
 * (0x1aa3630) and ORs flags|6 (=0x2 cheat-exempt | 0x4 dev-table-membership) into EVERY registration,
 * so the engine's OWN AddCommand inserts each command into BOTH tables, growing each list's own buffer
 * correctly. We mirror that: (1) detour AddCommand the same way for all FUTURE registrations (incl. the
 * gameplay commands that only register on level load); (2) a one-time pass for commands ALREADY
 * registered before our detour installed -- OR flags|6 + insert into the DEV list via the engine's OWN
 * idList grow. NO table aliasing: an earlier attempt pointed the DEV idList at the FULL backing array
 * with a stale DEV.count, so AddCommand's DEV-append wrote into the shared buffer at the wrong index and
 * duplicated/lost commands. The engine never shares those buffers; neither do we.
 *
 * Offsets DIRECT from the AddCommand (0x1aa3630) + ExecuteCommandText (0x1aa4950) decompiles:
 *   cmdSys: FULL idList {array@+0x08, count@+0x10}, DEV idList {array@+0x20, count@+0x28, cap@+0x2c}.
 *   idCommand (operator_new(0x28)): name@0, handler@8, argComp@0x10, help@0x18, flags@+0x20. */
#define CMD_FULL_ARRAY_OFF  0x08u
#define CMD_FULL_COUNT_OFF  0x10u
#define CMD_DEV_ARRAY_OFF   0x20u
#define CMD_DEV_COUNT_OFF   0x28u
#define CMD_DEV_CAP_OFF     0x2cu
#define CMD_OBJ_FLAGS_OFF   0x20u
#define CMD_DEV_FLAGS       0x6u        /* 0x2 cheat-exempt | 0x4 dev-table membership */
#define CMD_COUNT_SANITY    100000u

/* idList grow (engine FUN_140699a60): ensures room for one more element on the idList at `list`
 * (granularity-or-double then idList::Resize, the engine allocator). BUILD-LOCKED RVA + recipe:
 * re-derive by decompiling AddCommand (0x1aa3630) -- it calls THIS on cmdSys+0x08 (FULL) and
 * cmdSys+0x20 (DEV) before each append. A wrong RVA degrades to a skipped insert (SEH), never a crash. */
#define IDLIST_GROW_RVA     0x699a60u
typedef void (*idlist_grow_fn)(void *idlist);

/* The AddCommand detour: OR flags|6 then call through the trampoline (= the original mod's
 * `or [rsp+0x30],6`). 6-arg passthrough; flags is the 6th (stack) arg. */
typedef void (*add_command6_fn)(void *cmdsys, const char *name, void *handler, const char *help,
                                void *argComp, unsigned int flags);
static add_command6_fn g_addcmd_tramp = NULL;
#define ADDCMD_STOLEN 15   /* 3 whole `mov [rsp+N],reg` prologue movs (5B each) -- >=14, no RIP/rel */

static void hook_add_command(void *cmdsys, const char *name, void *handler, const char *help,
                             void *argComp, unsigned int flags)
{
    if (g_addcmd_tramp)
        g_addcmd_tramp(cmdsys, name, handler, help, argComp, flags | CMD_DEV_FLAGS);
}

/* SEH-guarded: is `cmd` already in the DEV idList? (A torn read -> treat as present, i.e. skip.) */
static int cmd_in_dev(uint8_t *cmdSys, void *cmd)
{
    __try {
        void   **dev = *(void ***)(cmdSys + CMD_DEV_ARRAY_OFF);
        uint32_t n   = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
        if (dev == NULL) return 0;
        if (n > CMD_COUNT_SANITY) return 1;
        for (uint32_t i = 0; i < n; i++)
            if (dev[i] == cmd) return 1;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

/* SEH-guarded: append `cmd` to the DEV idList, growing via the engine's OWN idList grow if full.
 * Mirrors AddCommand's DEV-append exactly (engine-managed buffer; never shares the FULL array). */
static void cmd_dev_append(uint8_t *cmdSys, void *cmd, idlist_grow_fn grow)
{
    __try {
        uint32_t count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
        uint32_t cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        if (count >= cap) {
            if (!grow) return;                       /* can't grow safely -> skip (never corrupt) */
            grow(cmdSys + CMD_DEV_ARRAY_OFF);        /* engine realloc; array + cap move */
            count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
            cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        }
        if (count < cap) {
            void **dev = *(void ***)(cmdSys + CMD_DEV_ARRAY_OFF);   /* re-read after grow */
            if (dev != NULL) {
                dev[count] = cmd;
                *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF) = count + 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip on fault */ }
}

/* One-time pass: OR flags|6 on every FULL command + insert any not yet in DEV. Catches every command
 * registered BEFORE our detour installed (the engine's core commands). Returns the count walked. */
static uint32_t command_unlock_pass(uint8_t *cmdSys, idlist_grow_fn grow)
{
    void   **full = NULL;
    uint32_t n = 0;
    __try {
        full = *(void ***)(cmdSys + CMD_FULL_ARRAY_OFF);
        n    = *(uint32_t *)(cmdSys + CMD_FULL_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (full == NULL || n == 0 || n > CMD_COUNT_SANITY) return 0;

    for (uint32_t i = 0; i < n; i++) {
        void *cmd = NULL;
        __try { cmd = full[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (cmd == NULL) continue;
        __try { *(uint32_t *)((uint8_t *)cmd + CMD_OBJ_FLAGS_OFF) |= CMD_DEV_FLAGS; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!cmd_in_dev(cmdSys, cmd))
            cmd_dev_append(cmdSys, cmd, grow);
    }
    return n;
}

/* Install the command unlock: detour AddCommand (all FUTURE registrations) + a one-time pass over the
 * commands already registered. Idempotent-latched by the caller (one-shot). cmdsys/add_command already
 * resolved; module_base anchors the engine idList-grow. */
static void sh_command_unlock_install(void *cmdsys, void *add_command, const uint8_t *module_base)
{
    if (cmdsys == NULL || add_command == NULL) {
        backend_log("B2: command-unlock SKIPPED -- cmdsys/AddCommand unresolved");
        return;
    }
    idlist_grow_fn grow = module_base ? (idlist_grow_fn)(module_base + IDLIST_GROW_RVA) : NULL;

    /* (1) detour AddCommand: every FUTURE registration (incl. gameplay commands on level load) gets
     *     flags|6, so the engine's own AddCommand inserts it into BOTH tables, growing properly. */
    void *tramp = install_inline_hook(add_command, (void *)hook_add_command, ADDCMD_STOLEN);
    if (tramp != NULL) {
        g_addcmd_tramp = (add_command6_fn)tramp;
        backend_log("B2: command-unlock -- AddCommand detour installed (flags|6 on every registration)");
    } else {
        backend_log("B2: command-unlock -- AddCommand detour FAILED (one-time pass still runs)");
    }

    /* (2) one-time pass over already-registered commands (the engine's core set): OR flags|6 + insert
     *     into the DEV list via the engine's own idList grow. */
    uint32_t walked = command_unlock_pass((uint8_t *)cmdsys, grow);
    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: command-unlock APPLIED -- %u commands now in DEV table + cheat-exempt (god/noclip/give stay usable after dev mode toggles)",
        walked);
    backend_log(line);
}

/* Register one command via the 6-arg engine AddCommand. flags=2 (developer-EXEMPT): AddCommand massages
 * 2 -> stored 6 (bits 0x2|0x4), so the command is appended into BOTH the cmdSystem FULL table (+0x08) AND
 * the DEV table (+0x20), and it passes ExecuteCommandText's "Attempting to call a developer command" cheat
 * guard (which throws iff dev-mode-on AND (flag&2)==0). Result: SnapHak's commands are typeable in the `~`
 * console whether or not dev mode is active (a developer tool flips dev-mode on -> the typed console then
 * scans the DEV table; with flags=0 our commands are FULL-only and read "Unknown command" there). The
 * engine's own always-typeable commands (`where`/`getviewpos`) use exactly flags=2.
 *   RE: command-console-exposure -- ExecuteCommandText 0x141aa4950 (gate getter *(cmdSys+0x200a8):
 *   0=>FULL@+0x08, !=0=>DEV@+0x20), AddCommand 0x141aa3630 (`flags|4 if flags&2`; cheat guard 0x1419fcb60).
 *   DELIBERATE divergence from OG-faithful: the OG passes NO flag (~stack garbage, effectively 0), so the
 *   OG's own commands are ALSO dev-gated -- flags=2 EXCEEDS OG (a console-usability fix). */
static int register_cmd(const cmd_entry *e)
{
    __try {
        g_add_command(g_cmdsys, e->name, e->handler, e->help, NULL, 2u);   /* flags=2 -> FULL+DEV + cheat-exempt */
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    if (!add_command) { backend_log("B2: commands SKIPPED -- AddCommand unresolved"); return 0; }
    if (!printf_disp) { backend_log("B2: commands SKIPPED -- Printf unresolved"); return 0; }
    if (!cmdsys)      { backend_log("B2: commands SKIPPED -- cmdSystem unresolved"); return 0; }

    g_add_command = (add_command_fn)add_command;
    g_cmdsys      = cmdsys;
    g_printf      = (printf_dispatch_fn)printf_disp;
    g_get_decls   = get_decls;
    g_module_base = module_base;   /* devmode [15][16] resolve SessionDevModeGetter at FIRE off this base */

    int n = 0;
    for (int i = 0; i < CMD_COUNT; i++)
        if (register_cmd(&CMD_TABLE[i])) n++;

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: registered %d/%d console commands (cmdsys=%p add=%p printf=%p)",
        n, CMD_COUNT, cmdsys, add_command, printf_disp);
    backend_log(line);

    /* Command unlock: detour AddCommand (flags|6 on every future registration) + a one-time pass over
     * the already-registered set, so every command stays usable once a dev command flips dev mode.
     * Runs AFTER our own registrations are in the FULL table (the pass mirrors them into DEV too). */
    sh_command_unlock_install(g_cmdsys, (void *)g_add_command, g_module_base);
    return n;
}
