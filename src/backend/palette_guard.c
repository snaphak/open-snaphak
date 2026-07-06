/* palette_guard.c -- the editor WIRE-RENDER render-node guard.
 *
 * NOTE ON THE NAME. This file (and sh_palette_guard_install) keep their legacy names so the single dllmain
 * call site + the build source list are untouched; the CONTENT is the render-node guard described below. The
 * two earlier guards this file used to host -- the palette-migration name-sort guard (0x5ec6c0) and the
 * entity-collection teardown guard (0x4e9aa0) -- were REMOVED: they targeted the "dangling embedded idStr"
 * theory (crash face 0x1ab32ee), which is NOT the create-timeline reload crash. Evidence: the palette guard
 * logged "reset 0" on every load (it never once fired). The idStr/decl face's actual ROOT fix lives in
 * apply_engine.c (ae_apply_one's timeline decl-unregister) and stays; this file now owns only the render-node
 * face. (Trivial follow-up: rename the file/function to rendernode_guard.)
 *
 * ROOT (DIRECT, reverse-engineered + live-measured). The editor's per-frame wire/connection overlay renderer
 * walks a per-entity RENDER-NODE array and, for each entity, reads its output-node (+0x70) and input-node
 * (+0x80) references and dereferences their +0x30 status byte (predicate FUN_140d32a30 `cmp byte [rax+0x30]`).
 * The connection resolver FUN_1405e0ad0(OP, arrHandle, out, i, flag) drives that walk: `arrHandle` (its 2nd
 * arg) is the render-node vector handle {base @+0x00, size @+0x08 (int), cap @+0x10}, a PERSISTENT member of
 * the editor VIEW (editor+0x1d0) that SURVIVES play->exit->re-enter (only the map reloads). A from-scratch
 * timeline is a NODE-LESS is-target: it has no output/input node, but the paste+reclass leaves a render-node
 * whose +0x70 references an output-node object that the reload frees while the persistent slot keeps the now
 * dangling reference (or, after the vector reallocs on reload, fresh heap garbage like 0x1/0x4). Next re-enter,
 * the renderer walks that record and dereferences the stale reference -> access violation (rip 0xd32a39).
 *
 * FIX. Detour the resolver (0x5e0ad0) and, BEFORE calling the original, walk the render-node array and null any
 * record's +0x70/+0x80 that is NOT a readable object reference (a small integer, or an unmapped/freed pointer).
 * The predicate treats a null node reference as "no node" and skips it -- which is the CORRECT result for a
 * node-less timeline (it has no wire to draw), so this repairs the render rather than merely masking a fault. A
 * VALID node reference (a readable heap object) is left untouched. We never reorder the array or free anything.
 * SEH-guarded end to end + game-thread (the resolver runs on the render/game thread), so it cannot fault the
 * guard or race the editor.
 *
 * Clean-room from our own reverse-engineering. BUILD-SPECIFIC RVAs + struct offsets (recipe-tagged) --
 * RE-DERIVE per build: disasm 0x5e0ad0 (prologue = four arg home-stores, 20 bytes, clean boundary before the
 * pushes -> STOLEN=20; its 2nd arg is the render-node vector handle = View+0x60 = editor+0x1d0). Live-measured
 * the vector shape {base, size, cap} + the 0x180 stride + the +0x70/+0x80 node refs + the +0x30 status byte the
 * predicate FUN_140d32a30 reads. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "palette_guard.h"
#include "hook.h"            /* install_inline_hook */
#include "backend_log.h"

#define RVA_WIRE_RESOLVER      0x5e0ad0u   /* FUN_1405e0ad0(OP, arrHandle, out, i, flag): the wire-render walk */
#define WIRE_RESOLVER_STOLEN   20u         /* clean prologue: 4x arg home-store (mov [rsp+8/+0x10/+0x18/+0x20], rcx/rdx/r8/r9d) = 20 bytes, before the PUSH block */

/* render-node vector handle (the resolver's 2nd arg = View+0x60 = editor+0x1d0). */
#define RN_BASE_OFF            0x00        /* base ptr (first 0x180 record) */
#define RN_SIZE_OFF            0x08        /* logical element count (int) */
#define RN_CAP_OFF             0x0c        /* allocated capacity (int) -- the resolver's entity high-water can exceed SIZE for a freshly-added entity yet stay within CAP; those [size,cap) slots are uninitialized */
#define RN_STRIDE             0x180u       /* one per-entity render-node record */
#define RN_COUNT_MAX          0x4000       /* editor entity cap ~0x3ffe; a larger size = a bad read -> skip */

/* the two node references a render-node record holds (each a ptr to a vtable'd node object). */
#define RN_OUTNODE_OFF        0x70         /* output-node reference (the crash field) */
#define RN_INNODE_OFF         0x80         /* input-node reference */
#define NODE_STATUS_OFF       0x30         /* the byte the predicate FUN_140d32a30 reads: `cmp byte [node+0x30]` */
#define NODE_MIN_PTR          0x10000u     /* below this = a small integer, never a real heap object */

typedef void (*wire_resolver_fn)(void *op, void *arr_handle, void *out, int i, char flag);
static wire_resolver_fn g_orig_resolver = NULL;
static volatile LONG    g_reset_total   = 0;

/* Is the byte range [addr, addr+nbytes) fully backed by committed, readable memory? VirtualQuery so a freed page
 * reports its true state (a plain 1-byte SEH probe misses a buffer straddling a still-mapped page into an
 * unmapped one). Returns 0 on any un-committed / no-access / guard page in the range. */
static int mem_range_readable(const void *addr, size_t nbytes)
{
    const uint8_t *p   = (const uint8_t *)addr;
    const uint8_t *end = p + (nbytes ? nbytes : 1);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
        if (mbi.State != MEM_COMMIT) return 0;                           /* freed = MEM_FREE/MEM_RESERVE */
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        const uint8_t *region_end = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return 0;                                  /* no forward progress -> bail */
        p = region_end;
    }
    return 1;
}

/* Is `p` an INVALID node reference (would fault the predicate's `[p+0x30]` read)? null is VALID (== no node,
 * the predicate handles it). A small integer or a pointer whose [p, p+0x30] range is not committed is INVALID.
 * A readable heap object is left as-is (a real node, or a freed-but-still-mapped one whose byte read won't
 * fault -- not our crash). */
static int node_ref_invalid(const void *p)
{
    if (p == NULL) return 0;                                             /* no node -> fine */
    if ((uintptr_t)p < NODE_MIN_PTR) return 1;                          /* small integer in a pointer slot */
    return mem_range_readable(p, NODE_STATUS_OFF + 1) ? 0 : 1;          /* range not committed -> dangling */
}

/* Null one node-reference field if it is invalid. SEH-guarded so a torn record cannot fault the guard.
 * Returns 1 if it reset the field, 0 otherwise. */
static int sanitize_node_ref(uint8_t *field)
{
    void *p;
    __try { p = *(void *const volatile *)field; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }                   /* can't even read the slot -> leave it */
    if (!node_ref_invalid(p)) return 0;
    __try { *(void *volatile *)field = NULL; }                          /* -> "no node"; the old ref is never freed */
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

/* Walk the render-node array and null every invalid output/input node reference before the resolver reads them. */
static void sh_rendernode_sanitize(void *arr_handle)
{
    void *base;
    int   size, cap;
    __try {
        base = *(void *const volatile *)((const uint8_t *)arr_handle + RN_BASE_OFF);
        size = *(const volatile int *)((const uint8_t *)arr_handle + RN_SIZE_OFF);
        cap  = *(const volatile int *)((const uint8_t *)arr_handle + RN_CAP_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                                                          /* can't read the handle -- defer to the engine */
    }
    /* Walk the FULL allocation [0, cap), not just [0, size). The wire-render resolver iterates up to the entity
     * HIGH-WATER, which for a freshly-added entity (a just-created timeline) can exceed `size` (the vector's logical
     * count -- not grown) while staying within `cap` (the allocation). Those [size, cap) slots are UNINITIALIZED ->
     * a garbage +0x70/+0x80 the predicate FUN_140d32a30 dereferences = the wire-render AV 0xd32a39. Nulling them
     * before the resolver reads them averts it. cap >= size always; an absurd/unreadable cap falls back to size. */
    int n = (cap >= size && cap <= RN_COUNT_MAX) ? cap : size;
    if (base == NULL || n <= 0 || n > RN_COUNT_MAX) return;

    int reset = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *rec = (uint8_t *)base + (size_t)i * RN_STRIDE;
        reset += sanitize_node_ref(rec + RN_OUTNODE_OFF);
        reset += sanitize_node_ref(rec + RN_INNODE_OFF);
    }
    if (reset) {
        /* fires only on a real repair; the first few log (a stale/uninitialized render-node was neutralized). */
        if (InterlockedAdd(&g_reset_total, reset) <= 64) {
            char m[128];
            _snprintf_s(m, sizeof m, _TRUNCATE,
                "rendernode-guard: walked %d records (size=%d cap=%d), nulled %d invalid node ref(s)",
                n, size, cap, reset);
            backend_log(m);
        }
    }
}

static void sh_wire_resolver_detour(void *op, void *arr_handle, void *out, int i, char flag)
{
    sh_rendernode_sanitize(arr_handle);                  /* clean the render-node array BEFORE the walk reads it */
    g_orig_resolver(op, arr_handle, out, i, flag);
}

int sh_palette_guard_install(const uint8_t *module_base)
{
    if (module_base == NULL) return 0;
    if (g_orig_resolver != NULL) return 1;               /* one-shot */
    void *target = (void *)(module_base + RVA_WIRE_RESOLVER);
    void *tramp  = install_inline_hook(target, (void *)sh_wire_resolver_detour, WIRE_RESOLVER_STOLEN);
    if (tramp == NULL) {
        backend_log("rendernode-guard: install FAIL (install_inline_hook returned NULL -- re-derive 0x5e0ad0)");
        return 0;
    }
    g_orig_resolver = (wire_resolver_fn)tramp;
    backend_log("rendernode-guard: armed -- wire-render stale-node guard on the connection resolver (0x5e0ad0)");
    return 1;
}
