/* fault_shield.c -- the in-process fault shield, MERGED into the backend XINPUT1_3.dll (2026-06-22).
 *
 * Was a separate winmm.dll proxy; DOOM's loader REJECTED that proxy at load (the winmm boot-wedge --
 * DllMain never ran, despite the proxy being valid + loading fine in isolation; the real
 * winmm + our XINPUT1_3/dinput8 proxies all load, only our winmm did not). The shield now installs from
 * the BACKEND's bootstrap_thread (backend/dllmain.c) via shield_install(), AFTER the backend's SteamStub
 * decrypt-poll -- so the shield's engine signatures resolve on DECRYPTED .text (the standalone shield
 * never waited for the decrypt, so its sigs would have missed -> bad frame-hook). Same sanctioned
 * divergence (recover-in-place vs OG's TerminateProcess); the VEH + recovery frame-hook are unchanged
 * (veh.c / recovery.c). The backend's hook.c + signatures.c are reused (one copy, no double-link).
 *
 * (The standalone winmm/xinput proxy variants -- winmm_proxy.c/xinput_proxy.c + the .defs -- are NOT in
 * the backend build; kept in this dir only as the abandoned standalone-load-vector reference.)
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "crash_report.h"
#include "shield_sigs.h"

uint8_t *g_doom_base = NULL;   /* shield's view of the DOOM module (set by shield_install from the backend) */
size_t   g_doom_size = 0;

/* Raw kernel-only persistent log (CreateFile/WriteFile, NO CRT -> safe from any context). In -Diag builds it
 * records the arming timeline to <DOOM>\snaphak\logs\shield_arm.log (WRITE_THROUGH, survives a termination).
 * In a RELEASE build it is a file-wise no-op -- the arm sequence still flows to OutputDebugString (the in-game
 * console + any attached debugger), so release leaves no shield_arm.log in the DOOM dir. */
void shield_raw(const char *msg)
{
#ifdef SNAPHAK_DIAG
    static HANDLE h = INVALID_HANDLE_VALUE;
    static int tried = 0;
    if (h == INVALID_HANDLE_VALUE) {
        if (tried) return;
        tried = 1;
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        char *slash = NULL, *q;
        for (q = path; *q; q++) if (*q == '\\') slash = q;
        if (!slash) return;
        /* <DOOM>\snaphak\logs\shield_arm.log -- CRT-free path build (lstrcpy/lstrcat + CreateDirectory,
         * one level at a time) */
        lstrcpyA(slash + 1, "snaphak");
        CreateDirectoryA(path, NULL);
        lstrcatA(path, "\\logs");
        CreateDirectoryA(path, NULL);
        lstrcatA(path, "\\shield_arm.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD wr;
    WriteFile(h, msg, lstrlenA(msg), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
#else
    (void)msg;
#endif
}

/* Install the catch points: the VEH (raw AVs) + the recovery frame-hook. The engine fns both touch are
 * resolved by SIGNATURE (shield_resolve_engine -> g_eng) -- a sig miss falls back to the recipe RVA + is
 * logged. Runs AFTER the backend's decrypt-poll, so the scan sees real decrypted .text. */
int veh_install(void);        /* veh.c */
int recovery_install(void);   /* recovery.c */
static int shield_install_hooks(void)
{
    shield_resolve_engine(g_doom_base);   /* portable: fill g_eng by signature (RVA fallback if a sig misses) */
    return veh_install() && recovery_install();
}

/* ---- instrumentation-coexistence gate (defense-in-depth; veh.c already early-outs on any non-DOOM first-chance AV,
 * so the shield's VEH never competes with an external tool's injection exceptions regardless of order) --------------
 * An external instrumentation tool may create a manual-reset named event BEFORE launching DOOM and SetEvent it at its
 * verified-attach commit point. The shield waits on it (bounded) so the common testing case arms AFTER the tool attaches:
 *   event EXISTS (tool present) -> wait until SetEvent OR the bounded fallback, then arm.
 *   event ABSENT (end user) -> arm IMMEDIATELY (zero added latency). The shield is NEVER skipped. */
#define SHIELD_INSTR_EVENT_NAME   "Local\\SnaphakInstrAttached"
#define SHIELD_ARM_FALLBACK_MS    10000   /* bounded: a stalled/dead attach event still lets the shield arm */

static void shield_wait_for_instr(void)
{
    HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, SHIELD_INSTR_EVENT_NAME);
    if (ev == NULL) {
        OutputDebugStringA("[shield] no instrumentation-attach event (end-user path) -> arming now\n");
        shield_raw("wait: OpenEvent NULL (no attach event) -> arming NOW");
        return;
    }
    shield_raw("wait: event FOUND -> blocking until attach signal or fallback");
    DWORD w = WaitForSingleObject(ev, SHIELD_ARM_FALLBACK_MS);
    CloseHandle(ev);
    if (w == WAIT_OBJECT_0)        shield_raw("wait: SIGNALED -> arming after injection");
    else if (w == WAIT_TIMEOUT)    shield_raw("wait: TIMEOUT (fallback) -> arming");
    else                           shield_raw("wait: ERROR -> arming");
}

/* The shield's single entry point -- called from the backend's bootstrap_thread AFTER the decrypt-poll,
 * with the backend's already-resolved DOOM module base/size. Blocks briefly on the coexistence wait, then
 * arms. NEVER raises (the install path is SEH-guarded internally). */
void shield_install(uint8_t *doom_base, size_t doom_size)
{
    g_doom_base = doom_base;
    g_doom_size = doom_size;
    if (g_doom_base == NULL) { shield_raw("shield_install: NULL base -> skipped"); return; }
    shield_raw("shield_install: entered (merged into backend XINPUT1_3)");
    shield_wait_for_instr();
    if (shield_install_hooks()) {
        shield_raw("shield_install: ARMED (VEH + frame-hook installed)");
        OutputDebugStringA("[shield] armed (merged into backend XINPUT1_3)\n");
    } else {
        shield_raw("shield_install: FAILED to install hooks");
        OutputDebugStringA("[shield] FAILED to install hooks\n");
    }
    /* Crash-record capture (crash_report.c): the durable evidence trail behind the in-app crash-report
     * dialog. Armed EVEN IF the recovery hooks failed -- a build whose sigs shifted still deserves crash
     * records. Off the loader lock (we are on the bootstrap thread). */
    crash_report_init();
    crash_report_arm_fatal_handlers();
}
