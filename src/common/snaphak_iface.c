/* snaphak_iface.c -- the BACKEND-hosted interface-object factory + the live REGISTER/UNREGISTER/DRAIN
 * bodies (the generic, engine-free trio). See snaphak_iface.h for the pinned ABI.
 *
 * The OG builds this object in XINPUT1_3 FUN_1800229b1 (operator_new(0x60) + the 0x78 sub-object + the
 * RB-tree map + the work-queue vector). We build the SAME shape with a portable C implementation:
 * the cmd-map is a small open hash/linear list keyed by name (the frontend's 20 subcommands fit easily),
 * the work-queue is a growable sh_work_item vector, and the mutex is a Win32 CRITICAL_SECTION stored in
 * the obj+0x08 blob. The vtable is a single static instance shared by all (only one object is ever made).
 *
 * Clean-room: our own RE; zero OG bytes. Compiled into the backend (XINPUT1_3.dll); the frontend only
 * CALLS through the vtable (it never constructs the object).
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "snaphak_iface.h"

/* The cmd-map: a simple name->{handler,ctx} list. OG uses a std::map RB-tree at sub+0x00; for the clone
 * the lookup is by name and the set is small (20), so a linear/grown array is faithful in behavior (same
 * register/lookup/unregister semantics) without re-implementing a std::map ABI across the DLL line. */
typedef struct cmd_entry {
    char           *name;           /* heap-owned */
    sh_cmd_handler  handler;
    void           *ctx;
} cmd_entry;

typedef struct cmd_map {
    cmd_entry *items;
    size_t     count;
    size_t     cap;
} cmd_map;

/* The sub-object as the backend actually allocates it: the pinned sh_iface_sub header + the live C
 * containers we back the map/work-queue with. The pinned wq_begin/end/cap mirror the vector; the cmd_map
 * lives where the OG RB-tree head would (sub+0x00..) -- we stash a cmd_map* in map_root and keep map_nil
 * as a sentinel so the layout offsets stay put. */
typedef struct sub_impl {
    sh_iface_sub  pinned;           /* the ABI-pinned header (must be first: offsets are load-bearing) */
    cmd_map       map;              /* the live cmd-map backing store */
    CRITICAL_SECTION lock;          /* guards both the map and the work-queue */
} sub_impl;

/* --------------------------------------------------------------------- the mutex in obj+0x08 ---------
 * OG: _Mtx_init_in_situ writes a CRT _Mtx into obj+8. We store a CRITICAL_SECTION in the same blob (the
 * blob is 0x50 bytes; CRITICAL_SECTION is 0x28 on x64 -- fits). The frontend never touches it; only the
 * backend's drain/register lock through it (and through the sub's lock). We use the sub's CRITICAL_SECTION
 * as the single guard (the OG's two mutexes guard the same logical queue); the obj+0x08 blob is reserved
 * for ABI-layout exactness and left zero. */

/* --------------------------------------------------------------------- REGISTER (+0x188) ------------ */
static void iface_register_cmd(sh_iface *self, const char *name, sh_cmd_handler handler, void *ctx)
{
    if (!self || !self->sub || !name) return;
    sub_impl *si = (sub_impl *)self->sub;
    EnterCriticalSection(&si->lock);
    /* replace if the name already exists (OG map insert overwrites the slot) */
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            si->map.items[i].handler = handler;
            si->map.items[i].ctx     = ctx;
            LeaveCriticalSection(&si->lock);
            return;
        }
    }
    if (si->map.count == si->map.cap) {
        size_t ncap = si->map.cap ? si->map.cap * 2 : 32;
        cmd_entry *ni = (cmd_entry *)realloc(si->map.items, ncap * sizeof(cmd_entry));
        if (!ni) { LeaveCriticalSection(&si->lock); return; }
        si->map.items = ni;
        si->map.cap   = ncap;
    }
    size_t namelen = strlen(name) + 1;
    char  *namecpy = (char *)malloc(namelen);
    if (!namecpy) { LeaveCriticalSection(&si->lock); return; }
    memcpy(namecpy, name, namelen);
    si->map.items[si->map.count].name    = namecpy;
    si->map.items[si->map.count].handler = handler;
    si->map.items[si->map.count].ctx     = ctx;
    si->map.count++;
    LeaveCriticalSection(&si->lock);
}

/* --------------------------------------------------------------------- UNREGISTER (+0x190) ---------- */
static void iface_unregister_cmd(sh_iface *self, const char *name)
{
    if (!self || !self->sub || !name) return;
    sub_impl *si = (sub_impl *)self->sub;
    EnterCriticalSection(&si->lock);
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            free(si->map.items[i].name);
            si->map.items[i] = si->map.items[si->map.count - 1];
            si->map.count--;
            break;
        }
    }
    LeaveCriticalSection(&si->lock);
}

/* --------------------------------------------------------------------- DRAIN (+0x1a0) ---------------
 * Run every queued {handler, args} on the CURRENT thread (the UI/main thread, since the think-loop calls
 * this per-frame), then reset the queue. OG: takes the mutex, runs [wq_begin,wq_end), clears the vector.
 * Each item owns its argv copy -- freed after the handler runs. SEH would be ideal but this file is plain
 * C without the engine's fault surface; a handler fault here would already be the SnapStack op's concern
 * (op execution wraps it). There are no producers yet, so the queue is always empty -- this just
 * proves the drain is wired + callable from the frontend's think-loop. */
static void iface_drain_work_queue(sh_iface *self)
{
    if (!self || !self->sub) return;
    sub_impl *si = (sub_impl *)self->sub;
    sh_iface_sub *sub = &si->pinned;

    EnterCriticalSection(&si->lock);
    sh_work_item *begin = sub->wq_begin;
    sh_work_item *end   = sub->wq_end;
    /* detach the queue so handlers can re-enqueue without reentrancy on our iteration */
    sub->wq_begin = NULL;
    sub->wq_end   = NULL;
    sub->wq_cap   = NULL;
    LeaveCriticalSection(&si->lock);

    for (sh_work_item *it = begin; it != end; it++) {
        if (it->handler) it->handler(it->ctx, it->argc, (const char **)it->argv);
        /* free the per-item argv copy (each string + the array) */
        if (it->argv) {
            for (int i = 0; i < it->argc; i++) free(it->argv[i]);
            free(it->argv);
        }
    }
    free(begin);   /* the detached vector's storage */
}

/* --------------------------------------------------------------------- cmd-map LOOKUP (C2, +0x58) --
 * The `sh` dispatcher (XINPUT 0x7620) looks the subcommand up in the runtime cmd-map. OG does an RB-tree
 * find on the std::map at obj+0x58; our backing store is the same linear map iface_register_cmd fills.
 * Returns 1 + the {handler,ctx} on a hit; 0 on a miss. Taken under the cmd-map lock for consistency with
 * a concurrent register (the registrar runs on the UI thread, the dispatcher on the console thread). */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx)
{
    if (!self || !self->sub || !name) return 0;
    sub_impl *si = (sub_impl *)self->sub;
    int found = 0;
    EnterCriticalSection(&si->lock);
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            if (handler) *handler = si->map.items[i].handler;
            if (ctx)     *ctx     = si->map.items[i].ctx;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&si->lock);
    return found;
}

/* --------------------------------------------------------------------- work-queue ENQUEUE -----
 * The producer the OG `sh` dispatcher is: append {handler,ctx,argc, deep-copied argv} onto the work-queue
 * vector (sub+0x60/+0x68/+0x70) under the mutex. The DRAIN (+0x1a0) runs + frees it on the UI thread. We
 * grow the vector geometrically (the OG std::vector push_back); the argv strings are heap-duplicated so
 * the caller (the engine cmd handler, whose argv is engine-owned + transient) need not keep them alive. */
int sh_iface_enqueue_work(sh_iface *self, sh_cmd_handler handler, void *ctx,
                          int argc, const char **argv)
{
    if (!self || !self->sub || !handler) return 0;
    sub_impl *si = (sub_impl *)self->sub;
    sh_iface_sub *sub = &si->pinned;

    /* deep-copy argv OUTSIDE the lock (malloc/strdup are the slow part). */
    char **argv_copy = NULL;
    if (argc > 0 && argv) {
        argv_copy = (char **)calloc((size_t)argc, sizeof(char *));
        if (!argv_copy) return 0;
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i] ? argv[i] : "";
            size_t n = strlen(s) + 1;
            char *c = (char *)malloc(n);
            if (!c) { for (int j = 0; j < i; j++) free(argv_copy[j]); free(argv_copy); return 0; }
            memcpy(c, s, n);
            argv_copy[i] = c;
        }
    } else {
        argc = 0;
    }

    EnterCriticalSection(&si->lock);
    /* grow the vector if full (geometric, like std::vector). begin/end/cap are sh_work_item*. */
    size_t cur_len = (size_t)(sub->wq_end - sub->wq_begin);
    size_t cur_cap = (size_t)(sub->wq_cap - sub->wq_begin);
    if (cur_len == cur_cap) {
        size_t ncap = cur_cap ? cur_cap * 2 : 8;
        sh_work_item *nb = (sh_work_item *)realloc(sub->wq_begin, ncap * sizeof(sh_work_item));
        if (!nb) {
            LeaveCriticalSection(&si->lock);
            if (argv_copy) { for (int i = 0; i < argc; i++) free(argv_copy[i]); free(argv_copy); }
            return 0;
        }
        sub->wq_begin = nb;
        sub->wq_end   = nb + cur_len;
        sub->wq_cap   = nb + ncap;
    }
    sub->wq_end->handler = handler;
    sub->wq_end->ctx     = ctx;
    sub->wq_end->argc    = argc;
    sub->wq_end->argv    = argv_copy;
    sub->wq_end++;
    LeaveCriticalSection(&si->lock);
    return 1;
}

/* --------------------------------------------------------------------- the shared vtable ------------
 * One static instance. The live trio carry real bodies; every other slot is NULL (a pin-and-stub
 * placeholder). The frontend only invokes +0x1a0 (drain) in the think-loop and +0x88/+0x1c0/+0x1d0/
 * +0x1b0/+0x98 in the EntityMode key-poll branch -- which is GATED behind a debug flag + an editor-ready
 * check, so it does not fire in normal boot. The remaining slots are filled later. A defensive frontend treats a NULL
 * slot as "not yet implemented" (it null-checks before calling the optional ones).
 *
 * A C99 designated initializer binds the live bodies to their named members; ALL other slots start
 * NULL. The engine-touch slots (selection/toast/class-read) are bound at install via
 * sh_iface_bind_engine_slots -- so the vtable is no longer const (the backend patches the engine slots
 * once the editor singleton + the AddToSelection/ClearSelection/Toast engine fns are signature-resolved).
 * The offsets stay pinned by the struct layout regardless. */
static sh_iface_vtbl g_iface_vtbl_live = {
    .register_cmd     = iface_register_cmd,     /* +0x188 */
    .unregister_cmd   = iface_unregister_cmd,   /* +0x190 */
    .drain_work_queue = iface_drain_work_queue, /* +0x1a0 */
    /* engine-touch + apply/serialize slots NULL until sh_iface_bind_engine_slots / the heavy bind */
};

/* --------------------------------------------------------------------- engine-slot binder ------
 * The backend resolves the editor singleton + the selection/toast engine fns by SIGNATURE, then hands the
 * bodies here once. We patch the SINGLE shared vtable's engine-touch slots in place (only one interface
 * object is ever made, so all instances see the bind). Idempotent + null-tolerant: a NULL body leaves the
 * slot NULL (the frontend null-checks before calling). Keeps the common factory engine-FREE -- the engine
 * code lives entirely in the backend (iface_engine.c); this just wires the function pointers. */
void sh_iface_bind_engine_slots(const sh_iface_engine_slots *s)
{
    if (!s) return;
    g_iface_vtbl_live.set_editor_vec3   = s->set_editor_vec3;    /* +0x00  (camera) */
    g_iface_vtbl_live.get_editor_vec3   = s->get_editor_vec3;    /* +0x08  (camera) */
    g_iface_vtbl_live.entity_count      = s->entity_count;       /* +0x10  */
    g_iface_vtbl_live.id_to_string      = s->id_to_string;       /* +0x18  */
    g_iface_vtbl_live.is_valid_id       = s->is_valid_id;        /* +0x28  */
    g_iface_vtbl_live.editor_ready_poll = s->editor_ready_poll;  /* +0x88  (window gate) */
    g_iface_vtbl_live.get_classname_copy= s->get_classname_copy; /* +0x48  */
    g_iface_vtbl_live.get_inherit_copy  = s->get_inherit_copy;   /* +0x50  */
    g_iface_vtbl_live.add_to_selection  = s->add_to_selection;   /* +0x138 */
    g_iface_vtbl_live.clear_selection   = s->clear_selection;    /* +0x148 */
    g_iface_vtbl_live.get_selection     = s->get_selection;      /* +0x150 */
    g_iface_vtbl_live.hovered_id        = s->hovered_id;         /* +0x198 */
    g_iface_vtbl_live.is_entity_mode    = s->is_entity_mode;     /* +0x1c0 (Create-New-Timeline gate) */
    g_iface_vtbl_live.toast             = s->toast;              /* +0x1b8 */
    /* heavy slots (NULL until sh_iface_engine binds them with the full apply chain). */
    g_iface_vtbl_live.serialize_entity  = s->serialize_entity;   /* +0xc8 */
    g_iface_vtbl_live.apply_edit        = s->apply_edit;         /* +0xd0 */
    g_iface_vtbl_live.read_prefab       = s->read_prefab;        /* +0xb8 */
    /* DATA-tab slots (Entity-State read/write + Prefabs serialize/path + Delete). */
    g_iface_vtbl_live.get_declsource_copy    = s->get_declsource_copy;    /* +0x30  */
    g_iface_vtbl_live.rebuild_set_declsource = s->rebuild_set_declsource; /* +0x40  */
    g_iface_vtbl_live.get_displayname        = s->get_displayname;        /* +0x58  */
    g_iface_vtbl_live.set_classname          = s->set_classname;          /* +0x78  */
    g_iface_vtbl_live.set_inherit            = s->set_inherit;            /* +0x80  */
    g_iface_vtbl_live.set_entity_0x170       = s->set_displayname;        /* +0x128 */
    g_iface_vtbl_live.serialize_selection    = s->serialize_selection;    /* +0xb0  */
    g_iface_vtbl_live.resolve_prefab_path    = s->resolve_prefab_path;    /* +0xc0  */
    g_iface_vtbl_live.selection_guard        = s->remove_from_selection;  /* +0x130 */
    /* the Timeline-Editor constrained decl-combobox enumerator (+0x110). */
    g_iface_vtbl_live.enum_decls_of_resclass = s->enum_decls_of_resclass; /* +0x110 */
    /* clone-extension slots (the atomic class+inherit morph). */
    g_iface_vtbl_live.apply_class_inherit    = s->apply_class_inherit;    /* +0x268 */
    /* clone-extension (the class-dropdown enumerator). */
    g_iface_vtbl_live.enum_valid_classes     = s->enum_valid_classes;     /* +0x270 */
    /* clone-extension (the inherit-dropdown enumerator). */
    g_iface_vtbl_live.enum_inherits          = s->enum_inherits;          /* +0x278 */
    /* clone-extension (the dev-layer entity-hidden query). */
    g_iface_vtbl_live.id_dev_layer_hidden    = s->id_dev_layer_hidden;    /* +0x280 */
    /* clone-extension (the wire-any connect-edit generation counter; entity-list re-read signal). */
    g_iface_vtbl_live.wire_edit_generation   = s->wire_edit_generation;   /* +0x288 */
    /* clone-extension (the synchronous inline apply -- OG-faithful commit for the SnapStack decl-edit ops). */
    g_iface_vtbl_live.apply_sync             = s->apply_sync;             /* +0x290 */
}

/* --------------------------------------------------------------------- the factory -----------------
 * operator_new(0x60) shape: zero-init the object, install the vtable, init the obj-mutex blob (left zero
 * for layout; the live guard is the sub's CRITICAL_SECTION), allocate + zero the sub-object, init its
 * map/queue empty. Returns the object the backend stores + hands to the frontend. */
sh_iface *sh_iface_create(void)
{
    sh_iface *obj = (sh_iface *)calloc(1, sizeof(sh_iface));
    if (!obj) return NULL;

    sub_impl *si = (sub_impl *)calloc(1, sizeof(sub_impl));
    if (!si) { free(obj); return NULL; }
    InitializeCriticalSection(&si->lock);
    /* empty map + empty work-queue (pinned vector NULL/NULL/NULL = empty) */
    si->map.items   = NULL;
    si->map.count   = 0;
    si->map.cap     = 0;
    si->pinned.wq_begin = NULL;
    si->pinned.wq_end   = NULL;
    si->pinned.wq_cap   = NULL;
    /* map_nil/map_root/map_size carry the cmd_map backing pointer + count for ABI-shape parity (OG
     * stashes the RB-tree head here); the live store is si->map, reached via the sub_impl cast. */
    si->pinned.map_nil  = &si->map;
    si->pinned.map_root = NULL;
    si->pinned.map_size = 0;

    obj->vtbl = &g_iface_vtbl_live;     /* +0x00 */
    /* obj->mtx left zero (ABI-layout reserve; the live guard is si->lock at the sub) */
    obj->sub  = (sh_iface_sub *)si;     /* +0x58 -- the sub_impl's first member IS the pinned header */

    return obj;
}
