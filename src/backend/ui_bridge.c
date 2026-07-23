/* ui_bridge.c -- the BACKEND TOUCH: create the shared UI-interface object, load the
 * frontend snapmap-plus-ui.dll, and spin its sh_ui_init thread with the matched-pair arg block. The `sh`
 * dispatcher gates on the interface this module owns (null -> "Ui interface doesnt exist yet!").
 *
 * FAITHFUL to the OG spine tail (XINPUT1_3 FUN_1800229b1, RE-confirmed this session; the OG's own
 * folder/DLL/export names appear verbatim below -- ours differ post-rebrand, the MECHANISM is what
 * is reproduced):
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
 * ../common/snapmap_plus_iface.c (compiled into this backend). It creates the minimal object (empty cmd-map
 * + empty work-queue); the SnapStack op bodies + the work-queue producer (the `sh` enqueue) are filled in later.
 *
 * Clean-room: our own RE; zero OG bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "snapmap_plus_iface.h"
#include "ui_bridge.h"
#include "backend_log.h"
#include "config.h"
#include "snapstack.h"

/* The shared interface object the frontend consumes (OG DAT_18003e608). Created once at spine install;
 * the `sh` dispatcher gates on it. */
static sh_iface *g_iface = NULL;

/* The CreateThread arg block handed to sh_ui_init (OG &DAT_18003e5e0). Must outlive the thread (the
 * frontend reads it during init), so it is module-static. Layout pinned by sh_ui_argblock:
 *   [0] out_slot (frontend writes the loop-state obj here), [1] argc, [2] argv, [3] iface. */
static sh_ui_argblock g_argblock;
static void          *g_ui_out_slot = NULL;   /* receives the frontend's loop-state obj address */

/* A synthetic argv for the pinned arg-block layout (the OG passes DOOM's own argv to its frontend; we
 * hand a minimal stable one). Static => stable for the frontend thread's life. */
static char  g_argv0[] = "snapmap-plus";
static char *g_argv[]  = { g_argv0, NULL };

/* the loaded frontend module handle (OG _DAT_18003e600); kept for symmetry / a future FreeLibrary. */
static HMODULE g_ui_dll = NULL;

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
    /* The frontend may read its theme as soon as its DLL thread begins. Bind the engine-independent
     * config service before any command registration, LoadLibrary, or CreateThread can expose it. */
    sh_config_bind_iface_slots();

    /* 1.5) Register the 20 SnapStack subcommands (snapstack.c) here in the backend, once, before the
     *    frontend loads. This is the SOLE registration -- the frontend never re-registers/overwrites
     *    these, so `sh psel`/`sh acctargets`/etc. always resolve to this module's handlers, against ONE
     *    shared store. Run `sh snapstack_diag` in-game to see, per command, which DLL owns it. */
    sh_register_snapstack_commands_backend(g_iface);
    backend_log("C0: backend SnapStack commands registered (sole owner) -- `sh snapstack_diag` to verify");

    /* 2) Fill the matched-pair arg block: [0]=out-slot, [1]=argc, [2]=argv, [3]=interface. */
    g_argblock.out_slot = &g_ui_out_slot;
    g_argblock.argc     = 1;
    g_argblock.argv     = g_argv;
    g_argblock.iface    = g_iface;

    /* 3) Load the frontend the same WAY the OG does (a LoadLibraryA relative to the DOOM cwd's overlay
     *    subfolder), from OUR OWN snapmap-plus\ folder (the OG used .\snaphak\ -- see the spine quote
     *    above; the folder/DLL names diverge post-rebrand). SetDllDirectoryA mirrors the OG's call and
     *    puts .\snapmap-plus\ on the search path for any DLL dependency the frontend resolves at load. */
    SetDllDirectoryA(".\\snapmap-plus\\");
    g_ui_dll = LoadLibraryA(".\\snapmap-plus\\snapmap-plus-ui.dll");
    if (!g_ui_dll) {
        DWORD e = GetLastError();
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C0: LoadLibraryA(.\\snapmap-plus\\snapmap-plus-ui.dll) FAILED err=%lu "
            "(interface still created; sh will report it exists)", e);
        backend_log(line);
        /* The interface exists, so `sh` no longer says "doesnt exist yet"; the window just won't show.
         * Not fatal to the backend -- return 1 (the handshake half landed). */
        return 1;
    }

    /* 4) Resolve sh_ui_init (undecorated C export; the OG's counterpart was snaphak_ui_init, ord 10)
     *    + spin the UI thread. */
    LPTHREAD_START_ROUTINE start =
        (LPTHREAD_START_ROUTINE)GetProcAddress(g_ui_dll, "sh_ui_init");
    if (!start) {
        backend_log("C0: GetProcAddress(sh_ui_init) FAILED -- frontend export missing");
        return 1;   /* interface still created */
    }

    /* OG: CreateThread(0, 0x100000, start, &argblock, 0, ...). 1 MiB stack (the OG reserve), the arg block
     *    by pointer, default creation flags. The thread runs the frontend's 30 Hz pump and never returns. */
    HANDLE h = CreateThread(NULL, 0x100000, start, &g_argblock, 0, NULL);
    if (!h) {
        backend_log("C0: CreateThread(sh_ui_init) FAILED");
        return 1;   /* interface still created */
    }
    CloseHandle(h);
    backend_log("C0: snapmap-plus-ui.dll loaded + sh_ui_init thread spun (interface handed over)");
    return 1;
}
