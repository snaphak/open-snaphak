/* veh.c -- catch a raw access-violation in DOOM frame code and recover, by fault class.
 *
 * The OS raises an access-violation the engine's C++ EH never sees; we catch it FIRST in the VEH chain
 * (AddVectoredExceptionHandler first-in-chain) and recover per class:
 *
 *   Class A -- an IN-EDITOR draw fault (a corrupt/OOR connection-CSR column -> wild-pointer deref in the
 *     per-frame module-view draw: editor Think 0x523140 -> 0x521D90 -> 0x5E7380 -> 0x5E6410 -> resolver
 *     0x5E0AD0 -> visibility leaf 0xD32A30). The resolver writes into a CALLER stack idList the consumer
 *     0x5E5CB0 draws only `if (0<count)` (DIRECT) -- so a partial list is safe. We ABORT the faulting
 *     draw by RtlVirtualUnwind'ing the thread back to the editor frame 0x523140 and continuing: the frame
 *     completes, the editor stays LIVE + responsive. NO modal, NO thread-parking. (Detected by whether
 *     the editor frame is an ancestor on the faulting stack -- the unwind IS the classifier.)
 *
 *   Class B / unknown -- a bad-LOAD CSR-builder fault, or an unclassified wild deref (the editor frame is
 *     NOT on the stack). Fall back to the engine's own recoverable idCommon::Error(6) (-> idException ->
 *     idCommonLocal::Frame catch) + drive the proven editor-exit -> My-Maps browser (recovery.c). This
 *     CAN surface DOOM's recoverable-error modal; it is the documented fallback while the clean Class-B
 *     load-abort (return-failure from GetLocalSavedMapEdit 0x566640) is RE-pending.
 *
 * WHY the split: routing the in-editor fault through Error(6) traps the user on a thread-parking modal
 * (LIVE 2026-06-19) -- the level-6 dispatcher 0x1A08E80 ALWAYS raises a surface (overlay listener-walk
 * FUN_141a44420) AND throws/exits (DIRECT, decompile), so it can never be the in-editor recovery.
 *
 * Provenance: our own clean-room RE of the engine fault dispatcher + recovery (nonmodal-recovery,
 * ratified against the primary decompiles, 2026-06-19).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_layout.h"
#include "fault_record.h"
#include "recovery.h"
#include "shield_sigs.h"
#include "veh.h"
#pragma comment(lib, "user32.lib")   /* MessageBoxA -- the crash popup */

extern uint8_t *g_doom_base;
extern size_t   g_doom_size;

/* The classifier RVA ranges ride the SIG-RESOLVED entry (g_eng.*_rva) when present, else the recipe-
 * tagged pinned RVA. HI is derived as LO + the recipe-tagged span (engine_layout.h) so it tracks a
 * shifted build off the sig. */
static uintptr_t editor_lo_rva(void) { return g_eng.editor_pump_rva ? g_eng.editor_pump_rva : RVA_EDITOR_FRAME_LO; }
static uintptr_t editor_hi_rva(void) { return editor_lo_rva() + EDITOR_FRAME_SPAN; }
static uintptr_t resolver_lo_rva(void) { return g_eng.resolver_rva ? g_eng.resolver_rva : RVA_RESOLVER_LO; }
static uintptr_t resolver_hi_rva(void) { return resolver_lo_rva() + RESOLVER_SPAN; }

#define SHIELD_MAX_REDIRECTS 8
static volatile LONG g_redirects = 0;    /* Class-B/fallback Error(6) redirects (runaway-guarded) */
static volatile LONG g_classa_seen = 0;  /* Class-A in-editor recoveries (log rate-limit only; NOT capped) */
static volatile LONG g_visleaf_seen = 0; /* vis-leaf micro-recoveries (log rate-limit only; NOT capped) */
static volatile LONG g_diag_seen = 0;    /* DIAGNOSTIC: count of AVs the VEH has logged */
static volatile LONG g_rn_seen = 0;      /* DIAGNOSTIC: render-node (vis-leaf) fault detail count */
static char g_why[200];   /* persists: Error(6) reads it as the fmt (rcx) after we return */
static char g_diag[260];
static char g_rndiag[220];

/* ---- CRASH POPUP + STACK: name exactly what faulted (type + site + call stack) to the log AND a user
 * dialog, so a serious fault is never silent. Rate-limited (a per-frame fault would otherwise spam). ------ */
#define SHIELD_MAX_POPUP 3
static volatile LONG g_popup_seen = 0;
static char g_crashbody[1024];
static char g_crashstk[512];

/* ---- FIRST-CHANCE CRASH LOGGER (crash-forensics: name a death the recovery paths never touch) --------
 * The recovery logic below only ACTS on AVs/HW-faults whose rip is INSIDE DOOM; it CONTINUE_SEARCHes every
 * other first-chance exception -- a crash-class fault in a NON-DOOM module (the SnapHak UI DLL, a backend
 * detour, Qt) or a fastfail/heap-stop -- so those deaths left NO shield trace and an attached debugger saw
 * only a bare process-terminated. This LOG-ONLY block records ANY crash-class first-chance exception in ANY
 * module (code + name + rip + module+offset + fault addr), rate-limited + immediate-flush, then falls through
 * so the recovery logic runs UNCHANGED (it never alters the exception disposition). */
#define SHIELD_MAX_FIRSTCHANCE 64
static volatile LONG g_firstchance_seen = 0;
static char g_fcdiag[320];

/* crash-class = a severity-ERROR status code (0xC.../0xE...), minus the benign first-chance noise + the DOOM
 * C++ throw (0xE06D7363) which LAYER 2 below handles + logs on its own. */
static int is_crash_class(DWORD c)
{
    if (c == 0x80000003u /*BREAKPOINT*/ || c == 0x80000004u /*SINGLE_STEP*/ ||
        c == 0x40010005u /*DBG_CONTROL_C*/ || c == 0x40010006u /*DBG_PRINTEXCEPTION_C*/ ||
        c == 0x4001000Au /*DBG_PRINTEXCEPTION_WIDE_C*/ || c == 0x406D1388u /*MS_VC_SET_THREAD_NAME*/ ||
        c == 0xE06D7363u /*C++ throw -- handled by LAYER 2*/)
        return 0;
    return (c >> 30) == 3;   /* STATUS_SEVERITY_ERROR */
}

static const char *exc_name(DWORD c)
{
    switch (c) {
        case 0xC0000005u: return "ACCESS_VIOLATION";
        case 0xC0000006u: return "IN_PAGE_ERROR";
        case 0xC000001Du: return "ILLEGAL_INSTRUCTION";
        case 0xC0000025u: return "NONCONTINUABLE";
        case 0xC0000094u: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000096u: return "PRIVILEGED_INSTRUCTION";
        case 0xC00000FDu: return "STACK_OVERFLOW";
        case 0xC0000374u: return "HEAP_CORRUPTION";
        case 0xC0000409u: return "STACK_BUFFER_OVERRUN/FASTFAIL";
        case 0xC000041Du: return "FATAL_USER_CALLBACK";
        default:          return "status";
    }
}

/* Resolve an address to "<module-basename>+0x<offset>". Best-effort; a failed lookup yields "?". Uses the
 * FROM_ADDRESS/UNCHANGED_REFCOUNT flags (no LoadLibrary side effect); called only on the rate-limited log path. */
static void module_at(void *addr, char *out, size_t cap, uintptr_t *off)
{
    HMODULE h = NULL;
    out[0] = '\0';
    if (off) *off = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &h) && h) {
        char path[MAX_PATH];
        const char *base = "?";
        if (GetModuleFileNameA(h, path, MAX_PATH)) {
            char *s = strrchr(path, '\\');
            base = s ? s + 1 : path;
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s", base);
        if (off) *off = (uintptr_t)addr - (uintptr_t)h;
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "?");
    }
}

static int rip_in_doom(void *rip)
{
    return g_doom_base &&
           (uint8_t *)rip >= g_doom_base &&
           (uint8_t *)rip <  g_doom_base + g_doom_size;
}

/* "wild" = a data address NOT backed by committed memory (the conn_oor out-of-range-index -> garbage-
 * pointer class). Mirrors the reference implementation's findModuleByAddress(fa)==null intent. A committed-but-
 * garbage heap address would slip past this; tighten to a module/heap check if a fixture needs it. */
static int is_wild(void *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) == 0) return 1;   /* unqueryable -> wild */
    if (mbi.State != MEM_COMMIT) return 1;                     /* free / reserved -> a read faults */
    /* Committed BUT a read still faults: PAGE_NOACCESS. The conn_oor wild index landed on a
     * committed-but-no-access page (LIVE crash 2026-06-19: CSR-build AV, state=MEM_COMMIT prot=0x1,
     * slipped the old State-only check -> the shield missed the AV and DOOM died). Mask off the
     * modifier bits (PAGE_GUARD 0x100, PAGE_NOCACHE 0x200, ...) so stack GUARD pages are NOT treated
     * as wild (those legitimately drive stack growth and must not be redirected). */
    if ((mbi.Protect & 0xFF) == PAGE_NOACCESS) return 1;
    return 0;
}

/* ---- Class-A recovery primitive: unwind the faulting thread to a known-good ancestor frame -----------
 * Walk the call stack from `ctx` (a COPY of the fault context) up to `maxframes` real frames with the OS
 * unwinder (RtlLookupFunctionEntry + RtlVirtualUnwind), which correctly restores callee-saved registers +
 * RSP at each step -- unlike a naive RIP/RSP poke, which would leave the resume frame's nonvolatiles
 * clobbered. Stop when RIP lands in the DOOM RVA range [lo,hi): leave `ctx` at that frame's resume state
 * (i.e. as if the inner call had returned) and return 1. A frameless leaf (no RUNTIME_FUNCTION -- e.g. the
 * visibility predicate 0xD32A30) is unwound by popping its return address off [RSP]. Returns 0 if the
 * range is not reached within the cap (the caller then falls back to Error(6)).
 *
 * Safe to call from the VEH: the fault is a wild DATA read, so the thread stack is intact + walkable, and
 * RtlVirtualUnwind takes no locks the fault could hold. UNW_FLAG_NHANDLER => we do NOT run __finally / EH
 * termination handlers in the skipped frames (at worst a per-frame temp leak; validated live). */
static int unwind_to_rva_range(CONTEXT *ctx, uintptr_t lo_rva, uintptr_t hi_rva, int maxframes)
{
    int i;
    for (i = 0; i < maxframes; i++) {
        if (rip_in_doom((void *)ctx->Rip)) {
            uintptr_t r = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
            if (r >= lo_rva && r < hi_rva) return 1;
        }
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)ctx->Rip, &imageBase, NULL);
            if (fn == NULL) {
                /* frameless leaf: the return address sits at [RSP], no saved nonvolatiles. */
                uintptr_t sp = (uintptr_t)ctx->Rsp;
                if (sp == 0 || is_wild((void *)sp)) return 0;
                ctx->Rip = *(uintptr_t *)sp;
                ctx->Rsp = sp + 8;
            } else {
                PVOID  handlerData = NULL;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)ctx->Rip, fn,
                                 ctx, &handlerData, &establisherFrame, NULL);
            }
        }
        if (ctx->Rip == 0) return 0;
    }
    return 0;
}

/* ---- Capture the FAULTING call stack as a compact "DOOM+0xRVA <- ..." string (for the crash log + popup).
 * Walks a COPY of the fault context with the OS unwinder (like unwind_to_rva_range); non-DOOM frames show as
 * module+off. SEH-guarded per frame so a wild frame just ends the walk -- never re-faults the VEH. */
static void capture_fault_stack(const CONTEXT *ctx_in, char *out, size_t cap, int maxframes)
{
    CONTEXT ctx = *ctx_in;
    size_t used = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < maxframes && used + 12 < cap; i++) {
        __try {
            const char *sep = used ? "\n    " : "";
            if (rip_in_doom((void *)ctx.Rip)) {
                uintptr_t r = (uintptr_t)ctx.Rip - (uintptr_t)g_doom_base;
                int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "%sDOOM+0x%llx", sep,
                                    (unsigned long long)r);
                if (n <= 0) break; used += (size_t)n;
            } else {
                char mod[64]; uintptr_t moff = 0;
                module_at((void *)ctx.Rip, mod, sizeof mod, &moff);
                int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "%s%s+0x%llx", sep, mod,
                                    (unsigned long long)moff);
                if (n <= 0) break; used += (size_t)n;
            }
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)ctx.Rip, &imageBase, NULL);
            if (fn == NULL) {
                uintptr_t sp = (uintptr_t)ctx.Rsp;
                if (sp == 0 || is_wild((void *)sp)) break;
                ctx.Rip = *(uintptr_t *)sp; ctx.Rsp = sp + 8;
            } else {
                PVOID hd = NULL; DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)ctx.Rip, fn, &ctx, &hd, &ef, NULL);
            }
            if (ctx.Rip == 0) break;
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
}

/* A committed, writable 4-byte slot? (guard before the shield pokes engine memory). */
static int writable_int(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD wr = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (VirtualQuery((void *)addr, &mbi, sizeof mbi) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((mbi.Protect & wr) == 0) return 0;
    return 1;
}

/* ---- In-shield REVERT: neutralize the corrupt connection so the resolver stops re-faulting ----------
 * The Class-A unwind survives the fault, but if the bad CSR value PERSISTS the resolver re-faults every
 * frame -> the module-view draw is aborted each frame -> editor interaction degrades (LIVE 2026-06-19:
 * objects stopped highlighting / grabbing). Unlike an external instrumentation tool (which knew the original value), the shield
 * neutralizes the bad connection BLIND, from the fault context: the visibility leaf 0xD32A30 is frameless,
 * so at the leaf/resolver fault RBP still holds the RESOLVER 0x5E0AD0 frame. The faulting connection entry
 * = *( *(RBP-CSR_FRAME_COL_HOLDER) ) + lVar19*4  (lVar19 = *(RBP-CSR_FRAME_LOOPIDX)); the out-of-range
 * index value is in RSI; a guaranteed-valid index (the source entity, outer loop counter) is in R12. We
 * CLAMP the bad entry to the source index -> next frame reads a valid index -> no fault -> full draw.
 * HEAVILY GUARDED: only fires if RIP is in the resolver/leaf region, every pointer in the chain is
 * committed+readable, the located entry actually holds the faulting value (RSI), and the slot is writable
 * -- otherwise skip (survive-only). Returns 1 if it clamped. (DIRECT: disasm 0x5E0AD0.) */
static int try_revert_csr_entry(const CONTEXT *ctx)
{
    uintptr_t rva, rbp, holder, colArr, entry_addr;
    int64_t   loopidx;
    int       bad_value, valid_value;

    if (!rip_in_doom((void *)ctx->Rip)) return 0;
    rva = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
    /* resolver range rides the sig-resolved entry; the frameless vis-leaf stays on its recipe-tagged RVA. */
    if (!((rva >= resolver_lo_rva() && rva < resolver_hi_rva()) ||
          (rva >= RVA_VIS_LEAF_LO && rva < RVA_VIS_LEAF_HI)))
        return 0;

    rbp = (uintptr_t)ctx->Rbp;
    if (rbp == 0) return 0;
    if (is_wild((void *)(rbp - CSR_FRAME_COL_HOLDER))) return 0;
    if (is_wild((void *)(rbp - CSR_FRAME_LOOPIDX)))    return 0;

    holder = *(uintptr_t *)(rbp - CSR_FRAME_COL_HOLDER);     /* R15 = ET+0x5e0 */
    if (holder == 0 || is_wild((void *)holder)) return 0;
    colArr = *(uintptr_t *)holder;                           /* the column int array */
    if (colArr == 0 || is_wild((void *)colArr)) return 0;

    loopidx = *(int64_t *)(rbp - CSR_FRAME_LOOPIDX);
    if (loopidx < 0 || loopidx > 0x7FFFFFF) return 0;        /* sane column-index bound */
    entry_addr = colArr + (uintptr_t)loopidx * 4;
    if (!writable_int(entry_addr)) return 0;

    bad_value   = *(int *)entry_addr;
    valid_value = (int)ctx->R12;                             /* the source entity index (in range) */
    if (bad_value != (int)ctx->Rsi) return 0;                /* must match the faulting deref index */
    if (bad_value == valid_value)   return 0;                /* already safe */

    *(int *)entry_addr = valid_value;                        /* CLAMP -> next frame reads a valid index */
    return 1;
}

/* ---- In-shield NEUTRALIZE a dangling render-node connection ref (the create-timeline draw-fault class) --
 * A DIFFERENT fault shape from try_revert_csr_entry's corrupt-CSR-index. Here the visibility leaf 0xD32A30
 * faults reading *(node+0x70) / *(node+0x80) -- an OUTPUT / INPUT connection-node ref -- that DANGLES: a
 * node-less entity (a reclassed idTarget_Timeline) inherited a now-freed connection-node ptr from a prior
 * node-having occupant (an idTarget_Command) of its render-node slot. The per-module view build 0x5E6620
 * only clears such a ref LATER (its else-branch never rebuilds a node-less entry), so on the exit-Play view
 * rebuild the resolver reads the dangling ref one frame before the clear -> wild AV, re-firing every frame.
 * At the frameless leaf RCX still = the render-node; RAX = the faulting ref value (*(node+0x70) at 0xD32A39,
 * or *(node+0x80) at 0xD32A4B). We NULL the exact ref that faulted (its value == RAX) so the next frame's
 * predicate reads a null ref -> no wire drawn, no fault -- the correct state for a node-less entity. HEAVILY
 * GUARDED (rip in the vis-leaf, node committed+readable, the ref slot writable). Returns 1 if it nulled a
 * ref. (DIRECT: disasm 0xD32A30 = MOV RAX,[RCX+0x70]; TEST; JZ; CMP byte [RAX+0x30].) */
static int try_neutralize_rendernode(const CONTEXT *ctx)
{
    uintptr_t rva, node, rax;
    if (!rip_in_doom((void *)ctx->Rip)) return 0;
    rva = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
    if (!(rva >= RVA_VIS_LEAF_LO && rva < RVA_VIS_LEAF_HI)) return 0;
    node = (uintptr_t)ctx->Rcx;                       /* the render-node (RCX, preserved across the leaf) */
    rax  = (uintptr_t)ctx->Rax;                        /* the faulting connection-node ref value */
    if (node == 0 || is_wild((void *)node) || rax == 0) return 0;
    __try {
        if (*(uintptr_t *)(node + 0x70) == rax && writable_int(node + 0x70)) {
            *(uintptr_t *)(node + 0x70) = 0;
            return 1;
        }
        if (*(uintptr_t *)(node + 0x80) == rax && writable_int(node + 0x80)) {
            *(uintptr_t *)(node + 0x80) = 0;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

/* LAYER 1 -- the non-AV hardware-fault codes the shield ALSO recovers: any CPU exception that is a crash
 * the engine does not handle (illegal/privileged instruction, int/float divide + the float family, in-page,
 * datatype misalignment, array-bounds). These skip the AV-only wild-pointer gate and route straight to the
 * Class-B Error(6) redirect. DELIBERATELY NOT here: STACK_OVERFLOW (the VEH would run on the exhausted
 * stack -- needs a guard-page handler, deferred); C++ throws (0xE06D7363, engine-caught); breakpoints;
 * DBG_PRINTEXCEPTION_C -- those pass through uncaptured. AV keeps its wild gate + the Class-A unwind. */
static int is_other_hw_fault(DWORD code)
{
    switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return 1;
    default:
        return 0;
    }
}

/* Count of C++ throws the shield has gate-forced (log rate-limit only). */
static volatile LONG g_cxx_seen = 0;

/* LAYER 2 -- force idCommonLocal::Frame's idException catch to RECOVER (drop-to-menu + resume) instead of
 * rethrowing to the WinMain terminal exit. DIRECT (error-dispatcher-and-recovery.md): that catch recovers
 * iff errState(0x6dde19c)==0 AND both throw-gate suppressors are 0 (load_state!=1 is always true). An
 * Error(6)/downgraded-FatalError SETS errState as it raises, so by the time the throw reaches the catch the
 * gate is shut -> rethrow -> exit. Clearing these on the THROW (first-chance, before the catch reads them)
 * is what flips a fatal rethrow into a live recovery. SEH-guarded vs a shifted RVA.
 *
 * DIRECT (catch funclet 0x1F5B937, decompiled live): recover iff `(char)errState(0x6dde19c)==0 &&
 * load_state(0x6dde198)!=1`. The errState getter returns only the LOW BYTE, so errState=0x100 already
 * passes. We open the gate for errors that REACH the Frame catch: clear errState's low byte, neutralize
 * load_state ONLY when it is the blocking value 1 (LOADING) -- leave 0/2/3 alone (they already pass != 1
 * and the engine relies on them), and clear both throw-gate suppressors. NOTE: this cannot help an error
 * an INNER engine handler catches before idCommonLocal::Frame (e.g. the incompatible-class decl error,
 * which is a prevent-not-recover case). SEH-guarded vs a shifted RVA. */
static void force_recovery_gate(void)
{
    if (g_doom_base == NULL) return;
    __try {
        *(volatile int32_t *)(g_doom_base + RVA_ERRSTATE) = 0;               /* the (char) low-byte getter -> 0 */
        if (*(volatile int32_t *)(g_doom_base + RVA_LOAD_STATE) == 1)        /* only neutralize LOADING (the blocker) */
            *(volatile int32_t *)(g_doom_base + RVA_LOAD_STATE) = 0;
        *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_A) = 0;
        *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_B) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static LONG CALLBACK shield_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    void *rip  = (void *)ep->ContextRecord->Rip;

    /* FIRST-CHANCE CRASH LOGGER (LOG-ONLY -- the exception disposition is NOT touched here; the recovery
     * layers below run exactly as before). Names any crash-class fault the recovery paths ignore (a fault in
     * a non-DOOM module, a fastfail/heap-stop) so a `crash=None`/no-dump death is no longer anonymous.
     * SEH-guarded: a fault while formatting the diagnostic must never crash inside the VEH. */
    if (is_crash_class(code) && InterlockedIncrement(&g_firstchance_seen) <= SHIELD_MAX_FIRSTCHANCE) {
        __try {
            char mod[80]; uintptr_t moff = 0;
            module_at(rip, mod, sizeof mod, &moff);
            void *fa = (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                         ? (void *)er->ExceptionInformation[1] : NULL;
            uintptr_t drva = rip_in_doom(rip) ? ((uintptr_t)rip - (uintptr_t)g_doom_base) : 0;
            _snprintf_s(g_fcdiag, sizeof g_fcdiag, _TRUNCATE,
                "FIRST-CHANCE code=0x%08lx (%s) rip=%p mod=%s+0x%llx doom_rva=0x%llx fault=%p",
                (unsigned long)code, exc_name(code), rip, mod, (unsigned long long)moff,
                (unsigned long long)drva, fa);
            shield_fault fc = { "fc", (int)code, g_fcdiag, drva, (uintptr_t)fa };
            shield_emit(&fc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* LAYER 2 -- a DOOM C++ throw (MSVC 0xE06D7363) is the idException an Error(6)/downgraded-FatalError
     * raises. NOTE: a C++ throw's rip is inside kernel32's RaiseException, NOT DOOM, so identify it by the
     * THROWING MODULE -- ExceptionInformation[3] is the throw's image base on x64 (the RVA-encoded ThrowInfo
     * base). Force the engine's recovery gate open on the throw (first-chance, before the idCommonLocal::Frame
     * catch reads it), then CONTINUE_SEARCH so the engine's OWN catch does the drop-to-menu recovery -- this
     * is what makes a thrown engine error SURVIVE instead of rethrowing to the WinMain terminal exit. (Engine
     * throws the engine catches locally are unaffected -- only the Frame catch consults errState.) */
    if (code == 0xE06D7363 && g_doom_base != NULL &&
        er->NumberParameters >= 4 && er->ExceptionInformation[3] == (ULONG_PTR)g_doom_base) {
        int es = -1, ls = -1;
        uintptr_t ti = 0;   /* ThrowInfo RVA: idException=0x2ded690 (recoverable) vs idFatalException=0x2ded990 */
        __try {
            es = *(volatile int32_t *)(g_doom_base + RVA_ERRSTATE);
            ls = *(volatile int32_t *)(g_doom_base + RVA_LOAD_STATE);
            if (er->NumberParameters >= 3)
                ti = (uintptr_t)er->ExceptionInformation[2] - (uintptr_t)g_doom_base;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        force_recovery_gate();
        log_engine_error_text();     /* record the engine's verbatim error text -- e.g. a masked load-time FatalError */
        if (InterlockedIncrement(&g_cxx_seen) <= 10) {
            _snprintf_s(g_diag, sizeof g_diag, _TRUNCATE,
                "DOOM C++ throw -> forced gate (errState=%d load_state=%d throwInfo_rva=0x%llx)",
                es, ls, (unsigned long long)ti);
            shield_fault df = { "diag", -1, g_diag, 0, 0 };
            shield_emit(&df);
            /* capture the THROWING call stack -- for a masked heap-corruption FatalError ("Memory corruption
             * before block!") this names the heap operation (free/alloc/check) that detected it + its caller,
             * narrowing which structure was overflowed. SEH-guarded per frame inside the walker. */
            {
                char stk[512];
                capture_fault_stack(ep->ContextRecord, stk, sizeof stk, 20);
                if (stk[0]) { shield_fault sf = { "cxxstack", -1, stk, 0, 0 }; shield_emit(&sf); }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* LAYER 1 -- catch every HARDWARE fault in DOOM, not just AVs. An AV keeps its wild-pointer gate + the
     * Class-A in-editor unwind below; the other hardware faults (is_other_hw_fault) are crashes the engine
     * does not handle -> they fall straight through to the Class-B Error(6) redirect. Benign first-chance
     * exceptions (DBG_PRINTEXCEPTION_C on every console line, breakpoints) continue uncaptured. */
    if (code != EXCEPTION_ACCESS_VIOLATION && !is_other_hw_fault(code))
        return EXCEPTION_CONTINUE_SEARCH;

    int   is_av = (code == EXCEPTION_ACCESS_VIOLATION);
    /* fault-address + data-deref classification are AV-only (ExceptionInformation[1] is the AV address). */
    void *fault_addr = is_av ? (void *)er->ExceptionInformation[1] : NULL;
    int   data       = is_av && (fault_addr != rip);

    /* COEXISTENCE hardening (defense-in-depth beyond the install-time instrumentation gate): do NO work on a
     * non-DOOM first-chance AV. An external tool's injection (and other non-DOOM modules) raise first-chance AVs the
     * shield never acts on anyway (the act-gate below already requires rip_in_doom); early-out here so the
     * shield's handler is a near-instant pass-through for any exception not originating in DOOM -- it never
     * runs the diagnostic VirtualQuery+emit on an external tool's loader exceptions. Cheap + narrows the conflict
     * surface even if a race ever left the shield armed during injection. */
    if (!rip_in_doom(rip))
        return EXCEPTION_CONTINUE_SEARCH;

    /* DIAGNOSTIC (first 40 faults): log every captured hardware fault + the redirect-decision inputs, so an
     * un-redirected crash tells us WHY (fault addr committed / wrong class / etc.). */
    if (InterlockedIncrement(&g_diag_seen) <= 40) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T q = fault_addr ? VirtualQuery(fault_addr, &mbi, sizeof mbi) : 0;
        uintptr_t drva = (uintptr_t)rip - (uintptr_t)g_doom_base;
        _snprintf_s(g_diag, sizeof g_diag, _TRUNCATE,
            "FAULT code=0x%lx rip=%p rip_rva=0x%llx fault=%p vq=%llu state=0x%lx prot=0x%lx",
            (unsigned long)code, rip, (unsigned long long)drva, fault_addr, (unsigned long long)q,
            (unsigned long)(q ? mbi.State : 0), (unsigned long)(q ? mbi.Protect : 0));
        shield_fault df = { "diag", (int)code, g_diag, drva, (uintptr_t)fault_addr };
        shield_emit(&df);
    }

    /* RENDER-NODE fault detail (root-cause data for the create-timeline draw fault): at the vis-leaf
     * 0xD32A30 RCX = the render-node, RAX = the faulting connection-node ref. Compute the node's array
     * index (base = *(editor+0x1d0)) so the log names WHICH entity's render-node slot dangled + its
     * +0x70/+0x80 values. SEH-guarded (a wild read while formatting must never re-fault the VEH). */
    {
        uintptr_t rrva = (uintptr_t)rip - (uintptr_t)g_doom_base;
        if (is_av && rrva >= RVA_VIS_LEAF_LO && rrva < RVA_VIS_LEAF_HI &&
            InterlockedIncrement(&g_rn_seen) <= 12) {
            __try {
                uintptr_t node = (uintptr_t)ep->ContextRecord->Rcx;
                uintptr_t rax  = (uintptr_t)ep->ContextRecord->Rax;
                uintptr_t ed   = (uintptr_t)g_doom_base + RVA_EDITOR_SINGLETON;
                uintptr_t base = *(uintptr_t *)(ed + 0x1d0);
                long idx = (base && !is_wild((void *)base)) ? (long)(((intptr_t)node - (intptr_t)base) / 0x180) : -1;
                uintptr_t p70 = 0, p80 = 0;
                if (!is_wild((void *)node)) { p70 = *(uintptr_t *)(node + 0x70); p80 = *(uintptr_t *)(node + 0x80); }
                _snprintf_s(g_rndiag, sizeof g_rndiag, _TRUNCATE,
                    "RN-FAULT node=%p idx=%ld base=%p +70=%p +80=%p rax=%p",
                    (void *)node, idx, (void *)base, (void *)p70, (void *)p80, (void *)rax);
                shield_fault rf = { "rn", (int)code, g_rndiag, rrva, rax };
                shield_emit(&rf);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    uintptr_t rva = (uintptr_t)rip - (uintptr_t)g_doom_base;

    /* AV-specific: a WILD data AV is the in-editor draw-fault class (Class-A). A committed-but-valid AV is
     * left to the engine's own SEH (redirecting one the engine would have handled is worse than its local
     * handling -- fault-shield-recovery.md scope). Non-AV hardware faults skip this gate -> Class-B. */
    if (is_av) {
        /* ===== VIS-LEAF MICRO-RECOVERY (BEFORE the wild-gate + Class-A/Class-B) =========================
         * The render-node visibility predicate FUN_140d32a30 (frameless leaf) derefs node->+0x70 / +0x80
         * connection refs at 4 sites (0xd32a30/39/3f/4b). During the create-timeline / node-less rebuild
         * (exit-Play, tab-out to Blueprint, editor re-entry) those slots are transiently bad, so a deref
         * faults. The SAFE, correct result for a bad/absent connection is FALSE ("no valid connection") --
         * exactly what a clean node-less slot returns. So redirect RIP to the leaf's own XOR AL,AL;RET tail
         * (0xd32a54) and resume: the predicate returns FALSE, the resolver skips the node, the frame
         * completes. STACK-INDEPENDENT (works in EntityMode, Blueprint, AND editor re-entry alike -- unlike
         * the Class-A editor-Think unwind, which only claims some stacks and lets the rest fall to the heavy
         * Class-B Error(6)+toast+MessageBox+navigate path -- the cause of the "Error Detected" toast that
         * still fired on tab-out). SILENT + light (one RIP write; no notice/dialog/navigate). The leaf is
         * frameless so RSP still points at the return address -> the injected RET returns cleanly to the
         * resolver. Transient-frame skip: once the rebuild settles the predicate runs normally, so a real
         * connection loses at most one frame of overlay. RE-DERIVE: RVA_VIS_LEAF_* in engine_layout.h. */
        if (g_doom_base && rva >= RVA_VIS_LEAF_LO && rva < RVA_VIS_LEAF_FALSE) {
            /* Per-node skip: redirect to the predicate's own XOR AL,AL;RET tail (return FALSE = no valid
             * connection, what a clean node-less slot returns). A CLEAN RETURN from the frameless leaf -- it
             * does NOT unwind past the resolver/build frames. (A tried unwind-to-editor-Think "abort the whole
             * draw" fast-path was REVERTED: unwinding the frameless leaf out of deep in-flight resolver/build
             * frames corrupted the engine heap on exit-Play -- "Memory corruption before block!". The clean
             * per-node return is safe; it is slower under a storm but never corrupts.) Silent. */
            if (InterlockedIncrement(&g_visleaf_seen) <= 3) {
                _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                    "vis-leaf render-node fault @ 0x%llx (rip+0x%llx) -> predicate forced FALSE (skip node), resumed",
                    (unsigned long long)(uintptr_t)fault_addr, (unsigned long long)rva);
                shield_fault vf = { "visleaf", -1, g_why, rva, (uintptr_t)fault_addr };
                shield_emit(&vf);
            }
            ep->ContextRecord->Rip = (DWORD64)((uintptr_t)g_doom_base + RVA_VIS_LEAF_FALSE);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (!(data && is_wild(fault_addr)))
            return EXCEPTION_CONTINUE_SEARCH;

    /* ---- Class A: an IN-EDITOR draw fault. If the per-frame editor Think 0x523140 is an ancestor on the
     * faulting stack, ABORT the faulting module-view draw by unwinding the thread back to it and resuming
     * -- the frame completes, the editor stays live (DIRECT: the resolver builds into a caller stack list
     * the consumer draws only `if (0<count)`; the editor Think runs unconditional work after the 0x521D90
     * call). NO Error(6), NO thread-parking modal. This recovers in place each frame the corrupt CSR is
     * drawn (~1 fault/frame, always making forward progress), so it is NOT counted against the runaway
     * guard; the log is rate-limited to avoid per-frame spam. Unwind a COPY so a miss leaves the real
     * context untouched for the fallback. */
    {
        CONTEXT unwound = *ep->ContextRecord;
        if (unwind_to_rva_range(&unwound, editor_lo_rva(), editor_hi_rva(), 32)) {
            /* Neutralize the corrupt connection (BLIND -- the shield doesn't know the original value) so
             * the resolver stops re-faulting + the editor regains full per-frame function. Uses the
             * ORIGINAL fault context (RBP still = the resolver frame; the leaf is frameless). */
            int reverted   = try_revert_csr_entry(ep->ContextRecord);
            int rn_cleared = try_neutralize_rendernode(ep->ContextRecord);
            if (InterlockedIncrement(&g_classa_seen) <= 5) {
                _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                    "in-editor draw fault @ 0x%llx (rip+0x%llx) -> aborted draw%s%s, resumed editor frame",
                    (unsigned long long)(uintptr_t)fault_addr, (unsigned long long)rva,
                    reverted ? " + reverted bad connection" : "",
                    rn_cleared ? " + cleared dangling render-node ref" : "");
                shield_fault fa = { "action", -1, g_why, rva, (uintptr_t)fault_addr };
                shield_emit(&fa);
            }
            /* NO in-game notice for Class-A recoveries. Class-A = a RECOVERED in-editor draw fault -- a
             * transient render glitch the shield aborts + resumes in place. The create-timeline / node-less
             * rebuild produces a burst of these across MORE THAN ONE site: the vis-leaf predicate 0xd32a30
             * (micro-recovered before we ever get here) AND the module-render build ~0x5e68e7 (aborted here).
             * A per-site notice gate is whack-a-mole; the whole CLASS is spurious -- arming "Error Detected
             * in map logic" for a fault the shield already recovered in place was the exact repeating-toast
             * complaint. So Class-A recoveries are SILENT (every one is still logged to shield_faults.log for
             * diagnostics). SERIOUS faults take the Class-B path below, which keeps its notice + "Fault
             * Caught" dialog. */
            /* (intentionally no notice_request() here) */
            *ep->ContextRecord = unwound;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    }   /* end if (is_av) -- a wild AV the editor-unwind didn't claim falls through to Class-B */

    /* ---- Class B (Layer 1): a wild AV the editor-unwind did NOT claim, OR a non-AV hardware fault in DOOM
     * (illegal/priv instruction, int/float divide, in-page, ...). Redirect into the engine's own recoverable
     * Error(6) + the proven editor-exit -> My-Maps browser. Runaway-guarded; CAN surface DOOM's recoverable-
     * error drop-to-menu -- survivable, vastly better than the hard crash these faults would otherwise be. */
    if (InterlockedIncrement(&g_redirects) > SHIELD_MAX_REDIRECTS)
        return EXCEPTION_CONTINUE_SEARCH;                     /* runaway guard -> let the OS take it */

    _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
        "shield: caught fault 0x%lx at rip+0x%llx -> recovered via Error(6)",
        (unsigned long)code, (unsigned long long)rva);
    shield_fault f = { "load", -1, g_why, rva, (uintptr_t)fault_addr };
    shield_emit(&f);

    /* CRASH DETAIL + USER POPUP: capture the faulting call stack, log it (the citable record), and show the
     * user a dialog naming exactly what faulted -- so a serious fault is never silent. Rate-limited
     * (SHIELD_MAX_POPUP) so a per-frame fault can't spam; SEH-guarded so building/showing it can never re-fault
     * the VEH. MessageBoxA blocks the faulting thread until dismissed, then we resume into the Error(6) recovery
     * below -- acceptable for a crash. */
    if (InterlockedIncrement(&g_popup_seen) <= SHIELD_MAX_POPUP) {
        __try {
            const char *dir = "";
            if (is_av && er->NumberParameters >= 1)
                dir = er->ExceptionInformation[0] == 1 ? " (write to)" :
                      er->ExceptionInformation[0] == 8 ? " (execute at)" : " (read from)";
            capture_fault_stack(ep->ContextRecord, g_crashstk, sizeof g_crashstk, 14);
            shield_fault sk = { "stack", (int)code, g_crashstk, rva, (uintptr_t)fault_addr };
            shield_emit(&sk);   /* the full call stack -> shield_faults.log */
            _snprintf_s(g_crashbody, sizeof g_crashbody, _TRUNCATE,
                "SnapHak caught a fault in DOOM. The editor will try to recover, but this session may be "
                "unstable -- save to a new slot and restart if things look wrong.\n\n"
                "Fault:  %s (0x%08lx)%s 0x%llx\n"
                "Where:  DOOMx64vk.exe+0x%llx\n\n"
                "Call stack:\n    %s\n\n"
                "Full details were written to:\n    <DOOM folder>\\snaphak_logs\\shield_faults.log",
                exc_name(code), (unsigned long)code, dir, (unsigned long long)(uintptr_t)fault_addr,
                (unsigned long long)rva, g_crashstk[0] ? g_crashstk : "(unavailable)");
            /* FREE THE MOUSE: DOOM clips the cursor to its window + hides it (captured input), so the user
             * can't move the pointer to the dialog. Un-clip + force the system cursor visible (ShowCursor is
             * a refcount -- loop until non-negative, bounded) so the OK button is actually reachable. */
            ClipCursor(NULL);
            { int cc = 0; while (ShowCursor(TRUE) < 0 && ++cc < 32) {} }
            MessageBoxA(NULL, g_crashbody, "SnapHak - Fault Caught",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* Arm the recovery router: the engine's own Frame catch keeps us alive but does NOT navigate (it
     * resumes the editor on the dangling render-world -> re-fault loop). The frame-hook drives the
     * proven editor-exit -> My-Maps browser so the loop terminates. */
    recovery_arm();

    /* MESSAGE HARVEST (Class-B): we are about to resume INTO Error(6), which formats g_why and strncpy's it
     * into the engine's last-error global (DAT_146ddd990) before throwing. Arm the HARVESTING toast so that
     * if the recovery resumes in-editor (a genuine engine Error/FatalError the Frame catch recovers in
     * place), the recover-in-place toast carries the engine's VERBATIM error text instead of the generic
     * notice -- informative like OG's popup, survivable unlike OG's kill. The toast renders only once an
     * editor screen exists + self-dedups; if the recovery navigates to the browser it simply never shows
     * (no engine-native surface there), which is the correct bad-LOAD behavior. */
    notice_request_msg();

    /* Open Error(6)'s throw gate: BOTH suppressors clear, or the throw becomes ExitProcess(1)
     * (DAT_146faf820 is also the render-cap suppressor -- see engine_layout.h / the truth). The
     * suppressors are recipe-tagged DATA-global RVAs; a shifted build could land the literal on an
     * unmapped/RO page, so SEH-guard the writes (P6) -- a fault here would otherwise crash INSIDE the VEH.
     * If the write faults we still resume into Error(6) (the gate may already be open / re-armed). */
    __try {
        *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_A) = 0;
        *(volatile int32_t *)(g_doom_base + RVA_SUPPRESSOR_B) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* recipe-tagged suppressor RVA shifted on this build -> log + continue (Error6 may still recover) */
        shield_fault sf = { "sig", -1, "suppressor RVA write faulted (re-derive 0x6faf820/0x6faf8b0)", 0, 0 };
        shield_emit(&sf);
    }

    /* Simulate `call Error(g_why)` from the faulting site: 16-align rsp, push the faulting rip as a real
     * (unwindable) return address so idException unwinds up to idCommonLocal::Frame's catch. Error(6) is
     * SIG-RESOLVED (g_eng.error6, RVA fallback). */
    uintptr_t sp = (ep->ContextRecord->Rsp & ~(uintptr_t)0xF) - 8;
    *(uintptr_t *)sp = (uintptr_t)rip;
    ep->ContextRecord->Rsp = sp;
    ep->ContextRecord->Rcx = (uintptr_t)g_why;                       /* Error(fmt) in rcx */
    ep->ContextRecord->Rip = g_eng.error6                            /* resume INTO Error(6) (sig-resolved) */
                           ? (uintptr_t)g_eng.error6
                           : (uintptr_t)(g_doom_base + RVA_ERROR6);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int veh_install(void)
{
    return AddVectoredExceptionHandler(1 /* first-in-chain */, shield_veh) != NULL;
}
