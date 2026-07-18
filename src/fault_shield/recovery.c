/* recovery.c -- bad-LOAD recovery: after the VEH survives the fault, steer the editor to the My-Maps
 * browser so it doesn't re-fault on the half-built / UAF render-world.
 *
 * WHY a teardown is mandatory (editor-recovery RE): the engine's
 * own `Frame` catch does NOT navigate -- it pops a modal IN the editor and resumes on a dangling render-
 * world (GetLocalSavedMapEdit freed the old world up front; the fault unwound before the new one was
 * assigned) -> the AddRenderModel(NULL) re-fault loop. The ONLY path that destroys the render-world +
 * resets its model count is the editor-exit cycle.
 *
 * HOW (the live-proven editor->browser exit, `openStartMenu`+`exitEditor`,
 * ported to native + driven from a main-thread frame-hook): SetState(editor,0xb) opens the StartMenu
 * (synchronous), then write EXIT-pending + force the GDM dialog result to Yes(0); the StartMenu Think's
 * resolver calls ExitEditor 0x522680 in-frame -> EDITOR->BROWSER.
 *
 * Frame-hook target = idCommonLocal::Frame 0x17ce360 (collision-free: an external instrumentation tool may
 * hook the editor/menu pumps 0x523140/0x1702ba0, NOT Frame). The detour runs on the main thread,
 * before the engine's frame body, exactly the safe context the proven drives use.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "engine_layout.h"
#include "fault_record.h"
#include "hook.h"
#include "recovery.h"
#include "shield_sigs.h"

extern uint8_t *g_doom_base;

typedef void    (*setstate_t)(void *editor, int state);   /* SetState 0x5298A0 (synchronous) */
typedef int64_t (*frame_t)(void *self);                   /* idCommonLocal::Frame 0x17ce360 */
static frame_t orig_frame = NULL;

/* 5 pushes + `mov eax,0x119c0` => boundary 0x17ce36f, all position-independent (disassembly-verified). */
#define FRAME_STOLEN 15
#define RECOVER_BUDGET_FRAMES 600   /* ~10s @ 60fps backstop */

static volatile LONG g_armed = 0;
static int g_state  = 0;            /* 1 = recover, 2 = wait-exit */
static int g_frames = 0;

static uint8_t *editor(void) { return g_doom_base + RVA_EDITOR_SINGLETON; }
static int ed_state(void)    { return *(volatile int32_t *)(editor() + ED_STATE); }

static int in_editor(void)
{
    uint8_t *ed = editor();
    return *(void **)(ed + ED_MAP_PTR) != NULL
        && *(volatile int32_t *)(ed + ED_DEACT_REASON) == 0
        && *(volatile int32_t *)(ed + ED_STATE) != 0;
}

void recovery_arm(void)
{
    if (InterlockedExchange(&g_armed, 1) == 0) { g_state = 1; g_frames = 0; }
}

/* The proven editor->browser exit, one step per frame, on the main thread. */
static void recovery_tick(void)
{
    if (!g_armed) return;
    if (++g_frames > RECOVER_BUDGET_FRAMES) {
        shield_fault f = { "load", -1, "recovery timed out (no editor exit)", 0, 0 };
        shield_emit(&f);
        InterlockedExchange(&g_armed, 0);
        return;
    }
    uint8_t *ed = editor();
    /* SetState sig-resolved (g_eng.setstate); recipe-tagged RVA fallback if the sig missed. */
    setstate_t SetState = (setstate_t)(g_eng.setstate ? g_eng.setstate
                                                      : (uintptr_t)(g_doom_base + RVA_SETSTATE));

    switch (g_state) {
    case 1: /* open StartMenu (synchronous), then arm the EXIT (pending + GDM result = Yes) */
        if (ed_state() != EDITOR_STATE_STARTMENU)
            SetState(ed, EDITOR_STATE_STARTMENU);
        if (ed_state() == EDITOR_STATE_STARTMENU && *(volatile uint8_t *)(ed + ED_EXITING) == 0) {
            *(volatile int32_t *)(ed + ED_EXIT_PENDING) = 1;
            void *ms = *(void **)(ed + ED_MENU_SCREEN);
            if (ms) {
                void *gdm = *(void **)((uint8_t *)ms + MENUSCREEN_GDM);
                if (gdm) *(volatile int32_t *)((uint8_t *)gdm + GDM_RESULT) = 0;
            }
            g_state = 2;
        }
        break;
    case 2: /* the StartMenu Think calls ExitEditor in-frame; done once we're out of the editor */
        if (!in_editor()) {
            shield_fault f = { "load", -1, "recovered -> exited editor to browser", 0, 0 };
            shield_emit(&f);
            InterlockedExchange(&g_armed, 0);
        }
        break;
    }
}

/* ---- EDITOR-NATIVE in-editor notice (Class-A): a transient toast on the editor's own screen --------
 * The VEH (on a Class-A revert) sets g_notice_armed; this tick (main-thread, per-frame) shows a transient
 * toast on the EDITOR's OWN screen object (*(editor+0x21088) = ED_MENU_SCREEN) -- NOT the menu-shell GDM
 * dialog (which activated the browser + left editor render lag, LIVE 2026-06-19). Byte-identical to the
 * engine's "limits reached" toast (FUN_140531e60): build a title + text idStr, call the toast-show, free
 * both. The toast auto-fades + self-dedups (a "shown" byte at toast+0x1b0). Shell-free: touches no shell
 * slot / AddDialog / browser. Fire ONCE then disarm (re-arming each frame would churn idStr alloc/free).
 * DIRECT: FUN_140cfa0b0 (0xCFA0B0) + the engine call site 0x531E60. */
typedef void (*idstr_ctor_t)(void *buf, const char *s);
typedef void (*idstr_dtor_t)(void *buf);
typedef void (*toast_show_t)(void *screen, void *title, void *text);

static volatile LONG g_notice_armed = 0;   /* 1 = generic toast (Class-A), 2 = harvest engine text (Class-B) */

void notice_request(void)     { InterlockedExchange(&g_notice_armed, 1); }
void notice_request_msg(void) { InterlockedExchange(&g_notice_armed, 2); }

/* ---- MESSAGE HARVEST: read the engine's own last-formatted-error text (Class-B / Error(6)/FatalError) ---
 * A plain SEH-guarded READ of the engine global the dispatcher 0x1A08E80 strncpy's the formatted message
 * into right before it throws -- the same buffer the Frame catch funclet 0x1F5B937 prints as the error.
 * NO new hook (zero added instrumentation-conflict surface). A shifted build could land the recipe-tagged RVA on an
 * unmapped page, so SEH-guard the read (mirrors the suppressor-write guard in veh.c). Returns the captured
 * C-string in `out` (NUL-terminated) and 1 if a non-empty message was harvested, else 0 (caller falls back
 * to the generic NOTICE_TEXT_STR). */
static int harvest_engine_msg(char *out, size_t n)
{
    if (!out || n == 0 || g_doom_base == NULL) return 0;
    out[0] = '\0';
    __try {
        const char *p = (const char *)(g_doom_base + RVA_LAST_ERROR_MSG);
        if (p[0] == '\0') return 0;                 /* engine never stashed a message -> generic */
        lstrcpynA(out, p, (int)n);                  /* bounded copy; always NUL-terminates */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return 0;                                   /* RVA shifted onto an unmapped page -> generic */
    }
    return out[0] != '\0';
}

/* Public wrapper for the crash-record writer: same SEH-guarded read, callable from crash contexts. */
int shield_last_engine_msg(char *out, size_t n)
{
    return harvest_engine_msg(out, n);
}

/* Harvest + log the engine's own last-formatted-error text on a downgraded-FatalError / Error(6) throw,
 * INDEPENDENT of the in-editor toast (notice_tick only fires in-editor, so a LOAD-time FatalError -- e.g. a
 * decl-registry "Remove_Locked: Resource wasn't found by ID" -- would otherwise never record its verbatim text,
 * and the fault would have to be reverse-engineered from a bare fault address). Rate-limited; SEH-safe via
 * harvest_engine_msg. Called from the VEH's C++-throw (Layer 2) path. */
static volatile LONG g_harvest_logged = 0;
void log_engine_error_text(void)
{
    char msgbuf[HARVEST_MSG_MAX];
    if (g_harvest_logged >= 16) return;                 /* rate-limit the record */
    if (harvest_engine_msg(msgbuf, sizeof msgbuf)) {
        InterlockedIncrement(&g_harvest_logged);
        shield_fault hf = { "load", -1, msgbuf, 0, 0 };
        shield_emit(&hf);                               /* the verbatim engine error, on the load-time path too */
    }
}

static void notice_tick(void)
{
    uint8_t      *ed = editor();
    uint8_t      *screen;
    idstr_ctor_t  mk;
    idstr_dtor_t  rm;
    toast_show_t  toast;
    const char   *text;
    LONG          mode;
    char          msgbuf[HARVEST_MSG_MAX];
    /* idStr OBJECTS are stack locals (the engine uses 48-byte locals; long tokens heap-alloc internally,
     * freed by the dtor). Align for safety. */
    __declspec(align(16)) unsigned char title_buf[IDSTR_BUF_SIZE];
    __declspec(align(16)) unsigned char text_buf[IDSTR_BUF_SIZE];

    mode = g_notice_armed;
    if (!mode) return;
    /* The editor screen object exists only in-editor (null elsewhere); also require an in-editor state. */
    if (ed_state() == 0) return;
    screen = *(uint8_t **)(ed + ED_MENU_SCREEN);
    if (!screen) return;

    /* All three sig-resolved (g_eng.*); recipe-tagged RVA fallback per fn if a sig missed. */
    mk    = (idstr_ctor_t)(g_eng.idstr_ctor ? g_eng.idstr_ctor : (uintptr_t)(g_doom_base + RVA_IDSTR_CTOR));
    rm    = (idstr_dtor_t)(g_eng.idstr_dtor ? g_eng.idstr_dtor : (uintptr_t)(g_doom_base + RVA_IDSTR_DTOR));
    toast = (toast_show_t)(g_eng.toast_show ? g_eng.toast_show : (uintptr_t)(g_doom_base + RVA_TOAST_SHOW));

    /* MESSAGE HARVEST (Class-B/Error(6) only -- mode 2): carry the engine's verbatim last-error text if it
     * is present, else the generic notice. Class-A (mode 1) keeps the generic string -- a raw AV has no
     * engine error string, so the buffer would be stale. The captured text is logged for the record. */
    text = NOTICE_TEXT_STR;
    if (mode == 2 && harvest_engine_msg(msgbuf, sizeof msgbuf)) {
        text = msgbuf;
        {
            shield_fault hf = { "load", -1, msgbuf, 0, 0 };
            shield_emit(&hf);   /* record the harvested engine message alongside the toast */
        }
    }

    mk(title_buf, NOTICE_TITLE_STR);
    mk(text_buf,  text);
    toast(screen, title_buf, text_buf);    /* (screen, TITLE, TEXT) -- self-dedups via toast+0x1b0 */
    rm(text_buf);
    rm(title_buf);

    InterlockedExchange(&g_notice_armed, 0);   /* fire once; the toast's own guard prevents dup re-shows */
}

/* ---- LAYER 2: keep the dispatcher's throw-gate OPEN ---------------------------------------------------
 * The level>=6 dispatcher 0x1A08E80 throws the RECOVERABLE idException only `if (DAT_146faf820==0 &&
 * DAT_146faf8b0==0)`, else it ExitProcess(1)'s (error-dispatcher-and-recovery.md, the terminal-gate
 * footgun). 0x6faf820 doubles as the render-cap suppressor. The CLONE never arms either (only an
 * external render-cap instrumentation harness does, and it self-resets), so clearing them each frame is a no-op for
 * the end user AND supersedes that harness -- a render-pool overflow now RECOVERS (drop-to-menu) instead
 * of truncating. SEH-guarded: a shifted suppressor RVA must never fault inside the frame hook. */
static void keep_throw_gate_open(void)
{
    if (g_doom_base == NULL) return;
    __try {
        if (*(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_A) != 0)
            *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_A) = 0;
        if (*(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_B) != 0)
            *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_B) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* shifted suppressor RVA -> skip */ }
}

/* ---- RESIDENT SAVE-DELETION GUARD (B): protect the user's saves from the corrupt-verdict delete --------
 * DeleteBadSaveSlots 0x1737C90 does NOT unlink files (DIRECT decompile: it validates each slot via the save
 * load-test 0x563220 and on any bad slot SHOWS the corrupt-save dialog); the actual delete is the dialog's
 * Delete button ACTION (dispatcher 0xE67BF0, action 0x1f). DISMISS-A (DESC_CLEARFLAG_OFF=1, id-agnostic, runs
 * NO button action) closes the prompt WITHOUT deleting -- the live-proven dismiss, now resident.
 * Runs every frame (the dialog lives in the browser/menu, where idCommonLocal::Frame still ticks). SEH-guarded
 * (the menu shell is null pre-Initialize / off-menu). Access pattern ported from the live instrumentation drive
 * lcScanCorruptDialog: S=*(base+RVA_SHELL_PTR_SLOT); dlg=*(S+0x8); arr=*(dlg+0x900);
 * cnt=*(int*)(dlg+0x908); desc[i]=arr+i*0x1b0; desc+0x00=GDM id, desc+0x08=clear-flag (0=pending). */
static int is_corrupt_save_gdm(int gdm)
{
    return gdm == GDM_LOAD_DAMAGED_FILE || gdm == GDM_CORRUPT_CONTINUE
        || gdm == GDM_SNAPMAP_DETECTED_CORRUPT || gdm == GDM_SNAPMAP_REMOVED_CORRUPT;
}

static void save_guard_tick(void)
{
    if (g_doom_base == NULL) return;
    __try {
        uint8_t *S = *(uint8_t **)(g_doom_base + RVA_SHELL_PTR_SLOT);
        if (S == NULL) return;
        uint8_t *dlg = *(uint8_t **)(S + SHELL_DLGMGR_OFF);
        if (dlg == NULL) return;
        uint8_t *arr = *(uint8_t **)(dlg + DLGQ_ARR_OFF);
        if (arr == NULL) return;
        {
            int cnt = *(volatile int *)(dlg + DLGQ_COUNT_OFF);
            int i, dismissed = 0, pending_left = 0;
            if (cnt < 0) cnt = 0;
            if (cnt > 4) cnt = 4;                /* the dialog queue capacity is 4 */
            for (i = 0; i < cnt; i++) {
                uint8_t *d = arr + (size_t)i * DLG_DESC_STRIDE;
                if (*(volatile uint8_t *)(d + DESC_CLEARFLAG_OFF) != 0) continue;   /* already cleared */
                if (is_corrupt_save_gdm(*(volatile int *)(d + DESC_GDMID_OFF))) {
                    *(volatile uint8_t *)(d + DESC_CLEARFLAG_OFF) = 1;   /* DISMISS-A -- runs NO button action */
                    dismissed++;
                } else {
                    pending_left++;
                }
            }
            if (dismissed) {
                if (pending_left == 0) {         /* sync the visible byte once the queue drained */
                    uint8_t *shellMgr = *(uint8_t **)(S + SHELL_SHELLMGR_OFF);
                    if (shellMgr) *(volatile uint8_t *)(shellMgr + SHELLMGR_VISIBLE_OFF) = 0;
                }
                {
                    shield_fault f = { "save", -1, "save-guard: dismissed a corrupt-save dialog (save protected)", 0, 0 };
                    shield_emit(&f);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* shell not mapped / shifted RVA -> skip this frame */ }
}

static int64_t frame_detour(void *self)
{
    keep_throw_gate_open();      /* LAYER 2: gate clear so engine Error(6)/downgraded-FatalError recovers */
    save_guard_tick();           /* B: resident save-deletion guard (dismiss the corrupt-save dialogs) */
    recovery_tick();             /* main-thread, per-frame, before the engine frame body (Class-A recovery tick) */
    notice_tick();               /* show the editor-native toast if a Class-A revert armed it */
    return orig_frame(self);
}

/* ---- LAYER 2: downgrade idCommon::FatalError(7) -> the RECOVERABLE Error(6) throw, with a ONE-BYTE patch.
 * The FatalError7 wrapper (0x1a089e0) is byte-identical to Error6 except it passes `mov ecx,7` (B9 07) to
 * the dispatcher; rewriting that 07 imm8 to 06 makes EVERY engine FatalError throw idException (Frame catch
 * -> drop-to-menu + resume -> DOOM survives) instead of the terminal idFatalException (always rethrows ->
 * WinMain exit). DIRECT: error-dispatcher-and-recovery.md (level taxonomy + the recovery nest); this is the
 * "downgrade FatalError(7)->Error(6) survives" implication that truth flagged as not-yet-demonstrated.
 * ROBUST: scan the wrapper's first 0x40 bytes for B9 07 00 00 00 + verify before writing, so a shifted
 * build (sig fell back to a stale RVA) refuses the patch rather than corrupting a wrong byte. */
static void patch_fatalerror_downgrade(void)
{
    uint8_t *fe = (uint8_t *)(g_eng.fatalerror7 ? g_eng.fatalerror7
                                                : (uintptr_t)(g_doom_base + RVA_FATALERROR7));
    __try {
        int i, at = -1;
        for (i = 0; i < 0x40; i++) {
            if (fe[i] == 0xB9 && fe[i + 1] == 0x07 &&
                fe[i + 2] == 0 && fe[i + 3] == 0 && fe[i + 4] == 0) { at = i; break; }
        }
        if (at < 0) {
            shield_fault f = { "sig", -1,
                "FatalError downgrade: MOV ECX,7 not found in wrapper (re-derive 0x1a089e0)", 0, 0 };
            shield_emit(&f);
            return;
        }
        {
            DWORD old;
            uint8_t *imm = fe + at + 1;                      /* the level immediate byte (0x07) */
            if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *imm = 0x06;                                 /* 7 -> 6: throws recoverable idException now */
                VirtualProtect(imm, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), imm, 1);
                shield_fault f = { "action", -1,
                    "FatalError(7) downgraded to recoverable Error(6) (level 7->6)", 0, 0 };
                shield_emit(&f);
            } else {
                shield_fault f = { "sig", -1, "FatalError downgrade: VirtualProtect failed", 0, 0 };
                shield_emit(&f);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shield_fault f = { "sig", -1, "FatalError downgrade write faulted (re-derive 0x1a089e0)", 0, 0 };
        shield_emit(&f);
    }
}

int recovery_install(void)
{
    /* Hook the SIG-RESOLVED Frame entry (g_eng.frame); recipe-tagged RVA fallback if the sig missed. The
     * 15-byte FRAME_STOLEN prologue (5 pushes + mov eax,0x119c0) IS the Frame sig's fixed bytes, so a sig
     * hit lands the hook on exactly the stolen-byte boundary the disasm verified. */
    void *target = (void *)(g_eng.frame ? g_eng.frame : (uintptr_t)(g_doom_base + RVA_FRAME));
    orig_frame = (frame_t)install_inline_hook(target, (void *)frame_detour, FRAME_STOLEN);
    if (orig_frame == NULL) return 0;
    patch_fatalerror_downgrade();   /* LAYER 2: FatalError(7) -> recoverable Error(6) (one-byte level patch) */
    return 1;
}
