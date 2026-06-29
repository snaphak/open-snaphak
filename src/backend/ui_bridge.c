/* ui_bridge.c -- the BACKEND TOUCH: create the shared UI-interface object, load the
 * frontend snaphakui.dll, and spin its snaphak_ui_init thread with the matched-pair arg block. The `sh`
 * dispatcher gates on the interface this module owns (null -> "Ui interface doesnt exist yet!").
 *
 * FAITHFUL to the OG spine tail (XINPUT1_3 FUN_1800229b1, RE-confirmed this session):
 *   ... build the interface (operator_new(0x60) + vtable + the 0x78 sub-object) -> DAT_18003e608 ...
 *   ... register `sh` (AddCommand cmdsys "sh" FUN_180007620 ...) ...
 *   _DAT_18003e5e0 = &DAT_18003e128;        // the CreateThread arg block
 *   _DAT_18003e5f8 = DAT_18003e608;         // arg[3] = the interface object
 *   SetDllDirectoryA(".\\snaphak\\");
 *   _DAT_18003e600 = LoadLibraryA(".\\snaphak\\snaphakui.dll");
 *   lpStart = GetProcAddress(_DAT_18003e600, "snaphak_ui_init");
 *   CreateThread(0, 0x100000, lpStart, &_DAT_18003e5e0, 0, ...);
 *
 * The interface + its REGISTER/UNREGISTER/DRAIN bodies are the generic, engine-free factory in
 * ../common/snaphak_iface.c (compiled into this backend). It creates the minimal object (empty cmd-map
 * + empty work-queue); the SnapStack op bodies + the work-queue producer (the `sh` enqueue) are filled in later.
 *
 * Clean-room: our own RE; zero OG bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "snaphak_iface.h"
#include "ui_bridge.h"
#include "backend_log.h"

/* The shared interface object the frontend consumes (OG DAT_18003e608). Created once at spine install;
 * the `sh` dispatcher gates on it. */
static sh_iface *g_iface = NULL;

/* The CreateThread arg block handed to snaphak_ui_init (OG &DAT_18003e5e0). Must outlive the thread (the
 * frontend reads it during init), so it is module-static. Layout pinned by sh_ui_argblock:
 *   [0] out_slot (frontend writes the loop-state obj here), [1] argc, [2] argv, [3] iface. */
static sh_ui_argblock g_argblock;
static void          *g_ui_out_slot = NULL;   /* receives the frontend's loop-state obj address */

/* A synthetic argv for QApplication (the OG passes DOOM's own argv; we hand a minimal stable argv so the
 * Qt app constructs cleanly -- the frontend also has its own fallback). Static => stable for QApp's life. */
static char  g_argv0[] = "snaphak";
static char *g_argv[]  = { g_argv0, NULL };

/* the loaded frontend module handle (OG _DAT_18003e600); kept for symmetry / a future FreeLibrary. */
static HMODULE g_snaphakui = NULL;

sh_iface *sh_ui_get_iface(void)
{
    return g_iface;
}

/* Create the interface object (the minimal one) and store it. Idempotent. Returns the object or NULL.
 * Hosted here (the backend owns the interface lifecycle, OG-faithful: the backend builds DAT_18003e608). */
static sh_iface *sh_ui_create_iface(void)
{
    if (g_iface) return g_iface;
    g_iface = sh_iface_create();
    if (g_iface)
        backend_log("C0: interface object created (vtable + empty cmd-map + empty work-queue)");
    else
        backend_log("C0: interface object creation FAILED (operator_new)");
    return g_iface;
}

int sh_ui_bridge_install(void)
{
    /* 1) Build the interface FIRST so `sh` sees it the moment the command is registered (the OG builds it
     *    just before the AddCommand("sh",...) in the same spine). */
    if (!sh_ui_create_iface()) {
        backend_log("C0: ui-bridge abort -- no interface object");
        return 0;
    }

    /* 2) Fill the matched-pair arg block: [0]=out-slot, [1]=argc, [2]=argv, [3]=interface. */
    g_argblock.out_slot = &g_ui_out_slot;
    g_argblock.argc     = 1;
    g_argblock.argv     = g_argv;
    g_argblock.iface    = g_iface;

    /* 3) Load the frontend exactly as OG does (relative to the DOOM cwd's snaphak\ overlay). SetDllDirectory
     *    so snaphakui's Qt5*.dll resolve from .\snaphak\ (OG SetDllDirectoryA); the Qt platform plugin
     *    (qwindows.dll) loads separately from .\platforms\ via Qt's app-exe-dir search. */
    SetDllDirectoryA(".\\snaphak\\");
    g_snaphakui = LoadLibraryA(".\\snaphak\\snaphakui.dll");
    if (!g_snaphakui) {
        DWORD e = GetLastError();
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C0: LoadLibraryA(.\\snaphak\\snaphakui.dll) FAILED err=%lu "
            "(interface still created; sh will report it exists)", e);
        backend_log(line);
        /* The interface exists, so `sh` no longer says "doesnt exist yet"; the window just won't show.
         * Not fatal to the backend -- return 1 (the handshake half landed). */
        return 1;
    }

    /* 4) Resolve snaphak_ui_init (undecorated C export, OG ord 10) + spin the UI thread. */
    LPTHREAD_START_ROUTINE start =
        (LPTHREAD_START_ROUTINE)GetProcAddress(g_snaphakui, "snaphak_ui_init");
    if (!start) {
        backend_log("C0: GetProcAddress(snaphak_ui_init) FAILED -- frontend export missing");
        return 1;   /* interface still created */
    }

    /* OG: CreateThread(0, 0x100000, start, &argblock, 0, ...). 1 MiB stack (the OG reserve), the arg block
     *    by pointer, default creation flags. The thread runs the frontend's 30 Hz pump and never returns. */
    HANDLE h = CreateThread(NULL, 0x100000, start, &g_argblock, 0, NULL);
    if (!h) {
        backend_log("C0: CreateThread(snaphak_ui_init) FAILED");
        return 1;   /* interface still created */
    }
    CloseHandle(h);
    backend_log("C0: snaphakui.dll loaded + snaphak_ui_init thread spun (interface handed over)");
    return 1;
}
