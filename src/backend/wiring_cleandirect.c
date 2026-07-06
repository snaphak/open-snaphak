/* wiring_cleandirect.c -- see wiring_cleandirect.h. sh_target_any "wire-any": connect a wire to ANY target
 * ENTITY directly, with no "which input?" radial picker.
 *
 * Clean-room from our own reverse-engineering (DIRECT). Zero OG SnapHak bytes. This EXCEEDS the original
 * SnapHak: when a target exposes input actions (e.g. via the user's palette override), the stock editor
 * wire tool node-mediates the connection and raises the state-0xd "snapLogicPicker" modal (pick which of
 * the target's inputs) -- which interrupts the wire, especially when sh_target_any is toggled ON mid-drag
 * from an output node. The original cannot suppress that picker (it is override-driven, not toggle-driven).
 *
 * MECHANISM. The stock connect creators (cdbb40 base-entity source / cdb990 output-node source) already
 * have a CLEAN-DIRECT branch that binds the wire straight to the target ENTITY with no picker + no node
 * mediation -- but they only take it when the target decl is flagged an input-node (decl+0x3cd & 0x10) or
 * a filter (decl+0x3ce & 1). So while sh_target_any is revealed, we transiently set the hovered target's
 * decl+0x3cd input-node bit for the DURATION of the stock creator's call, let the STOCK creator run its own
 * clean-direct code, then restore the bit in a __finally. We force NO slots, create NO node, free NO node --
 * the stock branch does all the work, so none of the placeholder / stray-wire / "(no module)" artifacts a
 * forced-edge hook produces. The restore runs even if the stock creator exits non-locally (the __finally
 * fires during the unwind), so the transient bit can never leak onto the decl -- critical because the decl is
 * shared + process-lifetime and re-hide's &0x3F mask cannot clear a stray input-node bit once leaked.
 *
 * A target that is ALREADY an output-node (decl+0x3cd & 0x20) is left untouched -- that path does not raise
 * the input picker. Off unless sh_target_any is in its reveal state (the OFF passthrough is transparent).
 *
 * PORTABILITY: the two creators resolve by SIGNATURE. The tool/decl field offsets are build-specific --
 * RE-DERIVE per DOOM build by decompiling cdbb40/cdb990 (the constants they dereference are these).
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "wiring_cleandirect.h"
#include "target_any.h"     /* sh_target_any_is_shown() -- the shared reveal toggle that gates this */
#include "signatures.h"
#include "patch.h"          /* sh_install_detour_sig / sh_uninstall_detour */
#include "backend_log.h"
#include "apply_engine.h"   /* ae_schedule_target_write -- the Fix B targets-write for a bare timeline target */

#define SIG_CREATOR_BASE     "ConnectOutputCreator"   /* 0xcdbb40 -- base-entity-source connect creator */
#define SIG_CREATOR_OUTNODE  "WireConnectCreator1"    /* 0xcdb990 -- output-node-source connect creator */
#define WIRING_STOLEN        15u

/* ---- build-specific offsets (RE-DERIVE per DOOM build: decompile cdbb40/cdb990) ------------------- */
#define WORLD_ENTTABLE_OFF   0x204c8   /* world(param_2) -> loaded-map/entity-table object */
#define MAP_ENTARRAY_OFF     0x6a0     /* that object -> the entity-ptr array (8-byte entries) */
#define ENT_DECL_OFF         0x08      /* entity + 8 -> the resolved decl (idDeclSnapEditorEntity) */
#define DECL_FLAGS_OFF       0x3cd     /* decl -> the editor-flags byte */
#define DECL_ISINPUT_BIT     0x10      /* decl+0x3cd bit 0x10 = is-input-node (the clean-direct gate) */
#define DECL_ISOUTPUT_BIT    0x20      /* decl+0x3cd bit 0x20 = is-output-node (leave those untouched) */
#define WCD_TOOL_SOURCE_OFF  0x08      /* connect-tool + 8 -> the wire SOURCE entity id (the pick anchor). BUILD-
                                        * SPECIFIC -- RE-DERIVE per build (decompile cdbb40/cd9830's tool reads). */

typedef void (*creator_fn)(void *tool, void *world, int idx);

static const uint8_t *g_module_base    = NULL;
static volatile LONG  g_installed      = 0;
static creator_fn     g_orig_base      = NULL;   /* trampoline to stock cdbb40 */
static creator_fn     g_orig_outnode   = NULL;   /* trampoline to stock cdb990 */

/* Monotonic "a wire-any connect edit happened" counter. Bumped whenever the wire-any hook processes a real
 * target pick (below). The Studio entity list rebuilds its module-name labels ONLY on an entity-COUNT change
 * (a wire connect nets none), so after a wire the labels go stale until a manual refresh. The UI think-loop
 * polls this (via the iface +0x288 slot) alongside entity_count and forces a list rebuild when it changes --
 * so the labels auto-settle. Read-only from the UI; the backend only ever increments it. */
static volatile LONG  g_connect_generation = 0;

/* Fix B debounce: the last {source,target} written as a targets-trigger. The connect creators fire per
 * hover-FRAME, so a bare-target pick is written ONCE per distinct pair (reset when the hover leaves a bare
 * target). Backend-side the write is ALSO idempotent (ae_apply_target_write skips a ref already present), so a
 * re-hover cannot duplicate. Single-threaded (editor tick). */
static int            g_last_write_source = -1;
static int            g_last_write_target = -1;

int sh_wiring_cleandirect_generation(void)
{
    return (int)g_connect_generation;
}

/* Resolve the hovered target's decl-flags byte pointer from (world, idx). Returns the byte* or NULL. */
static uint8_t *wcd_target_flags(void *world, int idx)
{
    if (!world || idx < 0) return NULL;
    __try {
        void *mapobj = *(void *const *)((const uint8_t *)world + WORLD_ENTTABLE_OFF);
        if (!mapobj) return NULL;
        void *array = *(void *const *)((const uint8_t *)mapobj + MAP_ENTARRAY_OFF);
        if (!array) return NULL;
        void *ent = ((void *const *)array)[idx];
        if (!ent) return NULL;
        void *decl = *(void *const *)((const uint8_t *)ent + ENT_DECL_OFF);
        if (!decl) return NULL;
        return (uint8_t *)decl + DECL_FLAGS_OFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* The shared detour body: while sh_target_any is revealed, transiently flag a non-output-node target as an
 * input-node so the STOCK creator takes its clean-direct (bind-to-entity, no-picker) branch; restore after. */
static void wcd_run(creator_fn stock, void *tool, void *world, int idx)
{
    if (!sh_target_any_is_shown() || stock == NULL) {
        if (stock) stock(tool, world, idx);          /* OFF: transparent passthrough */
        return;
    }

    /* Classify the hovered target from its decl-flags byte (decl+0x3cd): an OUTPUT node (0x20) or an INPUT node
     * (0x10) is CSR-wireable, so the STOCK creator handles it (a normal wire). A BARE is-target (neither) is the
     * timeline case -- the CSR resolver cannot route a wire to it, so the stock clean-direct would lay a DANGLING
     * CSR edge (the observed re-entry hard crash + per-frame draw fault). For a bare target we SUPPRESS the
     * creator and instead write the source's state.edit.targets (Fix B -- the engine's native `activate` trigger,
     * the form every working ground-truth map uses). */
    uint8_t *flagp = (idx >= 0) ? wcd_target_flags(world, idx) : NULL;
    int bare = 0;
    if (flagp) {
        __try {
            uint8_t f = *flagp;
            if ((f & DECL_ISOUTPUT_BIT) == 0 && (f & DECL_ISINPUT_BIT) == 0) bare = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { bare = 0; }
    }

    if (bare && idx >= 0) {
        /* Fix B: bare target -> SUPPRESS (do NOT run the stock creator; it would lay the dangling CSR edge) and
         * write the source's targets ONCE per distinct pick (the creator fires per hover-frame -> debounce, and
         * the write is idempotent backend-side). SOURCE = the connect-tool anchor (tool+0x08). */
        int source = -1;
        __try { source = *(const int *)((const uint8_t *)tool + WCD_TOOL_SOURCE_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { source = -1; }
        if (source >= 0 && (source != g_last_write_source || idx != g_last_write_target)) {
            g_last_write_source = source;
            g_last_write_target = idx;
            ae_schedule_target_write(source, idx);        /* main-thread: serialize source -> splice targets -> apply */
            InterlockedIncrement(&g_connect_generation);  /* nudge the Studio list to re-scan labels */
        }
        return;   /* stock NOT run -> no node, no CSR edge, no dangling wire */
    }

    /* A real output/input-node target (or an off-target frame): normal wire via the stock creator. Reset the
     * debounce so the next bare pick writes. */
    g_last_write_source = -1;
    g_last_write_target = -1;
    stock(tool, world, idx);
    if (idx >= 0) InterlockedIncrement(&g_connect_generation);
}

static void connect_creator_base_detour(void *tool, void *world, int idx)
{
    wcd_run(g_orig_base, tool, world, idx);
}

static void connect_creator_outnode_detour(void *tool, void *world, int idx)
{
    wcd_run(g_orig_outnode, tool, world, idx);
}

static int wcd_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

void sh_wiring_cleandirect_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   /* one-shot */
    g_module_base = module_base;

    sig_result rb, ro;
    if (!wcd_resolve_sig(SIG_CREATOR_BASE, &rb) || rb.status != SIG_OK) {
        backend_log("B2: sh_target_any clean-direct NOT armed (base creator sig unresolved)");
        return;
    }
    if (!wcd_resolve_sig(SIG_CREATOR_OUTNODE, &ro) || ro.status != SIG_OK) {
        backend_log("B2: sh_target_any clean-direct NOT armed (output-node creator sig unresolved)");
        return;
    }

    void *tb = sh_install_detour_sig(&rb, (void *)connect_creator_base_detour, WIRING_STOLEN);
    if (tb == NULL) {
        backend_log("B2: sh_target_any clean-direct NOT armed (base creator detour install failed)");
        return;
    }
    void *to = sh_install_detour_sig(&ro, (void *)connect_creator_outnode_detour, WIRING_STOLEN);
    if (to == NULL) {
        sh_uninstall_detour(tb);   /* roll back -- leave the engine byte-clean, no half-state */
        backend_log("B2: sh_target_any clean-direct NOT armed (output-node creator detour install failed)");
        return;
    }
    g_orig_base    = (creator_fn)tb;
    g_orig_outnode = (creator_fn)to;
    backend_log("B2: sh_target_any clean-direct ready (off until reveal; cdbb40 + cdb990 detoured)");
}
