/* apply_engine.c -- see apply_engine.h. The BACKEND 8-pass full-entity APPLY CHAIN.
 *
 * Native, instruction-faithful port of the reference implementation (the proven mechanism):
 *   serializeEntityToJson  -> slot_serialize_entity   (+0xc8): clone live->temp -> reflection-serialize
 *                             struct->tree -> render tree->JSON text.
 *   deserializeTextToObject -> ae_deserialize_to_obj  (the FUN_180004950 lex+structDeserialize chain,
 *                             FIX A teardown), reused by both the bss apply (typeName "idSnapEntity",
 *                             dst = a temp def) and the mkcmd paste (typeName "idSnapEntityPrefab",
 *                             dst = editor+0x209a8).
 *   doApplyBssOne tail      -> ae_apply_one           (deserialize patched text -> temp def -> commit the
 *                             normalized source/class/inherit onto the live defsub -> dtor temp).
 *   doMkcmdApplyNow         -> ae_mkcmd_one           (deserialize prefab text -> editor+0x209a8).
 *   ensureBssCommand + doBssApplyNow -> the clone_bss_apply engine command + its drain handler (FIX B):
 *                             the heavy structured-deserialize AVs mid-frame (a stale reflection-handler),
 *                             so the frontend SCHEDULEs a batch (slot_schedule_apply -> BufferCommandText)
 *                             and the engine drains clone_bss_apply on the DOOM main thread (decl-safe).
 *   readPrefabStagingJson   -> slot_read_prefab       (+0xb8): the INVERSE +0xb0 serialize of editor+0x209a8.
 *
 * Every engine fn is signature-resolved (version-portable); the declMgr accessor is the ONE hardcoded RVA,
 * reused from sh_typeinfo (sh_typeinfo_get_declmgr). Every struct is allocated at its REAL size + engine-
 * ctor'd (an allocation-size freeze lesson); the teardown order mirrors FUN_180004950 (FIX A) and deliberately omits the
 * OG raw lexer-buffer free 0x1ab32e0 (corruption-cookie-guarded -> fatal on our SSO lexer). Every engine
 * deref / call is SEH-guarded -> a wrong/shifted-build offset or a partial bind degrades to a clean failure
 * (toast "0 applied"), never a crash.
 *
 * Clean-room: ported from our own RE + the reference implementation. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snaphak_iface.h"
#include "apply_engine.h"
#include "typeinfo.h"     /* sh_typeinfo_get_declmgr -- the one shared declMgr accessor */
#include "ui_bridge.h"    /* sh_ui_get_iface -- reach the toast slot for the apply-result toast */
#include "iface_engine.h" /* sh_iface_class_inherit_ok -- the LAYER-C class/inherit prevention guard */
#include "signatures.h"
#include "backend_log.h"

/* ============================================================ editor-struct field offsets ========== */
/* SAME this-live-build offsets iface_engine.c uses (ported from the reference implementation, SEH-guarded). The editor
 * singleton is a hardcoded data RVA (like cmdSystem/cvarSystem). The entity ARRAY + defsub reach is the
 * SAME the apply (FUN_180004b80) + serialize (FUN_1800044a0) resolve. */
#define EDITOR_SINGLETON_RVA   0x3056748u   /* module_base + this = the inline idSnapEditorLocal object */
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid; ALSO the clone base (ent+8) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object (commit target) */
/* (render-node bucket re-sync defines removed with the create-timeline path -- the clone no longer edits the
 * editor's wire-overlay render buckets; a timeline is placed by the engine via the in-game palette.) */
#define DEFSUB_CLASS_OFF       0x60         /* defsub+0x60 -> classname idStr (commit dst) */
#define DEFSUB_INHERIT_OFF     0x58         /* defsub+0x58 -> inherit idStr (commit dst) */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

/* mkcmd / prefab paste-staging slot: editor+0x209a8 (the in-game Ctrl+V target). LIVE-CONFIRMED 2026-06-25,
 * 3x DIRECT (paste-slot re-derive, verified vs the Copy-handler 0xCE3960 decompile): the
 * idSnapEditorLocal ctor 0x51A8E0 constructs an idSnapEntityPrefab at this[0x4135] (=0x209a8); the Copy handler
 * 0xCE3960 writes it via FUN_14054e410(editor+0x209a8, editor, &err); the Paste/Ctrl+V 0xCE1810 instantiates it
 * via FUN_14054f950(editor+0x209a8, editor). NOT stale -- open-problems A2's "stale 2021 offset" was OVERTURNED.
 * It sits between the live-verified +0x204d0 (selection) and +0x21088 (screen). RE-DERIVE per build: the
 * "idSnapEntityPrefab" registrar 0x5A9090 -> the populate fn 0x54e410 -> its sole caller (Copy 0xCE3960) -> read
 * the editor+OFF it passes. NB: the mkcmd "Error: system error" is NOT this offset -- and NOT the deserialize
 * design (OG FUN_1800094f0 also deserializes DIRECT-into-live editor+0x209a8; the clone's bss chain uses the same
 * chain + works 66/66 -- the "deserialize-into-temp" hypothesis was REFUTED 2026-06-25). UNRESOLVED; needs a live
 * repro (malformed prefab text / stale slot / read-back gate). The timeline-SPAWN auto-place needs the INSTANTIATE
 * FUN_14054f950(editor+0x209a8, editor) after staging (0x54F950, the engine Ctrl+V; not yet wired). */
#define PASTE_STAGING_OFF      0x209a8


/* the prefab-from-selection serialize (+0xb0) engine fns. RE-DERIVE off the OG XINPUT1_3
 * FUN_180004210 (the serialize-SELECTION body): a temp prefab is ctor'd via `(DAT_18003e120 + 0x54d0a0)`
 * then populated from the editor via `(DAT_18003e120 + 0x54e410)(temp, editor)`; on success it is reflection-
 * serialized as "idSnapEntityPrefab" + tree-rendered to JSON. The two prefab fns are jumptable/inline-prone
 * leaves the byte-sig scanner cannot reliably anchor, so they resolve by FALLBACK RVA off g_doom_base
 * (tagged for per-build re-derive, exactly like the editor singleton).
 *
 * PREFAB_TEMP_SIZE was previously 0x220, based on the OG local_6d8 frame slot (0x210 bytes) -- CONFIRMED
 * (2026-07-06) far too small: the ctor at +0x54d0a0 writes its own fields up to ~+0x118 then makes a
 * small forward call into a SECOND, larger ctor that keeps writing fields past +0x590 -- the real object
 * needs at least ~0x590+ bytes, ~2.6x the old allocation. The old size was a silent stack-buffer overflow
 * on every create-from-selection call (writes landing on valid stack memory just past our buffer -- not a
 * clean AV, so neither the fault-shield VEH nor our own SEH guard ever caught it; this is what crashed
 * DOOM outright). Bumped to 0x2000 for comfortable headroom over the confirmed-required size.
 *
 * PrefabPopulate is a 3-ARG function, not 2 -- CONFIRMED (2026-07-06). The 3rd (R8) is an out int* status/
 * reason code the engine writes through (seen storing 1, 2, and a cleared 0 on different validation paths).
 * Our call only ever passed 2 args, so R8 held whatever was left over from the prior call in the sequence
 * -- an intermittent crash (write through garbage/unmapped R8, e.g. observed 0x10) at two sites inside
 * PrefabPopulate: +0x2D7 and +0xE91. Fixed by adding the missing out-param and passing a real local's
 * address so the write always lands somewhere harmless.
 *
 * Separately CONFIRMED: not hovering an entity in the selection is a REAL engine requirement, not a red
 * herring -- with the crash fixed, status code 2 turns out to mean exactly that (the engine prints "Failed
 * to create prefab: not hovering entity in selection." itself before returning it). So the create flow now
 * checks the hovered-id slot (+0x198) up front (see poc_apply_create_prefab in snaphak_ui_webview.cpp)
 * instead of relying on this out-param at all -- simpler, and gives the UI an accurate "not hovering"
 * result instead of the generic "nothing selected". */
#define PREFAB_CTOR_RVA        0x54d0a0u   /* idSnapEntityPrefab ctor (OG FUN_180004210 local_6d8 ctor) */
#define PREFAB_POPULATE_RVA    0x54e410u   /* populate prefab from editor selection (returns char success) */
#define PREFAB_DTOR_RVA        0x51d870u   /* idSnapEntityPrefab dtor (OG FUN_180004210 cleanup) */
#define PREFAB_TEMP_SIZE       0x2000      /* was 0x220 -- confirmed too small, see comment above */
#define PASTE_INSTANTIATE_RVA  0x54f950u   /* PasteInstantiate FUN_14054f950: void(prefab=editor+0x209a8, editor)
                                            * -- instantiate the staged prefab into the live map + AddToSelection,
                                            * camera-relative grab placement; does NOT consume the slot. NOT called
                                            * by us at all (CONFIRMED 2026-07-06 against the ORIGINAL snaphakui.dll
                                            * + XINPUT1_3.dll: neither ever calls it -- only the engine's own native
                                            * Ctrl+V handler does, after a Copy or a prefab double-click just stages
                                            * the text via the SAME deserialize-into-editor+0x209a8 step our own
                                            * ae_mkcmd_one already does). Calling it ourselves skipped whatever
                                            * grab-tool state the native handler also sets up alongside it (left a
                                            * placed prefab undraggable + crashed on the next Play/Editor
                                            * transition); a follow-up attempt at synthesizing a native Ctrl+V from
                                            * our own code hit its own side effects (see backend-changes.md). Load
                                            * from the Prefabs tab now just stages (kind=1) and prompts the user to
                                            * paste manually, matching the ORIGINAL's own actual workflow. Kept here
                                            * for reference RVA only -- not wired to anything. */
#define ENT_DESHARE_RVA        0x52c920u   /* FUN_14052c920(&array[id]) -- COW make-unique: de-share an entity's 0x6f8
                                            * block before an in-place edit (the engine's UNIVERSAL edit discipline).
                                            * RE-DERIVE per build: if *(int*)*slot != 1 -> operator_new(0x6f8) + deep-copy
                                            * body (FUN_14053d800) + store into slot; returns *slot+8 (the private body). */

/* ============================================================ apply-chain struct sizes ============== */
/* DIRECT from the XINPUT1_3 FUN_180004b80 / FUN_180004950 / FUN_1800044a0 decompiles + the engine ctors
 * (the reference implementation LEXER_SIZE/PARSE_NODE_SIZE/TEMP_DEF_SIZE + the history notes). */
#define LEXER_SIZE             0xC0         /* sizeof(idLexer) -- ctor touches to +0xB8 (0x40 was the freeze) */
#define LEXER_IDSTR0_OFF       0x30         /* idLexer embedded idStr #1 (ctor FUN_1419fd040 @ self+0x30) */
#define LEXER_IDSTR1_OFF       0x88         /* idLexer embedded idStr #2 (ctor FUN_1419fd040 @ self+0x88) */
#define PARSE_NODE_SIZE        0x28         /* parse-node alloc (object 0x18; frame slot spacing 0x28) */
#define TEMP_DEF_SIZE          0x1B0        /* sizeof(temp idSnapEntity) -- ctor touches to +0x1AE */
#define IDSTR_SIZE             0x30         /* sizeof(idStr) -- the SSO size */
#define IDSTR_LEN_OFF          0x8          /* idStr: int len @ +0x8 */
#define IDSTR_DATA_OFF         0x10         /* idStr: char* data @ +0x10 (heap when len>=0x10 else inline SSO) */
#define TDEF_INHERIT_OFF       0x58         /* temp-def normalized inherit idStr (tmp+0x58) */
#define TDEF_CLASS_OFF         0x60         /* temp-def normalized classname idStr (tmp+0x60) */
#define TDEF_SOURCE_OFF        0x140        /* temp-def normalized decl-source idStr data-ptr (tmp+0x140) */
#define VSLOT_REFLECT_ACCESSOR 0x80         /* declMgr vtable +0x80 -> reflection mgr (the reference implementation + sh_typeinfo) */
#define SER_TREE_KIND          7            /* the out parse-tree is built with parse-node kind 7 */
#define SER_TAG_KIND           7            /* the {tag} arg5 node is also kind 7 */

/* command-buffer routing (FIX B). */
#define CLONE_BSS_CMD          "clone_bss_apply"
#define APPLY_TEXT_CAP         (4 * 1024 * 1024) /* max JSON we accept per item (sanity). Was 256 KB --
                                            * CONFIRMED (2026-07-06) too small: real prefab files can run
                                            * well past that (see poc_apply_create_prefab's own 4 MB
                                            * scratch buffer comment), and slot_schedule_apply's deep-copy
                                            * silently truncated anything over this cap with no error at
                                            * all -- Load/Place staging a large prefab got cut mid-JSON,
                                            * which then failed to lex ("applied 0/1", no crash, no
                                            * fault-shield entry -- just silently wrong). Bumped to match
                                            * the 4 MB scratch buffer already used elsewhere for prefab
                                            * content; live-tested up to 2 MB without issue. */
#define APPLY_MAX_ITEMS        4096         /* sanity cap on a scheduled batch */

/* ============================================================ fine-grained serialize DIAGNOSTIC ====
 * Per-sub-step backend_log inside the serialize path so a live test pinpoints EXACTLY where the empty
 * comes from (reflect NULL / clone defsub null / StructSerialize ret=0 / rendered len 0). Always-on for
 * now gated off (set AE_SER_DIAG_ON to 1 to debug). NOTE (resolved 2026-06-22): the empty-output bug was
 * UNINITIALIZED stack buffers (the engine ctors/clone got trailing garbage) -- FIXED by the memsets in
 * ae_serialize_to_json. It is NOT a thread bug: this diagnostic showed StructSerialize ret=1 + a 1091-byte
 * JSON on the UI thread (tid 39404), so the serialize works fine off-main. */
#define AE_SER_DIAG_ON  0   /* per-serialize trace (tid/clone/StructSerialize/idStr); flip to 1 to debug */
#if AE_SER_DIAG_ON
#define AE_SER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_SER_DIAG(...) do { } while (0)
#endif

/* same idea, deserialize side -- Load/Place (2026-07-06) showed certain larger/more complex real prefab
 * files silently fail to stage (backend log: "applied 0/1") while simple ones succeed, with no
 * fault-shield entry at all. RESOLVED (2026-07-06): this diagnostic pinpointed it immediately -- "text
 * len=262143" (one byte under the old 256 KB APPLY_TEXT_CAP), i.e. slot_schedule_apply's deep-copy was
 * silently truncating anything over that cap before ae_deserialize_to_obj ever saw it, so the Lexer
 * failed on the cut-off JSON. Fixed by raising APPLY_TEXT_CAP to 4 MB; retested against the prefabs that
 * were failing (all "applied 1/1" now) and live-tested up to 2 MB with no issue. Gated back off; flip to
 * 1 again if a similar silent-failure shows up. */
#define AE_DESER_DIAG_ON  1
#if AE_DESER_DIAG_ON
#define AE_DESER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_DESER_DIAG(...) do { } while (0)
#endif

/* apply-commit diagnostic (2026-07-10, save->reload heap-corruption hunt): log the ACTUAL srcPtr/clsPtr/inhPtr
 * pointers + strlens + the decl-source head that ae_apply_one hands to DeclSourceRebuild/IdStrAssign, and
 * confirm each engine call returns -- ground truth on where a pointer/length goes bad, instead of guessing.
 * Flip to 0 to silence. */
#define AE_APPLY_DIAG_ON  1
#if AE_APPLY_DIAG_ON
#define AE_APPLY_DIAG(...) do { char _la[512]; \
    _snprintf_s(_la, sizeof _la, _TRUNCATE, __VA_ARGS__); backend_log(_la); } while (0)
#else
#define AE_APPLY_DIAG(...) do { } while (0)
#endif

/* ============================================================ engine fn typedefs (sig-resolved) ===== */
typedef void  (*entity_clone_fn)(void *cloneBase, void *tmpDef, int one);            /* EntityClone 0x5a6460 */
typedef void  (*entity_def_ctor_fn)(void *self);                                     /* EntityDefCtor 0x5e9400 */
typedef void  (*entity_def_dtor_fn)(void *self);                                     /* EntityDefDtor 0x17ace70 */
typedef char  (*struct_serialize_fn)(void *reflect, const char *typeName, void *srcObj,
                                     void *outTree, void *arg5, void *flags);         /* StructSerialize 0x1a21b40 */
typedef char  (*struct_deser_fn)(void *reflect, const char *typeName, void *dstObj,
                                 void *parseNode, void *arg5, void *flags);           /* StructDeserialize 0x1a1d450 */
typedef void  (*tree_render_fn)(void *outTree, void *outIdStr);                       /* TreeRenderJson 0x1a43730 */
typedef char  (*lexer_fn)(void *lexer, void *srcIdStr, void *parseNode, int one);     /* Lexer 0x1a5cd90 */
typedef void  (*lexctx_ctor_fn)(void *lexer);                                         /* LexCtxCtor 0x1a5bb70 */
typedef void  (*parse_node_ctor_fn)(void *node, int kind);                            /* ParseNodeCtor 0x1a41400 */
typedef void  (*parse_node_dtor_fn)(void *node);                                      /* ParseNodeDtor 0x1a41640 */
typedef void *(*idstr_ctor_fn)(void *self, const char *cstr);                         /* IdStrCtor 0x19fcef0 */
typedef void  (*idstr_dtor_fn)(void *self);                                           /* IdStrDtor 0x19fd120 */
typedef void  (*idstr_assign_fn)(void *dstField, const char *cstr);                   /* IdStrAssign 0x1a03e10 */
typedef void  (*decl_src_rebuild_fn)(void *defsub, const char *srcText, int rebuild); /* DeclSourceRebuild 0x17ae560 */
typedef void *(*ent_deshare_fn)(void *slot);                                          /* COW make-unique 0x52c920 -- de-share an entity's 0x6f8 block before an in-place edit */
typedef void  (*buffer_cmd_fn)(void *cmdSys, const char *text);                       /* BufferCommandText 0x1aa3780 */
typedef void  (*add_command_fn)(void *cmdSys, const char *name, void *cb, void *p3,
                                const char *help, unsigned int flags);                /* AddCommand 0x1aa3630 */
typedef void  (*prefab_ctor_fn)(void *self);                                          /* PrefabCtor 0x54d0a0 */
typedef char  (*prefab_populate_fn)(void *self, void *editor, int *outStatus);        /* PrefabPopulate 0x54e410 */
typedef void  (*prefab_dtor_fn)(void *self);                                          /* PrefabDtor 0x51d870 */

/* ============================================================ module state (resolved once) ========== */
static const uint8_t      *g_doom_base   = NULL;
static const uint8_t      *g_editor      = NULL;   /* module_base + 0x3056748 (inline editor object) */
static void               *g_cmdsys      = NULL;   /* idCmdSystemLocal* (for BufferCommandText/AddCommand) */
static entity_clone_fn     g_entity_clone = NULL;
static entity_def_ctor_fn  g_def_ctor    = NULL;
static entity_def_dtor_fn  g_def_dtor    = NULL;
static struct_serialize_fn g_ser         = NULL;
static struct_deser_fn     g_deser       = NULL;
static tree_render_fn      g_render       = NULL;
static lexer_fn            g_lexer       = NULL;
static lexctx_ctor_fn      g_lex_ctor    = NULL;
static parse_node_ctor_fn  g_node_ctor   = NULL;
static parse_node_dtor_fn  g_node_dtor   = NULL;
static idstr_ctor_fn       g_idstr_ctor  = NULL;
static idstr_dtor_fn       g_idstr_dtor  = NULL;
static idstr_assign_fn     g_idstr_assign= NULL;
static decl_src_rebuild_fn g_decl_rebuild= NULL;
static ent_deshare_fn      g_deshare     = NULL;   /* COW make-unique before an in-place entity edit (engine's universal discipline) */
static buffer_cmd_fn       g_buffer_cmd  = NULL;
static add_command_fn      g_add_command = NULL;
static prefab_ctor_fn      g_prefab_ctor     = NULL;   /* +0xb0 serialize-selection */
static prefab_populate_fn  g_prefab_populate = NULL;
static prefab_dtor_fn      g_prefab_dtor     = NULL;
static volatile LONG       g_installed   = 0;
static volatile LONG       g_cmd_registered = 0;   /* clone_bss_apply registered once (lazy) */

/* ============================================================ the pending-apply store =============== */
/* The frontend SCHEDULEs a batch; the clone_bss_apply handler consumes it on the DOOM main thread. One
 * batch pending at a time (matching the reference implementation: one apply per enqueue). Guarded so the
 * frontend (UI thread) writer + the engine (main thread) drainer don't tear. */
typedef struct apply_item_copy {
    int   kind;     /* 0 = bss-style deserialize+commit on `id`; 1 = mkcmd prefab paste (stage-only, OG-faithful);
                     * 2 = mkcmd prefab paste + INSTANTIATE (create-from-scratch SPAWN: stage then place) */
    int   id;
    char *text;     /* heap-owned copy of the patched JSON */
} apply_item_copy;

static CRITICAL_SECTION  g_pending_lock;
static int               g_pending_lock_init = 0;
static apply_item_copy  *g_pending_items = NULL;
static int               g_pending_count = 0;
static char              g_pending_op[32] = {0};

/* ============================================================ SEH-guarded primitive reads =========== */
static int ae_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ae_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* int-typed sibling for the diagnostic's idStr-len read (idStr len@+8 is a signed int). */
static int ae_read_u32_safe(const void *src, int *out)
{
    __try { *out = *(const int *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return 0; }
}

/* "editor up" guard, matching the reference implementation editorSession: the loaded-map ptr (+0x204c8) is non-null in-editor. */
static const uint8_t *ae_editor_session(void)
{
    if (!g_editor) return NULL;
    void *mapObj = NULL;
    if (!ae_read_ptr(g_editor + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return g_editor;
}

/* the entity-ptr array {array, count} off the loaded-map object; 0 on no map / fault. */
static int ae_entity_array(void **out_array, uint32_t *out_count)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *arrObj = NULL;
    if (!ae_read_ptr(ed + ED_MAP_OBJ_OFF, &arrObj) || arrObj == NULL) return 0;
    void    *array = NULL;
    uint32_t count = 0;
    if (!ae_read_ptr((const uint8_t *)arrObj + ARR_ENT_ARRAY_OFF, &array)) return 0;
    if (!ae_read_u32((const uint8_t *)arrObj + ARR_ENT_COUNT_OFF, &count)) return 0;
    if (array == NULL || count > ENT_COUNT_CAP) return 0;
    *out_array = array;
    *out_count = count;
    return 1;
}

/* one entity ptr by id (the 8-byte array slot); NULL if out of range / fault / null slot. */
static void *ae_entity_ptr(void *array, uint32_t count, int id)
{
    if (id < 0 || (uint32_t)id >= count) return NULL;
    void *e = NULL;
    if (!ae_read_ptr((const uint8_t *)array + (size_t)id * 8, &e)) return NULL;
    return e;
}

/* declMgr -> reflection mgr: reflect = (*(*declMgr + 0x80))(declMgr). SEH-guarded; NULL on any fault.
 * Mirrors sh_typeinfo's ti_get_reflect + the reference implementation declMgr.readPointer().add(0x80).readPointer(). */
static void *ae_get_reflect(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    if (!declmgr) return NULL;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_REFLECT_ACCESSOR);
        if (!fn) return NULL;
        return fn(declmgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* read an idStr (48-byte layout: int len@+8; char* data@+0x10 [heap] or inline [SSO]). Copies up to cap-1
 * bytes into out + NUL. Returns the byte length written (0 on fault / empty). SEH-guarded. */
static int ae_read_idstr(const void *p, char *out, int cap)
{
    if (cap > 0) out[0] = '\0';
    if (!p || cap <= 1) return 0;
    int written = 0;
    __try {
        int len = *(const int *)((const uint8_t *)p + IDSTR_LEN_OFF);
        if (len <= 0 || len > APPLY_TEXT_CAP) return 0;
        const char *base;
        if (len >= 0x10) base = *(const char * const *)((const uint8_t *)p + IDSTR_DATA_OFF);
        else             base = (const char *)((const uint8_t *)p + IDSTR_DATA_OFF);
        if (!base) return 0;
        int n = len < (cap - 1) ? len : (cap - 1);
        for (int i = 0; i < n; i++) out[i] = base[i];
        out[n] = '\0';
        written = n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        written = 0;
    }
    return written;
}

/* ============================================================ bound-state predicates =============== */
static int ae_serialize_bound(void)
{
    return g_entity_clone && g_ser && g_render && g_def_ctor && g_def_dtor
        && g_node_ctor && g_node_dtor && g_idstr_ctor && g_idstr_dtor;
}
static int ae_deserialize_bound(void)
{
    return g_lexer && g_deser && g_idstr_ctor && g_lex_ctor && g_node_ctor && g_node_dtor;
}
static int ae_commit_bound(void)
{
    return g_def_ctor && g_def_dtor && g_decl_rebuild && g_idstr_assign;
}

/* ============================================================ PASS 1: serialize entity -> JSON =====
 * the reference implementation serializeEntityToJson (the +0xc8 serializer, XINPUT1_3 FUN_1800044a0). Specialized to a decl
 * type name (typeName) + a clone SOURCE ADDRESS (cloneBase). For an entity: cloneBase = ent+8 (the clone
 * 0x5a6460 reads cloneBase+0x150 = the defsub, == ent+0x158). Returns the byte length written into
 * out_json (0 on any failure). SEH-guarded throughout; every alloc dtor'd in the finally-equivalent. */
static int ae_serialize_to_json(const char *typeName, void *cloneBase, char *out_json, int cap)
{
    if (cap > 0) out_json[0] = '\0';
    if (!ae_serialize_bound() || !cloneBase) {
        AE_SER_DIAG("ser[%s]: bail early -- bound=%d cloneBase=%p",
                    typeName ? typeName : "?", ae_serialize_bound(), cloneBase);
        return 0;
    }

    void *reflect = ae_get_reflect();
    if (!reflect) {
        AE_SER_DIAG("ser[%s]: reflect NULL (declMgr/vtable+0x80 unresolved)", typeName ? typeName : "?");
        return 0;
    }
    AE_SER_DIAG("ser[%s]: tid=%lu cloneBase=%p reflect=%p", typeName ? typeName : "?",
                GetCurrentThreadId(), cloneBase, reflect);

    /* temp idSnapEntity (the clone target). Zero the stack buffers for parity with the reference implementation's zeroed
     * Memory.alloc (a raw stack array would otherwise feed trailing garbage into the engine ctors/copies). */
    uint8_t tmpDef[TEMP_DEF_SIZE];
    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);
    memset(node7, 0, sizeof node7);
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int def_ctored = 0, node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;

    __try {
        g_def_ctor(tmpDef);     def_ctored = 1;            /* 0x5e9400 */
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;

        /* clone the live source into the temp: (*0x5a6460)(cloneBase, tmp, 1). For an entity cloneBase =
         * ent+8 so cloneBase+0x150 = the defsub. */
        g_entity_clone(cloneBase, tmpDef, 1);
        {
            void *defsub = NULL;
            int got = ae_read_ptr(tmpDef + 0x150, &defsub);   /* clone wrote the defsub at tmp+0x150 */
            AE_SER_DIAG("ser[%s]: cloned -- tmp+0x150 defsub=%s%p",
                        typeName ? typeName : "?", got ? "" : "(read-fault)", defsub);
        }

        /* arg5 {1,1,0} tag + node@+0x08 (matches the reference implementation + the deserialize layout). NOTE: a "+0x10 node"
         * variant (claimed from the StructSerialize decompile) was tried and CRASHED StructSerialize
         * (live AV FUN_141a21b40+0x5b) -- so +0x08 is the correct node placement here. The empty-output
         * cause is the THREAD (this runs off the DOOM main thread), NOT arg5. */
        uint8_t arg5[8 + PARSE_NODE_SIZE];
        memset(arg5, 0, sizeof arg5);
        *(uint16_t *)(arg5) = 1;
        arg5[2] = 1;
        arg5[3] = 0;
        memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);

        /* 0x1a21b40(reflect, typeName, tmpDef, outTree, arg5, 0) -- reflection-serialize struct->tree. */
        char ok = g_ser(reflect, typeName, tmpDef, outTree, arg5, NULL);
        AE_SER_DIAG("ser[%s]: StructSerialize ret=%d", typeName ? typeName : "?", (int)(ok & 0xff));
        if (ok & 0xff) {
            g_render(outTree, jsStr);                      /* 0x1a43730 -> JSON text into jsStr */
            int rlen = 0; ae_read_u32_safe(jsStr + IDSTR_LEN_OFF, &rlen);
            written = ae_read_idstr(jsStr, out_json, cap); /* idStr 48-byte layout */
            AE_SER_DIAG("ser[%s]: rendered idStr len=%d written=%d first=\"%.32s\"",
                        typeName ? typeName : "?", rlen, written, written > 0 ? out_json : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
        AE_SER_DIAG("ser[%s]: SEH fault in serialize body", typeName ? typeName : "?");
    }

    /* teardown (order mirrors the reference implementation: jsStr -> outTree -> node7 -> tmpDef). */
    if (js_ctored)    { __try { g_idstr_dtor(jsStr); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)  { __try { g_node_dtor(outTree); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored) { __try { g_node_dtor(node7); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (def_ctored)   { __try { g_def_dtor(tmpDef); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* ============================================================ PASS 4+5: deserialize text -> object ==
 * the reference implementation deserializeTextToObject (FUN_180004950): parse-node(kind7) -> idLexer ctor(0xC0) ->
 * parse-node(kind0) -> src idStr -> lexer parse -> reflect -> structDeserialize(typeName,...) (SIX args).
 * FIX A teardown: src idStr -> parseNode(kind0) -> idLexer's two embedded idStrs (+0x30/+0x88, SSO-guarded)
 * -> node7(kind7). Does NOT call the OG raw lexer-buffer free 0x1ab32e0 (corruption-cookie fatal on our
 * SSO lexer). Returns 1 on a successful lex+deserialize. SEH-guarded; any gap -> 0 (no crash). */
static int ae_deserialize_to_obj(const char *text, void *dstObj, const char *typeName)
{
    if (!ae_deserialize_bound() || !text || !dstObj) {
        AE_DESER_DIAG("deser[%s]: bail early -- bound=%d text=%p dstObj=%p",
                      typeName ? typeName : "?", ae_deserialize_bound(), (void *)text, dstObj);
        return 0;
    }
    void *reflect = ae_get_reflect();
    if (!reflect) {
        AE_DESER_DIAG("deser[%s]: reflect NULL (declMgr/vtable+0x80 unresolved)", typeName ? typeName : "?");
        return 0;
    }
    AE_DESER_DIAG("deser[%s]: start, text len=%zu", typeName ? typeName : "?", strlen(text));

    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t lexer[LEXER_SIZE];
    uint8_t parseNode[PARSE_NODE_SIZE];
    uint8_t srcStr[IDSTR_SIZE];
    memset(node7, 0, sizeof node7);       /* parity with the serialize path (all its buffers are memset) */
    memset(lexer, 0, sizeof lexer);
    memset(parseNode, 0, sizeof parseNode);
    memset(srcStr, 0, sizeof srcStr);
    int node7_ctored = 0, lex_ctored = 0, pn_ctored = 0, src_ctored = 0;
    int ok = 0;

    __try {
        g_node_ctor(node7, 7);          node7_ctored = 1;  /* the {tag} arg5 sub-node (kind 7) */
        /* ROOT-CAUSE FIX (2026-07-10, save->reload "Memory corruption before block" crash): LexCtxCtor
         * (g_lex_ctor, 0x1a5bb70) does NOT construct the lexer's two embedded idStrs at +0x30/+0x88. OG's
         * deserialize wrapper FUN_180004370 constructs them SEPARATELY (0x19fd040) BEFORE LexCtxCtor -- proving
         * LexCtxCtor leaves them untouched. We used to call g_lex_ctor alone on an un-memset buffer, so those two
         * idStrs held stack GARBAGE; the lexer grows them while tokenizing and the teardown (g_idstr_dtor at
         * lexer+0x30/+0x88 below) frees a garbage/heap pointer -> heap corruption, detected as an IdStrDtor AV on
         * the NEXT map load. Shared by EVERY kind=0 commit (acctargets/timeline/bss), which is why they all
         * crashed on save/reload. Construct the two idStrs explicitly (empty), matching OG's order. */
        g_idstr_ctor(lexer + LEXER_IDSTR0_OFF, "");
        g_idstr_ctor(lexer + LEXER_IDSTR1_OFF, "");
        g_lex_ctor(lexer);              lex_ctored = 1;    /* LexCtxCtor -- context fields only, NOT the +0x30/+0x88 idStrs */
        g_node_ctor(parseNode, 0);      pn_ctored = 1;     /* the tree the lexer fills + deser reads (kind 0) */
        g_idstr_ctor(srcStr, text);     src_ctored = 1;    /* src idStr from the C string */

        /* 0x1a5cd90(lexer, src, parseNode, 1) -> success char. */
        AE_DESER_DIAG("deser[%s]: about to call Lexer", typeName ? typeName : "?");
        char lexed = g_lexer(lexer, srcStr, parseNode, 1);
        AE_DESER_DIAG("deser[%s]: Lexer returned %d", typeName ? typeName : "?", (int)(lexed & 0xff));
        if (lexed & 0xff) {
            /* arg5 for StructDeserialize: header 8 bytes, node@+0x08 -- DIRECT FUN_141a1d450 reads
             * {*param_5, param_5[1], param_5[2]} then FUN_141a41100(.., param_5+8). This is CORRECT for
             * deserialize and DIFFERS from serialize (FUN_141a21b40, header 0x10, node@+0x10) -- the two
             * paths legitimately differ; do NOT unify them. */
            uint8_t arg5[8 + PARSE_NODE_SIZE];
            memset(arg5, 0, sizeof arg5);
            *(uint16_t *)(arg5) = 1;
            memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
            /* 0x1a1d450(reflect, typeName, dstObj, parseNode, arg5, 0) -- SIX args. */
            AE_DESER_DIAG("deser[%s]: about to call StructDeserialize dstObj=%p", typeName ? typeName : "?", dstObj);
            g_deser(reflect, typeName, dstObj, parseNode, arg5, NULL);
            AE_DESER_DIAG("deser[%s]: StructDeserialize returned (no crash)", typeName ? typeName : "?");
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
        AE_DESER_DIAG("deser[%s]: SEH fault in deserialize body", typeName ? typeName : "?");
    }
    AE_DESER_DIAG("deser[%s]: ok=%d", typeName ? typeName : "?", ok);

    /* FIX A teardown, faithful order. The idStr dtor is SSO/heap-flag-guarded (no-op when never grown). */
    if (src_ctored)   { __try { g_idstr_dtor(srcStr); }                 __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (pn_ctored)    { __try { g_node_dtor(parseNode); }               __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (lex_ctored) {
        __try { g_idstr_dtor(lexer + LEXER_IDSTR0_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { g_idstr_dtor(lexer + LEXER_IDSTR1_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (node7_ctored) { __try { g_node_dtor(node7); }                   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return ok;
}

/* LAYER C: extract a string field's value from the patched QJson entity text (handles JSON `"field":"v"`
 * and decl `field = "v"`): find the field token at a key boundary, then the value quote after the separator
 * (: or =). Returns 1 + buf on a hit, 0 on a miss. Used for both `class` and `inherit`. */
static int ae_extract_field(const char *text, const char *field, char *buf, size_t cap)
{
    if (cap < 2) return 0;
    buf[0] = '\0';
    if (!text || !field || !field[0]) return 0;
    size_t flen = strlen(field);
    const char *p = text;
    while ((p = strstr(p, field)) != NULL) {
        char before = (p == text) ? ' ' : p[-1];
        char after  = p[flen];
        int btok = (before==' '||before=='\t'||before=='\n'||before=='\r'||before=='{'||before==','||before=='"');
        int atok = (after==' '||after=='\t'||after=='='||after==':'||after=='"');
        if (btok && atok) {
            const char *sep = p + flen;
            while (*sep && *sep != '=' && *sep != ':' && *sep != '\n' && *sep != '}') sep++;
            if (*sep == '=' || *sep == ':') {
                const char *q1 = strchr(sep, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && (size_t)(q2 - (q1 + 1)) < cap) {
                        size_t len = (size_t)(q2 - (q1 + 1));
                        memcpy(buf, q1 + 1, len);
                        buf[len] = '\0';
                        if (buf[0]) return 1;
                    }
                }
            }
        }
        p += flen;
    }
    return 0;
}

/* ============================================================ the per-id bss/bse apply (steps 4-7) ==
 * the reference implementation doApplyBssOne tail: deserialize the FULL patched entity text onto a TEMP def, then commit the
 * temp's normalized source/class/inherit onto the LIVE defsub, then dtor the temp. The patched text is the
 * frontend's QJson-patched full entity (the serialize already happened on the frontend's +0xc8 call). A
 * full entity carries the entity's REAL class/inherit (NOT the ctor "snapmaps/unknown" sentinel) so the
 * copy is faithful; a null field is skipped defensively. Returns 1 on a committed apply. */
static int ae_apply_one(int id, const char *patched_text)
{
    if (!ae_deserialize_bound() || !ae_commit_bound() || !patched_text) return 0;
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    void *defsub = NULL;
    if (!ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return 0;

    /* DE-SHARE DISABLED (2026-07-10) -- HYPOTHESIS TEST: match OG's confirmed commit discipline. OG SnapHak's
     * commit function (XINPUT1_3.dll FUN_1800045a0, both 1.31 and Beta 2) pokes the live defsub in place
     * (DeclSourceRebuild + IdStrAssign, exactly like the code below) with NO COW make-unique first. Verified by
     * instruction search: the engine de-share RVA 0x52c920 appears in 0 of 22,197 (1.31) and 0 of 36,277 (Beta 2)
     * XINPUT instructions, while the search method is validated (it finds 0x5e9400/0x17ae560 in the same commit).
     * So OG never de-shares in ANY edit path, yet doesn't crash on the acctargets->play->play repro that our
     * de-share-present build DOES crash on. Removing the de-share moves us TOWARD OG's proven-safe behavior.
     *
     * ORIGINAL rationale kept for context (this de-share was added to fix a create-timeline use-after-free:
     * de-share-pass AV 0x5a5167 on play-exit / wire-render AV 0xd32a39 on reload). NOTE those were ALSO play-exit
     * crashes -- and the de-share does NOT prevent the current acctargets play-exit crash -- so it may have been a
     * partial/wrong fix for a shared root cause OG addresses differently (OG: don't de-share, DO settle). If this
     * change reintroduces the create-timeline UAF, that path needs its own OG-matching fix, not a blanket de-share.
     * (void)g_deshare;  -- resolved-but-unused is fine. */
    (void)g_deshare;

    /* LAYER C (crash prevention): a class+inherit pair in the patched text where the class does NOT derive
     * from the inherit's base would FATALLY fault the deserialize below (the engine's "Class X does not
     * derive from Y" Error(6), inner-caught -> the fault-shield can't recover it). Reject up front -- extract
     * the resulting class + inherit + apply the engine-EXACT rule (a missing field falls back to the live
     * defsub value inside sh_iface_class_inherit_ok). */
    {
        char newcls[256], newinh[256];
        int hc = ae_extract_field(patched_text, "class",   newcls, sizeof newcls);
        int hi = ae_extract_field(patched_text, "inherit", newinh, sizeof newinh);
        if ((hc || hi) && !sh_iface_class_inherit_ok(id, hc ? newcls : NULL, hi ? newinh : NULL))
            return 0;   /* LAYER C: the resulting pair would fatally fault the deserialize -> skip it (no crash) */
    }

    /* (Double-commit / OG-settle-commit was tried here 2026-07-11 and TESTED-FAILED: byte-identical reload fault.
     * A second DeclSourceRebuild does NOT promote the blob to permanent. Removed. Leading remaining lead: our
     * committed decl is UNREGISTERED in the declManager -- see the "first apply not findable by reg-id" note below
     * -- while a normally-placed entity's decl IS registered; registration is the likely blob-permanence factor.) */

    uint8_t tmpDef[TEMP_DEF_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);   /* CRITICAL: zero before the ctor -- matches the serialize/prefab paths
                                         * (lines ~395/916/964) AND OG's commit FUN_1800045a0, which zeros its
                                         * whole temp-def stack region before 0x5e9400. Without this, members the
                                         * ctor doesn't fully init keep stack GARBAGE; g_def_dtor below then frees a
                                         * garbage idStr pointer -> "Memory corruption before block" AV in IdStrDtor
                                         * on reload-teardown (the acctargets/bss/timeline save->reload crash). */
    int def_ctored = 0, applied = 0;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;                /* 0x5e9400 */
        /* STEP 3 (deserialize the full modified entity onto the temp -> survives, it is a valid entity). */
        if (ae_deserialize_to_obj(patched_text, tmpDef, "idSnapEntity")) {
            /* STEP 4 commit (FUN_180004b80 tail). */
            void *srcPtr = *(void * const *)(tmpDef + TDEF_SOURCE_OFF);   /* normalized source-text ptr */
            void *clsPtr = *(void * const *)(tmpDef + TDEF_CLASS_OFF);    /* normalized classname idStr-data */
            void *inhPtr = *(void * const *)(tmpDef + TDEF_INHERIT_OFF);  /* normalized inherit idStr-data */
#if AE_APPLY_DIAG_ON
            {
                int slen = -1, clen = -1, ilen = -1;
                __try { slen = srcPtr ? (int)strlen((const char *)srcPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { slen = -2; }
                __try { clen = clsPtr ? (int)strlen((const char *)clsPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { clen = -2; }
                __try { ilen = inhPtr ? (int)strlen((const char *)inhPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { ilen = -2; }
                AE_APPLY_DIAG("apply id=%d: textlen=%d srcPtr=%p slen=%d clsPtr=%p clen=%d cls='%.48s' inhPtr=%p ilen=%d inh='%.48s'",
                              id, (int)strlen(patched_text), srcPtr, slen, clsPtr, clen,
                              (clen >= 0 ? (const char *)clsPtr : "?"), inhPtr, ilen, (ilen >= 0 ? (const char *)inhPtr : "?"));
                if (slen >= 0) AE_APPLY_DIAG("apply id=%d: src head='%.220s'", id, (const char *)srcPtr);
                if (slen >= 0 && slen > 200) AE_APPLY_DIAG("apply id=%d: src tail='%.120s'", id, (const char *)srcPtr + slen - 120);
            }
#endif
            /* the source rebuild carries the EDIT (the temp's canonical source includes the leaf) -- always. */
            g_decl_rebuild(defsub, (const char *)srcPtr, 1);             /* 0x17ae560 */
            AE_APPLY_DIAG("apply id=%d: decl_rebuild returned", id);
            /* class/inherit: with a full entity these are real values; null OR EMPTY -> skip (keep the live
             * defsub value). The empty-string guard is the timeline-save CRASH FIX: a complex componentTimeLine
             * can make StructDeserialize("idSnapEntity") choke and leave the temp's class/inherit BLANK; without
             * this guard we idstr_assign "" onto the live entity -> "No class specified"/"inherit = " on the
             * next map load -> the entity vanishes from the list + Save Map/play crashes (shield: "Couldn't find
             * map entity in entity palette '' inherit = "). Committing an empty class is never valid for ANY
             * kind=0 caller (Entities edit / wire-target / timeline), so this guard is universal, not
             * timeline-specific: keep-live also correctly preserves a placeholder timeline's real inherit
             * (snapmaps/editor_only/placeholder_target) instead of blanking it. This makes a choked timeline
             * edit fail SAFE (entity intact, edit didn't apply) rather than fatal. */
            if (clsPtr && *(const char *)clsPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF, (const char *)clsPtr);
            AE_APPLY_DIAG("apply id=%d: class assign done", id);
            if (inhPtr && *(const char *)inhPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, (const char *)inhPtr);
            AE_APPLY_DIAG("apply id=%d: inherit assign done -- commit complete", id);

            /* (SETTLE re-serialize REMOVED 2026-07-10: the "unsettled decl" theory was disproven -- OG's entity is
             * +0x48=0x10400 too. It didn't fix the crash and its extra EntityClone may compound the COW share
             * (refcount) that is the ACTUAL root cause -- see the refcount-2-vs-1 finding in the investigation
             * notes. The real fix is keeping the entity UNIQUE (refcount 1) like OG, not settling it.) */
            applied = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = 0;
    }
    if (def_ctored) { __try { g_def_dtor(tmpDef); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    /* NOTE (do NOT re-add a decl-unregister here). ae_apply_one rebuilds the decl (g_decl_rebuild); it must NOT
     * then un-register the per-entity decl via Remove_Locked (0x1801ae0). At an entity's first apply the decl is
     * not yet findable in the declManager by its reg-id, so Remove_Locked raises FatalError("Resource wasn't
     * found by ID") (0x1a089e0); the fault-shield's FatalError->Error(6) downgrade then masks that fatal into a
     * poisoned-but-alive process, so the NEXT map load crashes. Let the decl's OWN destructor un-register it at
     * teardown, when the registration is complete + findable. (This apply path is shared by the Timeline Editor
     * commit + bss + wire-target writes; keep it decl-safe.) */
    return applied;
}

/* ============================================================ sh_target_any TARGETS-WRITE (Fix B) ==========
 * The correct persisted form of "source's output triggers a TIMELINE" is the SOURCE entity's native
 * `state.edit.targets` list holding the timeline's full module-qualified id -- on fire the source posts
 * `activate` to each target -> the timeline plays (DIRECT: every working ground-truth map). NOT a SnapMap-logic
 * CSR `connections` edge (a timeline is a node-less is-target the CSR resolver cannot route -> the stray/dangling
 * wire that crashes on re-entry/draw). So sh_target_any, for a bare timeline target, SUPPRESSES the CSR edge
 * (wiring_cleandirect) and calls this to write the targets field via the SAME decl-edit round-trip that authors
 * componentTimeLine (serialize -> splice -> ae_apply_one). */

/* Insert (or append) `"targets":{"item[N]":"<ref>","num":N+1}` into entityDef.state.edit of a serialized entity
 * JSON. Raw string splice (the backend carries no JSON lib) -- a malformed result just fails the deserialize in
 * ae_apply_one (SEH-guarded, no crash). Returns 1 on success (out = spliced), 0 otherwise. */
static int ae_splice_targets(const char *src, const char *ref, char *out, int cap)
{
    if (!src || !ref || !out || cap <= 0) return 0;
    const char *edit = strstr(src, "\"edit\"");
    if (!edit) return 0;
    const char *brace = strchr(edit, '{');
    if (!brace) return 0;
    const char *targets = strstr(brace, "\"targets\"");   /* a listener source carries no other 'targets' key */
    if (targets) {
        /* APPEND: bump the targets object's "num" + insert a new item[N] before it. */
        const char *tbrace = strchr(targets, '{');
        if (!tbrace) return 0;
        const char *num = strstr(tbrace, "\"num\"");
        if (!num) return 0;
        const char *colon = strchr(num, ':');
        if (!colon) return 0;
        const char *p = colon + 1; while (*p == ' ' || *p == '\t') p++;
        int N = atoi(p);
        while (*p >= '0' && *p <= '9') p++;                /* p := just past the num value */
        size_t pre = (size_t)(num - src);                 /* everything up to the "num" key */
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"item[%d]\":\"%s\",\"num\":%d%s",
                            (int)pre, src, N, ref, N + 1, p) > 0) ? 1 : 0;
    }
    /* INSERT: a fresh targets object right after the edit "{". */
    {
        size_t pre = (size_t)(brace - src) + 1;           /* include the "{" */
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"targets\":{\"item[0]\":\"%s\",\"num\":1},%s",
                            (int)pre, src, ref, brace + 1) > 0) ? 1 : 0;
    }
}

/* kind=3: write the timeline TARGET's id into the SOURCE entity's state.edit.targets. Resolves the target's
 * module-qualified ref (rejects a still-global "(no module)" target -- its ref would not resolve), serializes
 * the source, splices, and commits via ae_apply_one. Runs on the main thread (the clone_bss_apply drain). */
static int ae_apply_target_write(int source_id, int target_id)
{
    char ref[256] = {0};
    const char *r = ie_resolve_id_string(target_id, ref, (int)sizeof ref);
    if (!r || !ref[0]) return 0;
    if (strstr(ref, "(no module)")) {
        backend_log("wire-target: target not in a module yet -- drag the timeline into a module first (skipped)");
        return 0;
    }
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, source_id);
    if (!ent) return 0;
    void *cloneBase = (uint8_t *)ent + ENT_VALID_OFF;     /* ent+8 = the serialize clone base */
    static char srcjson[64 * 1024];
    static char patched[64 * 1024 + 512];
    if (!ae_serialize_to_json("idSnapEntity", cloneBase, srcjson, (int)sizeof srcjson)) return 0;
    /* IDEMPOTENT: if this exact ref is already in the source's targets, skip the append -- the wire hook can fire
     * more than once for the same pick, and a duplicate item[N] would trigger the timeline twice. */
    {
        char quoted[264];
        _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", ref);
        if (strstr(srcjson, quoted)) { backend_log("wire-target: target already in source.targets -- skipped (idempotent)"); return 1; }
    }
    if (!ae_splice_targets(srcjson, ref, patched, (int)sizeof patched)) return 0;
    int ok = ae_apply_one(source_id, patched);
    if (ok) {
        char m[200];
        _snprintf_s(m, sizeof m, _TRUNCATE, "wire-target: source %d -> state.edit.targets += \"%s\" (applied)", source_id, ref);
        backend_log(m);
    }
    return ok;
}

/* ============================================================ mkcmd apply (deserialize -> +0x209a8) =
 * the reference implementation doMkcmdApplyNow: deserialize the prefab text as "idSnapEntityPrefab" into editor+0x209a8 (the
 * +0xb8 paste path). Reuses the SAME deserialize chain (only the type name + the destination differ).
 * Returns 1 on a successful deserialize. */
static int ae_mkcmd_one(const char *prefab_text)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed || !prefab_text) return 0;
    void *staging = (void *)(ed + PASTE_STAGING_OFF);   /* editor+0x209a8 (BUILD-MISMATCH risk -- R3) */
    /* Reset the REUSED staging slot to a clean idSnapEntityPrefab before deserializing. After a delete (and/or a
     * Play round-trip) the slot can retain a nested entity whose className idStr is NULL; the engine's reflection
     * deserialize then does a compare-then-assign (FUN_141800320) that reads that NULL className -> an
     * access-violation in the engine string compare (the observed delete-then-create-timeline crash). Re-ctor'ing
     * the slot makes the deserialize build the nested entities FRESH instead of reusing the stale one. Verified
     * crash-safe on a live slot: the ctor (FUN_14054d0a0) rewrites every field unconditionally with no reads/
     * branches, so it cannot double-free; it only leaks the slot's prior list allocations (small, one-shot per
     * create -- acceptable vs a crash). SEH-guarded: a failed reset falls through to the pre-existing behavior. */
    if (g_prefab_ctor) { __try { g_prefab_ctor(staging); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return ae_deserialize_to_obj(prefab_text, staging, "idSnapEntityPrefab");
}



/* ============================================================ the clone_bss_apply command (FIX B) ===
 * The engine drains this on the DOOM main thread at ExecuteCommandBuffer (the decl-safe exec point). It
 * consumes the pending batch the frontend stashed, runs the heavy apply per item, frees the batch, and
 * toasts the result count (so the result can be read from the backend toast log). __cdecl(void) -- the
 * engine AddCommand callback shape (matches the reference implementation's NativeCallback('void', [])). */
static void ae_toast_result(const char *op, int applied, int total)
{
    /* Reach the toast through the bound interface vtable slot (sh_iface_engine bound +0x1b8). */
    sh_iface *iface = sh_ui_get_iface();
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s: applied %d/%d (engine round-trip)",
                op[0] ? op : "apply", applied, total);
    /* Load/Place already shows its own actionable toast ("staged -- press Ctrl+V to place it") the
     * moment it schedules -- this generic "SnapStack: ..." one just duplicates/confuses that with no
     * new information (it's the same op every Qt sh_apply-style caller shares, hence the fixed
     * "SnapStack" label, which reads as unrelated to a Prefabs-tab action). Skip it for that op only;
     * the Qt mkcmd command has no toast of its own, so it still needs this one. */
    if (iface && iface->vtbl && iface->vtbl->toast && strcmp(op, "load-prefab") != 0)
        iface->vtbl->toast(iface, "SnapStack", text);
    /* ALSO log directly -- the toast slot logs too, but a no-editor/late drain might skip the engine toast. */
    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE, "C2 apply: %s applied %d/%d", op[0] ? op : "apply", applied, total);
    backend_log(line);
}

static void __cdecl ae_clone_bss_apply_cmd(void)
{
    apply_item_copy *items = NULL;
    int count = 0;
    char op[32];

    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    items = g_pending_items; count = g_pending_count;
    memcpy(op, g_pending_op, sizeof op); op[sizeof op - 1] = '\0';
    g_pending_items = NULL; g_pending_count = 0; g_pending_op[0] = '\0';   /* consume */
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);

    if (!items || count <= 0) { ae_toast_result(op, 0, 0); return; }

    int applied = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].text) continue;
        int ok = (items[i].kind == 1) ? ae_mkcmd_one(items[i].text)
               : (items[i].kind == 3) ? ae_apply_target_write(items[i].id, atoi(items[i].text))
                                      : ae_apply_one(items[i].id, items[i].text);
        if (ok) applied++;
    }
    ae_toast_result(op, applied, count);

    /* free the consumed batch (allocated in slot_schedule_apply). */
    for (int i = 0; i < count; i++) free(items[i].text);
    free(items);
}

/* register clone_bss_apply ONCE (lazy, on the first schedule). AddCommand takes the cmd-system lock; the
 * frontend's schedule runs on the UI thread (the +0x1a0 drain), which is a safe point for AddCommand
 * (the reference implementation registers it on the menu pump). Returns 1 if registered (or already was). */
static int ae_ensure_command(void)
{
    if (InterlockedCompareExchange(&g_cmd_registered, 1, 0) != 0) return 1;   /* already registered */
    if (!g_add_command || !g_cmdsys) {
        InterlockedExchange(&g_cmd_registered, 0);   /* allow a later retry once deps bind */
        return 0;
    }
    __try {
        g_add_command(g_cmdsys, CLONE_BSS_CMD, (void *)ae_clone_bss_apply_cmd, NULL,
                      "clone bss apply (decl-safe)", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_cmd_registered, 0);
        return 0;
    }
    backend_log("C2: clone_bss_apply engine command registered (command-buffer apply routing live)");
    return 1;
}

/* ============================================================ the vtable slot bodies =============== */

/* +0xc8 serialize entity id -> the FULL idSnapEntity JSON. the reference implementation serializeEntityToJson: cloneBase =
 * ent+8 (the clone 0x5a6460 reads cloneBase+0x150 = ent+0x158 = the defsub). */
static int slot_serialize_entity(sh_iface *self, int id, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    /* cloneBase = ent+8 (the ADDRESS, not a deref) -- the reference implementation ent.add(ENT_VALID_OFF). */
    void *cloneBase = (void *)((uint8_t *)ent + ENT_VALID_OFF);
    return ae_serialize_to_json("idSnapEntity", cloneBase, out_json, cap);
}

/* +0xd0 SCHEDULE a batch of apply-items at the engine command-exec point (FIX B). Deep-copies the items
 * (incl. the text strings) into the pending store, registers clone_bss_apply, BufferCommandTexts it. */
static int slot_schedule_apply(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    if (!g_buffer_cmd || !g_cmdsys) return 0;
    if (!ae_ensure_command()) return 0;

    /* deep-copy the batch OUTSIDE the lock. */
    apply_item_copy *copy = (apply_item_copy *)calloc((size_t)count, sizeof(apply_item_copy));
    if (!copy) return 0;
    int built = 0;
    for (int i = 0; i < count; i++) {
        const char *t = items[i].text ? items[i].text : "";
        size_t len = strlen(t);
        if (len + 1 > APPLY_TEXT_CAP) { len = APPLY_TEXT_CAP - 1; }
        char *tc = (char *)malloc(len + 1);
        if (!tc) break;
        memcpy(tc, t, len); tc[len] = '\0';
        copy[built].kind = items[i].kind;
        copy[built].id   = items[i].id;
        copy[built].text = tc;
        built++;
    }
    if (built == 0) { free(copy); return 0; }

    /* publish the pending batch (replace any stale one -- one apply per enqueue, the reference implementation). */
    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    apply_item_copy *stale = g_pending_items; int stale_n = g_pending_count;
    g_pending_items = copy; g_pending_count = built;
    if (op_label) { strncpy_s(g_pending_op, sizeof g_pending_op, op_label, _TRUNCATE); }
    else          { g_pending_op[0] = '\0'; }
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);
    if (stale) { for (int i = 0; i < stale_n; i++) free(stale[i].text); free(stale); }

    /* enqueue at the engine command buffer; drained main-thread next frame at ExecuteCommandBuffer. */
    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, CLONE_BSS_CMD "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }
    return enq;
}

/* PUBLIC (Fix B): enqueue a kind=3 targets-write {source, target}. The target id travels as a decimal string in
 * the item text (the drain parses it back). The actual serialize+splice+apply runs on the DOOM main thread via
 * the clone_bss_apply command. Called by the sh_target_any confirm hook (wiring_cleandirect) once per wire-any
 * timeline pick, INSTEAD of the stock creator laying an (invalid, dangling) CSR edge to the timeline. */
void ae_schedule_target_write(int source_id, int target_id)
{
    char tgt[16];
    _snprintf_s(tgt, sizeof tgt, _TRUNCATE, "%d", target_id);
    sh_apply_item it;
    it.kind = 3;
    it.id   = source_id;
    it.text = tgt;
    slot_schedule_apply(NULL, &it, 1, "wire-target");
}

/* +0xb8 READ-BACK the editor's pending prefab (editor+0x209a8) -> idSnapEntityPrefab JSON (the reference implementation
 * readPrefabStagingJson -- the +0xb0 serialize INVERSE; the +0x209a8 build-mismatch verification). The
 * slot already holds a live idSnapEntityPrefab after a paste/mkcmd, so a direct reflection-serialize of
 * it is the read-back (cloneBase = the staging address -- StructSerialize reads the object directly, no
 * clone for prefab; we pass the slot as srcObj). */
static int slot_read_prefab(sh_iface *self, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    if (!g_ser || !g_render || !g_node_ctor || !g_node_dtor || !g_idstr_ctor || !g_idstr_dtor) return 0;
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *reflect = ae_get_reflect();
    if (!reflect) return 0;
    void *staging = (void *)(ed + PASTE_STAGING_OFF);

    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(node7, 0, sizeof node7);     /* parity with the reference implementation zeroed Memory.alloc */
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;
    __try {
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;
        /* StructSerialize arg5: {1,1,0} tag + node@+0x08 (matches the working ae_serialize_to_json; +0x10 crashed). */
        uint8_t arg5[8 + PARSE_NODE_SIZE];
        memset(arg5, 0, sizeof arg5);
        *(uint16_t *)(arg5) = 1;
        arg5[2] = 1;
        arg5[3] = 0;
        memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
        char ok = g_ser(reflect, "idSnapEntityPrefab", staging, outTree, arg5, NULL);
        if (ok & 0xff) { g_render(outTree, jsStr); written = ae_read_idstr(jsStr, out_json, cap); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
    }
    if (js_ctored)    { __try { g_idstr_dtor(jsStr); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)  { __try { g_node_dtor(outTree); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored) { __try { g_node_dtor(node7); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* ============================================================ +0xb0 serialize SELECTION -> prefab ==
 * Faithful port of the OG XINPUT1_3 FUN_180004210 (the Prefabs create body): ctor a temp idSnapEntityPrefab,
 * populate it from the editor selection (returns a char success), and on success reflection-serialize it as
 * "idSnapEntityPrefab" + tree-render to JSON. Writes up to cap-1 bytes; returns the byte length (0 on a
 * binding gap / empty selection / fault). Every engine call SEH-guarded; the temp prefab is always dtor'd. */
static int slot_serialize_selection(sh_iface *self, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    if (!g_prefab_ctor || !g_prefab_populate || !g_prefab_dtor ||
        !g_ser || !g_render || !g_node_ctor || !g_node_dtor || !g_idstr_ctor || !g_idstr_dtor)
        return 0;
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *reflect = ae_get_reflect();
    if (!reflect) return 0;

    uint8_t prefab[PREFAB_TEMP_SIZE];
    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(prefab, 0, sizeof prefab);
    memset(node7, 0, sizeof node7);
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int prefab_ctored = 0, node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;

    __try {
        g_prefab_ctor(prefab); prefab_ctored = 1;
        int populateStatus = 0;
        char populated = g_prefab_populate(prefab, (void *)ed, &populateStatus);   /* fill from editor selection */
        if (populated & 0xff) {
            g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
            g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
            g_idstr_ctor(jsStr, "");            js_ctored = 1;
            /* StructSerialize arg5: {1,1,0} tag + node@+0x08 (matches ae_serialize_to_json; +0x10 crashed). */
            uint8_t arg5[8 + PARSE_NODE_SIZE];
            memset(arg5, 0, sizeof arg5);
            *(uint16_t *)(arg5) = 1;
            arg5[2] = 1;
            arg5[3] = 0;
            memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
            char ok = g_ser(reflect, "idSnapEntityPrefab", prefab, outTree, arg5, NULL);
            if (ok & 0xff) { g_render(outTree, jsStr); written = ae_read_idstr(jsStr, out_json, cap); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
    }
    if (js_ctored)     { __try { g_idstr_dtor(jsStr); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)   { __try { g_node_dtor(outTree); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored)  { __try { g_node_dtor(node7); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (prefab_ctored) { __try { g_prefab_dtor(prefab); }__except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* +0x290 (ext 5) SYNCHRONOUS inline apply -- the OG-faithful commit path. Runs the apply batch RIGHT NOW on
 * the CALLING (UI/think-loop) thread, exactly like OG's acctargets handler (FUN_18000228c) which calls its
 * +0xd0 commit (FUN_180004b80) INLINE. This REPLACES the deferred clone_bss_apply route for the SnapStack
 * decl-edit ops: the deferral (FIX B) split serialize (UI thread) from commit (DOOM main thread, a later
 * frame), which left the committed decl-source block DOUBLE-OWNED -> the play->teardown double-free
 * (acctargets/bss "Memory corruption before block"). OG never defers -- it commits inline on the SAME
 * UI/think-loop thread where the serialize already runs successfully (so reflect IS resolvable there;
 * slot_serialize_entity proves it), giving the block a single clean owner. Each ae_apply_one is SEH-guarded,
 * so if the deserialize ever DID fault off-main it degrades to 0-applied, never a crash. Returns applied
 * count. Same batch semantics as slot_schedule_apply (kind 0=decl edit / 1=mkcmd / 3=target-write) but
 * inline -- text is caller-owned + valid for the call, so NO deep copy / pending store is needed. */
static int slot_apply_sync(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    {   /* distinctive marker so the log unambiguously shows the SYNC (inline) path ran, vs the deferred
         * clone_bss_apply drain. (remove/quiet before release together with the AE_*_DIAG diagnostics.) */
        char dbg[112];
        _snprintf_s(dbg, sizeof dbg, _TRUNCATE, "C2 SYNC apply: %d item(s) INLINE on this thread (%s)",
                    count, op_label ? op_label : "apply");
        backend_log(dbg);
    }
    int applied = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].text) continue;
        int ok = (items[i].kind == 1) ? ae_mkcmd_one(items[i].text)
               : (items[i].kind == 3) ? ae_apply_target_write(items[i].id, atoi(items[i].text))
                                      : ae_apply_one(items[i].id, items[i].text);
        if (ok) applied++;
    }
    ae_toast_result(op_label ? op_label : "apply", applied, count);
    return applied;
}

/* ============================================================ slot export + install ================ */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab,
                               sh_apply_sync_fn       *apply_sync)
{
    if (serialize_entity) *serialize_entity = slot_serialize_entity;
    if (apply_edit)       *apply_edit       = slot_schedule_apply;
    if (read_prefab)      *read_prefab      = slot_read_prefab;
    if (apply_sync)       *apply_sync       = slot_apply_sync;
}

/* expose the +0xb0 serialize-selection body so sh_iface_engine folds it into the single bind. */
void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection)
{
    if (serialize_selection) *serialize_selection = slot_serialize_selection;
}

int sh_apply_engine_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    g_doom_base = module_base;
    if (module_base) g_editor = module_base + EDITOR_SINGLETON_RVA;
    g_cmdsys = cmdsys;

    if (!g_pending_lock_init) { InitializeCriticalSection(&g_pending_lock); g_pending_lock_init = 1; }

    g_entity_clone = (entity_clone_fn)    sig_addr_by_name(results, n, "EntityClone");
    g_def_ctor     = (entity_def_ctor_fn) sig_addr_by_name(results, n, "EntityDefCtor");
    g_def_dtor     = (entity_def_dtor_fn) sig_addr_by_name(results, n, "EntityDefDtor");
    g_ser          = (struct_serialize_fn)sig_addr_by_name(results, n, "StructSerialize");
    g_deser        = (struct_deser_fn)    sig_addr_by_name(results, n, "StructDeserialize");
    g_render       = (tree_render_fn)     sig_addr_by_name(results, n, "TreeRenderJson");
    g_lexer        = (lexer_fn)           sig_addr_by_name(results, n, "Lexer");
    g_lex_ctor     = (lexctx_ctor_fn)     sig_addr_by_name(results, n, "LexCtxCtor");
    g_node_ctor    = (parse_node_ctor_fn) sig_addr_by_name(results, n, "ParseNodeCtor");
    g_node_dtor    = (parse_node_dtor_fn) sig_addr_by_name(results, n, "ParseNodeDtor");
    g_idstr_ctor   = (idstr_ctor_fn)      sig_addr_by_name(results, n, "IdStrCtor");
    g_idstr_dtor   = (idstr_dtor_fn)      sig_addr_by_name(results, n, "IdStrDtor");
    g_idstr_assign = (idstr_assign_fn)    sig_addr_by_name(results, n, "IdStrAssign");
    g_decl_rebuild = (decl_src_rebuild_fn)sig_addr_by_name(results, n, "DeclSourceRebuild");
    g_buffer_cmd   = (buffer_cmd_fn)      sig_addr_by_name(results, n, "BufferCommandText");
    g_add_command  = (add_command_fn)     sig_addr_by_name(results, n, "AddCommand");

    /* the prefab-from-selection serialize engine fns (+0xb0). These jumptable/inline-prone leaves
     * resolve by FALLBACK RVA off module_base (re-derive-tagged like the editor singleton); a wrong/shifted
     * offset just makes the serialize SEH-fail -> a clean 0-length result, never a crash. */
    if (module_base) {
        g_prefab_ctor     = (prefab_ctor_fn)    (module_base + PREFAB_CTOR_RVA);
        g_prefab_populate = (prefab_populate_fn)(module_base + PREFAB_POPULATE_RVA);
        g_prefab_dtor     = (prefab_dtor_fn)    (module_base + PREFAB_DTOR_RVA);
        g_deshare         = (ent_deshare_fn)    (module_base + ENT_DESHARE_RVA);
    }

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 wave B: apply-engine install -- ser=%d deser=%d commit=%d cmdsys=%p buf_cmd=%p add_cmd=%p",
        ae_serialize_bound(), ae_deserialize_bound(), ae_commit_bound(),
        g_cmdsys, (void *)g_buffer_cmd, (void *)g_add_command);
    backend_log(line);
    return ae_serialize_bound() && ae_deserialize_bound() && ae_commit_bound();
}
