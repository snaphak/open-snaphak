/* wiring_mode.c -- see wiring_mode.h. The sh_target_any interactive "link any entities" wire mode.
 * Clean-room from our own reverse-engineering (DIRECT). Zero OG SnapHak bytes.
 *
 * The editor wire tool runs a small per-pick state machine. Its central pick processor also drives the
 * tool's input/camera/escape handling, so we must NOT intercept it -- doing so leaves the tool active
 * and capturing input while its state never advances, which swallows all input including escape. Instead
 * we leave the input handling fully intact and detour three FSM LEAVES, each gated on g_wire_mode:
 *
 *   OFF -> trampoline to the original (the stock wire tool is untouched; no uninstall needed for OFF).
 *   ON  -> the two leaves redirect the tool's OUTCOME to a direct source->target edge:
 *
 *     Hook 1 -- the output-select leaf (engine 0xcdaa30, ABI void(tool, a, world)).
 *       Stock: after the first (source) pick this leaf raises a modal output-node picker. In wire-mode we
 *       record the source into the tool's first chain slot, select the direct-edge creator, advance the
 *       tool to target-select, clear the picker-open flag, and return WITHOUT raising the modal. The tool
 *       is left in a handled state, so input/escape stay alive (this is the whole point -- never leave the
 *       tool's think-state un-advanced).
 *
 *     Hook 2 -- the connect creator (engine 0xcdbb40, ABI void(tool, world, idx)).
 *       Stock: on the second (target) pick this leaf lays a node-mediated edge only if the target exposes
 *       a compatible node, else it injects/auto-creates a node. In wire-mode we instead force the stock
 *       direct-edge outcome for ANY target: record the target into the tool's second chain slot and set
 *       the direct-edge flags. The leaf's own caller then runs the tool's trailing finalize, which lays a
 *       direct edge between the two consecutive chain slots (source -> target) -- no node mediation, so a
 *       node-less target (e.g. a timeline) connects too.
 *
 *     Hook 3 -- the connect creator for an OUTPUT-NODE source (engine 0xcdb990).
 *       Stock: when the wire source is an output node (e.g. an "On Sound Finished" listener) rather than a
 *       base entity, the pick processor routes the target pick to THIS creator (creator-selector 1) instead
 *       of Hook 2's, and it lays a direct edge only if the target is itself an input node, else it
 *       node-mediates (the "which input" radial). In wire-mode we force the direct-edge outcome here too:
 *       for an output-node source the pick processor already records the source into chain slot 1, so we
 *       record the target into chain slot 2 and set the direct-edge flags, and the trailing finalize lays
 *       slot1 (source) -> slot2 (target). So ANY source -- base entity OR output node -- wires directly to
 *       ANY target, with no input radial and no node mediation.
 *
 * The tool-struct field offsets below are read straight out of the three leaves' own decompile (the values
 * they dereference). RE-DERIVE per DOOM build: decompile the output-select leaf (resolved as
 * WireOutputState) and the connect creator (ConnectOutputCreator); the constants they use are these
 * offsets.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wiring_mode.h"
#include "commands.h"     /* idCmdArgs / sh_printf */
#include "signatures.h"
#include "patch.h"        /* sh_install_detour_sig / sh_uninstall_detour (reversible inline-detour layer) */
#include "backend_log.h"

/* ---- engine-function signature names (in the shipped signature DB) ------------------------------- */
#define SIG_OUTPUT_STATE     "WireOutputState"        /* 0xcdaa30 -- Hook 1: the output-select FSM leaf */
#define SIG_OUTPUT_CREATOR   "ConnectOutputCreator"   /* 0xcdbb40 -- Hook 2: the creator (base-entity src) */
#define SIG_OUTPUT_CREATOR1  "WireConnectCreator1"    /* 0xcdb990 -- Hook 3: the creator (output-node src)  */
#define SIG_TOOL_RESET       "ToolReset"              /* 0xcdb3e0 -- the per-tool reset (OFF cleanup)    */

/* Whole, position-independent prologue bytes each detour installer steals. Both leaves' prologues land on
 * an instruction boundary at 15 bytes (0xcdaa30 = 2 reg-save MOVs + push + sub rsp; 0xcdbb40 = 3 reg-save
 * MOVs), 15 >= the installer's 14-byte minimum, with no RIP-relative or relative-branch operand in range
 * -- so the window copies verbatim to the trampoline. */
#define WIRING_STOLEN        15u

/* ---- build-specific wire-tool field offsets (RE-DERIVE per DOOM build) ---------------------------- */
#define TOOL_FSM_ANCHOR_OFF  0x08   /* the source entity index recorded for the in-progress pick      */
#define TOOL_FLAGS_C_OFF     0x0c   /* create-state flag byte (bit 0x01/0x08 = the direct-edge marks) */
#define TOOL_FLAGS_D_OFF     0x0d   /* flag byte; bit 0x40 = output-picker-open                        */
#define TOOL_SLOT0_OFF       0x10   /* chain slot 0 = source                                           */
#define TOOL_SLOT1_OFF       0x14   /* chain slot 1 = target for a base src; = SOURCE for an output-node src */
#define TOOL_SLOT2_OFF       0x18   /* chain slot 2 = target for an output-node src (slot 1 holds the src)  */
#define TOOL_CREATOR_OFF     0x20   /* creator selector (0 = the direct-edge output creator)           */
#define TOOL_UNDOCLASS_OFF   0x24   /* undo class (1 = the single-edge undo, so undo removes one edge) */
#define TOOL_THINK_OFF       0x28   /* think-state; 2 = target-select. MUST never be 0 while active.   */

/* ---- engine-function typedefs (resolved by signature) ------------------------------------------- */
typedef void (*output_state_fn)(void *tool, void *a, void *world);
typedef void (*output_creator_fn)(void *tool, void *world, int idx);
typedef void (*reset_fn)(void *tool);

/* ---- module state ------------------------------------------------------------------------------- */
static const uint8_t *g_module_base = NULL;
static volatile LONG  g_installed   = 0;     /* one-shot install latch */
static volatile LONG  g_wire_mode   = 0;     /* the toggle (0 = off by default) */

static output_state_fn   g_orig_output_state   = NULL;  /* trampoline to the original output-select leaf */
static output_creator_fn g_orig_output_creator = NULL;  /* trampoline to the original creator (base-entity src) */
static output_creator_fn g_orig_output_creator1 = NULL; /* trampoline to the original creator (output-node src) */
static reset_fn          g_reset               = NULL;  /* the tool-reset helper (OFF cleanup; may be 0) */
static void             *g_last_tool           = NULL;  /* the live tool object (captured in either hook) */

/* ---- helpers ------------------------------------------------------------------------------------ */

/* Resolve a named signature off the cached module base (mirrors the other feature modules' resolvers). */
static int wm_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* SEH-guarded tool reset (clears the pick slots + picker-result fields). */
static void wm_reset_tool(void *tool)
{
    if (!g_reset || !tool) return;
    __try { g_reset(tool); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ---- Hook 1: the output-select FSM leaf --------------------------------------------------------- */
/* MS x64 ABI maps (rcx,rdx,r8) to these three parameters, matching the original's declaration. */
static void wire_output_state_detour(void *tool, void *a, void *world)
{
    /* OFF: transparent passthrough to the stock leaf (OFF needs no uninstall, leaves the original intact). */
    if (!g_wire_mode || g_orig_output_state == NULL) {
        if (g_orig_output_state) g_orig_output_state(tool, a, world);
        return;
    }

    g_last_tool = tool;   /* remember the live tool so OFF can reset a half-done pick */

    /* Record the source, select the direct-edge creator, advance to target-select, drop the picker flag,
     * and return WITHOUT raising the modal output picker. think MUST end != 0 (a 0 think-state with the
     * tool still active swallows input, incl. escape). */
    __try {
        uint8_t *t = (uint8_t *)tool;
        *(int *)(t + TOOL_SLOT0_OFF)   = *(int *)(t + TOOL_FSM_ANCHOR_OFF);  /* source */
        *(int *)(t + TOOL_CREATOR_OFF) = 0;                                  /* -> the direct-edge creator */
        *(int *)(t + TOOL_THINK_OFF)   = 2;                                  /* think -> target-select */
        *(uint8_t *)(t + TOOL_FLAGS_D_OFF) &= (uint8_t)~0x40;                /* clear picker-open */
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ---- Hook 2: the connect creator --------------------------------------------------------------- */
/* MS x64 ABI maps (rcx,rdx,r8d) to (tool, world, idx). */
static void connect_creator_detour(void *tool, void *world, int idx)
{
    (void)world;
    /* OFF: transparent passthrough to the stock creator. */
    if (!g_wire_mode || g_orig_output_creator == NULL) {
        if (g_orig_output_creator) g_orig_output_creator(tool, world, idx);
        return;
    }

    g_last_tool = tool;

    /* idx == -1 is a deselect / internal call -- not a real target pick; do nothing. */
    if (idx != -1) {
        /* Force the stock direct-edge outcome for ANY target: record it into chain slot 1 and set the
         * direct-edge flags. The caller's trailing finalize then lays slot0 (source) -> slot1 (target). */
        __try {
            uint8_t *t = (uint8_t *)tool;
            *(int *)(t + TOOL_SLOT1_OFF)        = idx;            /* target -> slot 1 */
            *(uint8_t *)(t + TOOL_FLAGS_C_OFF) |= (uint8_t)9;     /* the direct-edge create flags */
            *(int *)(t + TOOL_UNDOCLASS_OFF)    = 1;              /* single-edge undo class */
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

/* ---- Hook 3: the connect creator for an OUTPUT-NODE source ------------------------------------- */
/* MS x64 ABI maps (rcx,rdx,r8d) to (tool, world, idx). For an output-node source the pick processor has
 * already recorded the source into chain slot 1 and advanced the tool to target-select (think=2), and it
 * routes the target pick to THIS creator (not Hook 2's). Stock this creator lays a direct edge only when
 * the target is itself an input node; in wire-mode we force the direct edge for ANY target. */
static void connect_creator1_detour(void *tool, void *world, int idx)
{
    (void)world;
    /* OFF: transparent passthrough to the stock creator. */
    if (!g_wire_mode || g_orig_output_creator1 == NULL) {
        if (g_orig_output_creator1) g_orig_output_creator1(tool, world, idx);
        return;
    }

    g_last_tool = tool;

    /* The source is in chain slot 1 (the pick processor put it there for an output-node source). idx == -1
     * is a deselect, and idx == source is the source pick itself (the first dispatch) -- neither lays an
     * edge. Returning leaves the tool exactly as the pick processor set it (slot 1 = source, think = 2, a
     * valid handled state), so the stock spurious node-create on the source pick is suppressed and the
     * tool's input/escape handling stays alive. */
    __try {
        uint8_t *t = (uint8_t *)tool;
        int src = *(int *)(t + TOOL_SLOT1_OFF);
        if (idx != -1 && idx != src) {
            /* Force the stock direct-edge outcome for ANY target: record it into chain slot 2 (the next
             * slot after the source) and set the direct-edge flags. The caller's trailing finalize then
             * lays slot1 (source) -> slot2 (target) -- direct, no node mediation, so a node-less target
             * (e.g. a timeline) connects too. */
            *(int *)(t + TOOL_SLOT2_OFF)        = idx;            /* target -> chain slot 2 */
            *(uint8_t *)(t + TOOL_FLAGS_C_OFF) |= (uint8_t)9;     /* the direct-edge create flags */
            *(int *)(t + TOOL_UNDOCLASS_OFF)    = 2;              /* this creator's single-edge undo class */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ---- install + toggle --------------------------------------------------------------------------- */

void sh_wiring_mode_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   /* one-shot */
    g_module_base = module_base;

    /* Resolve the two detour targets FIRST; install nothing unless BOTH resolve (so we never leave one
     * leaf detoured and the other stock -- no half-state). The tool-reset helper is best-effort (used
     * only for OFF cleanup). */
    sig_result rs, rc, rc1;
    if (!wm_resolve_sig(SIG_OUTPUT_STATE, &rs) || rs.status != SIG_OK) {
        backend_log("B2: sh_target_any wire-any NOT armed (output-select leaf sig unresolved)");
        return;
    }
    if (!wm_resolve_sig(SIG_OUTPUT_CREATOR, &rc) || rc.status != SIG_OK) {
        backend_log("B2: sh_target_any wire-any NOT armed (base connect-creator sig unresolved)");
        return;
    }
    if (!wm_resolve_sig(SIG_OUTPUT_CREATOR1, &rc1) || rc1.status != SIG_OK) {
        backend_log("B2: sh_target_any wire-any NOT armed (output-node connect-creator sig unresolved)");
        return;
    }

    /* The OFF-cleanup reset helper: nice to have, not required to arm. */
    sig_result rr;
    if (wm_resolve_sig(SIG_TOOL_RESET, &rr) && rr.status == SIG_OK)
        g_reset = (reset_fn)rr.addr;

    /* Install all three (off-by-default) detours. If any fails, roll the prior ones back -- no half-state. */
    void *tramp_state = sh_install_detour_sig(&rs, (void *)wire_output_state_detour, WIRING_STOLEN);
    if (tramp_state == NULL) {
        g_reset = NULL;
        backend_log("B2: sh_target_any wire-any NOT armed (output-select detour install failed)");
        return;
    }
    void *tramp_creator = sh_install_detour_sig(&rc, (void *)connect_creator_detour, WIRING_STOLEN);
    if (tramp_creator == NULL) {
        sh_uninstall_detour(tramp_state);   /* undo the first so we leave the engine byte-clean */
        g_reset = NULL;
        backend_log("B2: sh_target_any wire-any NOT armed (base connect-creator detour install failed)");
        return;
    }
    void *tramp_creator1 = sh_install_detour_sig(&rc1, (void *)connect_creator1_detour, WIRING_STOLEN);
    if (tramp_creator1 == NULL) {
        sh_uninstall_detour(tramp_creator);   /* undo the prior two so we leave the engine byte-clean */
        sh_uninstall_detour(tramp_state);
        g_reset = NULL;
        backend_log("B2: sh_target_any wire-any NOT armed (output-node connect-creator detour install failed)");
        return;
    }

    g_orig_output_state    = (output_state_fn)tramp_state;
    g_orig_output_creator  = (output_creator_fn)tramp_creator;
    g_orig_output_creator1 = (output_creator_fn)tramp_creator1;
    backend_log("B2: sh_target_any wire-any ready (off by default; output-select + 2 connect-creators detoured)");
}

void h_wiring_mode(struct idCmdArgs *a)
{
    (void)a;
    if (g_orig_output_state == NULL || g_orig_output_creator == NULL || g_orig_output_creator1 == NULL) {
        sh_printf("sh_target_any: wire-any unavailable -- the editor connect-tool functions did not "
                  "resolve on this build.\n");
        return;
    }
    if (!g_wire_mode) {
        g_wire_mode = 1;
        sh_printf("sh_target_any: WIRE-ANY mode ON -- pick a source (a base entity OR an output node like "
                  "'On Sound Finished') then a target with the wire tool to lay a direct connection to ANY "
                  "entity (even node-less targets like a timeline). Repeat as needed; run sh_target_any "
                  "again to turn off.\n");
        backend_log("B2: sh_target_any wire-any ON");
    } else {
        g_wire_mode = 0;
        wm_reset_tool(g_last_tool);   /* abort any half-done pick; harmless if none is in progress */
        sh_printf("sh_target_any: WIRE-ANY mode OFF -- the normal wire tool is restored.\n");
        backend_log("B2: sh_target_any wire-any OFF");
    }
}
