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
#define ED_SEL_OBJ_OFF         0x204d0      /* editor+0x204d0 -> selection object ptr */
#define SEL_HOVERED_OFF        0x2c         /* selObj+0x2c -> looked-at/hovered entity id (-1 = none) */
#define LM_ENTINST_ARR_OFF     0x6f0        /* loaded-map+0x6f0 -> per-entity instance(module)-index array (s32[id]) */
#define LM_INSTANCES_CNT_OFF   0x758        /* loaded-map+0x758 -> module COUNT (== the global/no-module sentinel bucket) */
#define LM_MODXFORM_OFF        0x750        /* loaded-map+0x750 -> the {moduleXformTableBase, moduleCount} struct that
                                            * WorldToModuleLocal takes (world->module-local re-base for birth-in-module) */
#define SEL_IDS_OFF            0x80         /* selObj+0x80 -> selected-id array (the paste AddToSelection's the new ents) */
#define SEL_COUNT_OFF          0x88         /* selObj+0x88 -> selected-id count */
#define ENT_XFORM_OFF          0x288        /* entity+0x288 -> 3x4 transform: [0..2] translation, [3..11] basis */
#define ENT_DIRTY_OFF          0x160        /* entity+0x160 -> transform dirty flags (|= 3 forces a re-render) */
#define ED_CAMERA_ORIGIN_OFF   0x170        /* editor+0x170 -> camera origin vec3 (the spawn ray start) */
#define ED_CAMERA_ANGLES_OFF   0x17c        /* editor+0x17c -> camera {pitch,yaw} (ViewForward input) */
#define PREFAB_GRABDIST_OFF    0x24         /* staging+0x24 -> the staged prefab's grab DISTANCE (spawn = origin+fwd*dist) */
#define MOD_TBL_STRIDE         0x98         /* module-transform table entry stride (E = *(lm+0x750) + m*0x98) */
#define MOD_TBL_VALID_OFF      0x30         /* tableEntry+0x30 -> module present/valid (nonzero) */
#define MOD_OBB_OFF            0xa0         /* moduleObj+0xa0 -> module-local OBB (6 floats: min[0..2], max[3..5]) */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid; ALSO the clone base (ent+8) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object (commit target) */
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
 * (tagged for per-build re-derive, exactly like the editor singleton). The temp-prefab struct size is taken
 * generous (the OG local_6d8 frame slot is 0x210 bytes; we alloc 0x220 zeroed). */
#define PREFAB_CTOR_RVA        0x54d0a0u   /* idSnapEntityPrefab ctor (OG FUN_180004210 local_6d8 ctor) */
#define PREFAB_POPULATE_RVA    0x54e410u   /* populate prefab from editor selection (returns char success) */
#define PREFAB_DTOR_RVA        0x51d870u   /* idSnapEntityPrefab dtor (OG FUN_180004210 cleanup) */
#define PREFAB_TEMP_SIZE       0x220       /* generous sizeof(temp idSnapEntityPrefab) */
#define PASTE_INSTANTIATE_RVA  0x54f950u   /* PasteInstantiate FUN_14054f950: void(prefab=editor+0x209a8, editor)
                                            * -- instantiate the staged prefab into the live map + AddToSelection,
                                            * camera-relative grab placement; does NOT consume the slot. The
                                            * create-from-scratch timeline SPAWN (kind=2) calls it AFTER staging.
                                            * sig-resolved ("PasteInstantiate"); this RVA = hook-tolerant fallback.
                                            * RE'd DIRECT from our own decompile. */
#define WORLD_TO_LOCAL_RVA     0x5a8be0u   /* WorldToModuleLocal FUN_1405a8be0 (world->module-local re-base for the
                                            * birth-in-module SPAWN); sig-resolved ("WorldToModuleLocal"), RVA fallback. */
#define VIEW_FORWARD_RVA       0x1a6ac60u  /* ViewForward FUN_141a6ac60 -- camera aim dir from editor+0x17c angles */
#define MODULE_CONTAIN_XFORM_RVA 0x5546b0u /* ModuleContainTransform FUN_1405546b0 -- world->module-local (OBB frame) */
#define SEG_VS_AABB_RVA        0x1a60c20u  /* SegVsAabb FUN_141a60c20 -- segment/point vs module OBB */
#define MODULE_W2L_FULL_RVA    0x554620u   /* ModuleWorldToLocal FUN_140554620 -- full 3x4 world->module-local */
#define SET_OWNING_MODULE_RVA  0x544be0u   /* SetOwningModule FUN_140544be0 -- entity+0x338 owning-module obj */
#define ENTITY_FINALIZE_RVA    0x544c00u   /* EntityFinalize FUN_140544c00 -- finalize a new module entity */

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
#define APPLY_TEXT_CAP         (256 * 1024) /* max patched-entity JSON we accept per item (sanity) */
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
typedef void  (*buffer_cmd_fn)(void *cmdSys, const char *text);                       /* BufferCommandText 0x1aa3780 */
typedef void  (*add_command_fn)(void *cmdSys, const char *name, void *cb, void *p3,
                                const char *help, unsigned int flags);                /* AddCommand 0x1aa3630 */
typedef void  (*prefab_ctor_fn)(void *self);                                          /* PrefabCtor 0x54d0a0 */
typedef char  (*prefab_populate_fn)(void *self, void *editor);                        /* PrefabPopulate 0x54e410 */
typedef void  (*prefab_dtor_fn)(void *self);                                          /* PrefabDtor 0x51d870 */
typedef void  (*paste_instantiate_fn)(void *staging, void *editor);                   /* PasteInstantiate 0x54f950 */
typedef void *(*world_to_local_fn)(void *modtbl, float *out, int mi, const float *world); /* WorldToModuleLocal 0x5a8be0 */
typedef float *(*view_forward_fn)(const void *angles, float *out);                        /* ViewForward 0x1a6ac60 */
typedef void  (*module_contain_xform_fn)(const void *tblEntry, float *out, const float *world); /* ModuleContainTransform 0x5546b0 */
typedef char  (*seg_vs_aabb_fn)(const float *aabb6, const float *a, const float *b);      /* SegVsAabb 0x1a60c20 */
typedef void  (*module_w2l_full_fn)(const void *E, float *out12, const float *world12);   /* ModuleWorldToLocal 0x554620 */
typedef void  (*set_owning_module_fn)(void *entBody, void *moduleObj);                    /* SetOwningModule 0x544be0 */
typedef void  (*entity_finalize_fn)(void *entBody, void *moduleObj);                      /* EntityFinalize 0x544c00 */

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
static buffer_cmd_fn       g_buffer_cmd  = NULL;
static add_command_fn      g_add_command = NULL;
static prefab_ctor_fn      g_prefab_ctor     = NULL;   /* +0xb0 serialize-selection */
static prefab_populate_fn  g_prefab_populate = NULL;
static prefab_dtor_fn      g_prefab_dtor     = NULL;
static paste_instantiate_fn g_paste_instantiate = NULL;  /* create-from-scratch SPAWN: place staged prefab */
static world_to_local_fn   g_world_to_local = NULL;      /* world->module-local re-base for the birth-in-module SPAWN */
static view_forward_fn     g_view_forward = NULL;        /* camera aim direction (spatial spawn-module resolve) */
static module_contain_xform_fn g_module_contain_xform = NULL; /* world->module-local for OBB containment */
static seg_vs_aabb_fn      g_seg_vs_aabb = NULL;         /* point/segment-in-module-OBB test */
static module_w2l_full_fn  g_module_w2l_full = NULL;     /* full 3x4 world->module-local (native-birth position) */
static set_owning_module_fn g_set_owning_module = NULL;  /* set entity+0x338 owning-module obj (settled member) */
static entity_finalize_fn  g_entity_finalize = NULL;     /* finalize a just-created module entity */
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
    if (!ae_deserialize_bound() || !text || !dstObj) return 0;
    void *reflect = ae_get_reflect();
    if (!reflect) return 0;

    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t lexer[LEXER_SIZE];
    uint8_t parseNode[PARSE_NODE_SIZE];
    uint8_t srcStr[IDSTR_SIZE];
    int node7_ctored = 0, lex_ctored = 0, pn_ctored = 0, src_ctored = 0;
    int ok = 0;

    __try {
        g_node_ctor(node7, 7);          node7_ctored = 1;  /* the {tag} arg5 sub-node (kind 7) */
        g_lex_ctor(lexer);              lex_ctored = 1;    /* idLexer ctor (constructs +0x30/+0x88 idStrs) */
        g_node_ctor(parseNode, 0);      pn_ctored = 1;     /* the tree the lexer fills + deser reads (kind 0) */
        g_idstr_ctor(srcStr, text);     src_ctored = 1;    /* src idStr from the C string */

        /* 0x1a5cd90(lexer, src, parseNode, 1) -> success char. */
        char lexed = g_lexer(lexer, srcStr, parseNode, 1);
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
            g_deser(reflect, typeName, dstObj, parseNode, arg5, NULL);
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }

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

    uint8_t tmpDef[TEMP_DEF_SIZE];
    int def_ctored = 0, applied = 0;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;                /* 0x5e9400 */
        /* STEP 3 (deserialize the full modified entity onto the temp -> survives, it is a valid entity). */
        if (ae_deserialize_to_obj(patched_text, tmpDef, "idSnapEntity")) {
            /* STEP 4 commit (FUN_180004b80 tail). */
            void *srcPtr = *(void * const *)(tmpDef + TDEF_SOURCE_OFF);   /* normalized source-text ptr */
            void *clsPtr = *(void * const *)(tmpDef + TDEF_CLASS_OFF);    /* normalized classname idStr-data */
            void *inhPtr = *(void * const *)(tmpDef + TDEF_INHERIT_OFF);  /* normalized inherit idStr-data */
            /* the source rebuild carries the EDIT (the temp's canonical source includes the leaf) -- always. */
            g_decl_rebuild(defsub, (const char *)srcPtr, 1);             /* 0x17ae560 */
            /* class/inherit: with a full entity these are real values; null -> skip (defensive). */
            if (clsPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF,   (const char *)clsPtr);
            if (inhPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, (const char *)inhPtr);
            applied = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = 0;
    }
    if (def_ctored) { __try { g_def_dtor(tmpDef); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return applied;
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
    return ae_deserialize_to_obj(prefab_text, staging, "idSnapEntityPrefab");
}

/* the entity's module index (an inline of the iface reader): the engine stores each entity's instance(module)
 * index at *(*(lm+0x6f0)+id*4); a global/no-module entity carries instanceIdx == the module COUNT (lm+0x758).
 * Returns the real module index, or -1 for global. SEH-guarded -- a stale offset yields -1, never a crash. */
static int ae_id_module_index(const uint8_t *lm, uint32_t id)
{
    void *idxArr = NULL; int instIdx = 0, modCnt = 0;
    if (!ae_read_ptr(lm + LM_ENTINST_ARR_OFF, &idxArr) || idxArr == NULL) return -1;
    if (!ae_read_u32_safe((const uint8_t *)idxArr + (size_t)id * 4, &instIdx)) return -1;
    if (!ae_read_u32_safe(lm + LM_INSTANCES_CNT_OFF, &modCnt) || modCnt <= 0) return -1;
    return (instIdx >= 0 && instIdx < modCnt) ? instIdx : -1;   /* instIdx == modCnt => global */
}

/* Which module contains a WORLD point -- the engine's own placement test, replicated (editor pick FUN_14059a520):
 * iterate the module transform table (stride 0x98, count lm+0x758), transform the point into each module's local
 * OBB frame (ModuleContainTransform), and point-test it against the module OBB at modObj+0xa0 (SegVsAabb a==b).
 * Returns the module index, or -1 (outside all modules => global). SEH-guarded per module; the engine calls run
 * inside the guard, so a bad offset falls through to -1, never a crash. No-op (-> -1) unless both engine fns resolved. */
static int ae_module_containing_world(const void *lm, const float world[3])
{
    if (!g_module_contain_xform || !g_seg_vs_aabb) return -1;
    int modCnt = 0;
    void *tblBase = NULL;
    if (!ae_read_u32_safe((const uint8_t *)lm + LM_INSTANCES_CNT_OFF, &modCnt) || modCnt <= 0) return -1;
    if (!ae_read_ptr((const uint8_t *)lm + LM_MODXFORM_OFF, &tblBase) || tblBase == NULL) return -1;
    for (int m = 0; m < modCnt && m < 256; m++) {
        const uint8_t *E = (const uint8_t *)tblBase + (size_t)m * MOD_TBL_STRIDE;
        void *p1 = NULL, *modObj = NULL;
        if (!ae_read_ptr(E, &p1) || p1 == NULL) continue;             /* *(E) */
        if (!ae_read_ptr(p1, &modObj) || modObj == NULL) continue;    /* modObj = *(*(E)) */
        int hit = 0;
        __try {
            if (*(const volatile char *)(E + MOD_TBL_VALID_OFF) != 0) {
                float local[3] = { 0.0f, 0.0f, 0.0f };
                g_module_contain_xform((const void *)E, local, world);   /* world -> module-m-local */
                if (g_seg_vs_aabb((const float *)((const uint8_t *)modObj + MOD_OBB_OFF), local, local))
                    hit = 1;                                             /* point inside module m's OBB */
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { hit = 0; }
        if (hit) return m;
    }
    return -1;   /* outside all modules -> global */
}

/* The world point the create-from-scratch SPAWN lands at, matching PasteInstantiate: camera origin (editor+0x170)
 * plus the view forward (ViewForward on editor+0x17c) times the staged prefab's grab distance (staging+0x24). The
 * prefab MUST already be staged (ae_mkcmd_one ran). Returns 1 + fills out[3] on success. SEH-guarded. */
static int ae_compute_grab_point(const uint8_t *ed, float out[3])
{
    if (!g_view_forward) return 0;
    __try {
        float fwd[3] = { 0.0f, 0.0f, 0.0f };
        g_view_forward((const void *)(ed + ED_CAMERA_ANGLES_OFF), fwd);
        const float *cam = (const float *)(ed + ED_CAMERA_ORIGIN_OFF);
        float dist = *(const float *)(ed + PASTE_STAGING_OFF + PREFAB_GRABDIST_OFF);
        out[0] = cam[0] + fwd[0] * dist;
        out[1] = cam[1] + fwd[1] * dist;
        out[2] = cam[2] + fwd[2] * dist;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* the module a create-from-scratch SPAWN should belong to. Module membership is SPATIAL (there is no stored
 * "active module"), so resolve it from WHERE the entity spawns: the camera-aim grab point -> the module whose OBB
 * contains it (exactly how native placement decides). Order: single-module map -> 0; else the spatial pick; else
 * the last looked-at entity's module (a cheap corroborator); else -1 => leave it GLOBAL (always save-safe). A
 * wrong/absent module only affects the organizational bucket + ID label, never correctness (position is re-based
 * to whichever bucket M it lands in). */
static int ae_active_module_for_spawn(const uint8_t *ed, const void *lm)
{
    int modCnt = 0;
    if (!ae_read_u32_safe((const uint8_t *)lm + LM_INSTANCES_CNT_OFF, &modCnt) || modCnt <= 0) return -1;
    if (modCnt == 1) return 0;                          /* single-module map: the only module */
    float grab[3];
    if (ae_compute_grab_point(ed, grab)) {              /* SPATIAL: the module the spawn point lands in */
        int m = ae_module_containing_world(lm, grab);
        if (m >= 0) return m;
    }
    void *sel = NULL;                                   /* fallback: the last looked-at entity's module */
    if (ae_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) && sel) {
        int hov = -1;
        if (ae_read_u32_safe((const uint8_t *)sel + SEL_HOVERED_OFF, &hov) && hov >= 0)
            return ae_id_module_index((const uint8_t *)lm, (uint32_t)hov);
    }
    return -1;                                          /* leave it global (always save-safe) */
}

/* Finalize the just-spawned (now-selected) entities as proper MODULE members -- a faithful replica of native
 * module-entity birth (FUN_1405949a0), which does per entity: register into M -> set +0x288 module-LOCAL ->
 * SetOwningModule (+0x338) -> Finalize. Our temp-swap already registered into bucket M and PasteInstantiate left
 * a WORLD transform at +0x288; here we (1) convert that world transform to the module's LOCAL frame with the
 * engine's OWN canonical converter ModuleWorldToLocal (FUN_140554620, full 3x4), (2) set the owning-module OBJECT
 * back-ref +0x338 via SetOwningModule (FUN_140544be0), (3) Finalize via FUN_140544c00. Steps 2+3 are what EVERY
 * native module entity has and our earlier attempts OMITTED -- without them the engine's per-entity refresh
 * (FUN_140595fb0) re-derives +0x288 and clobbers any hand-computed position (why attempts 1-3 all misplaced even
 * when the local math was right). With them the entity is a "settled member" indistinguishable from a native
 * module entity, so it renders + saves at the correct in-module position. moduleObj = *(*(E)) (the module object
 * whose OBB the spatial pick reads). The SPAWN branch had nothing selected pre-paste, so the selection now IS the
 * new entity set. All reads SEH-guarded. No-op unless anchored to a real module AND the engine fns resolved. */
static void ae_finalize_module_birth(const uint8_t *ed, void *lm, int M)
{
    if (!g_module_w2l_full || !g_set_owning_module || !g_entity_finalize || M < 0) return;
    void *sel = NULL, *ids = NULL, *arr = NULL, *tblBase = NULL, *p1 = NULL, *modObj = NULL;
    int count = 0;
    if (!ae_read_ptr((const uint8_t *)lm + LM_MODXFORM_OFF, &tblBase) || tblBase == NULL) return;
    const uint8_t *E = (const uint8_t *)tblBase + (size_t)M * MOD_TBL_STRIDE;   /* module M's transform-table entry */
    if (!ae_read_ptr(E, &p1) || p1 == NULL) return;
    if (!ae_read_ptr(p1, &modObj) || modObj == NULL) return;                    /* moduleObj = *(*(E)) */
    if (!ae_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) || sel == NULL) return;
    if (!ae_read_u32_safe((const uint8_t *)sel + SEL_COUNT_OFF, &count) || count <= 0) return;
    if (!ae_read_ptr((const uint8_t *)sel + SEL_IDS_OFF, &ids) || ids == NULL) return;
    if (!ae_read_ptr((const uint8_t *)lm + ARR_ENT_ARRAY_OFF, &arr) || arr == NULL) return;
    for (int i = 0; i < count && i < 64; i++) {
        int id = -1;
        if (!ae_read_u32_safe((const uint8_t *)ids + (size_t)i * 4, &id) || id < 0) continue;
        void *ent = NULL;
        if (!ae_read_ptr((const uint8_t *)arr + (size_t)id * 8, &ent) || ent == NULL) continue;
        __try {
            float world[12], local[12];
            memcpy(world, (const uint8_t *)ent + ENT_XFORM_OFF, sizeof world);   /* the world grab transform */
            g_module_w2l_full((const void *)E, local, world);                    /* full 3x4 world -> module-M-local */
            memcpy((uint8_t *)ent + ENT_XFORM_OFF, local, sizeof local);          /* store the module-local transform */
            *(volatile uint32_t *)((uint8_t *)ent + ENT_DIRTY_OFF) |= 3u;         /* transform dirty (as FUN_140545040) */
            g_set_owning_module(ent, modObj);   /* +0x338 = moduleObj + dirty|=0x20 -- the "settled member" mark */
            g_entity_finalize(ent, modObj);     /* finalize +0x350 + dirty|=0x20 (last step of native birth) */
            { char ln[168]; _snprintf_s(ln, sizeof ln, _TRUNCATE,
                "create-timeline: native-birth id=%d M=%d world=(%.1f,%.1f,%.1f) -> local=(%.1f,%.1f,%.1f) +modobj+finalize",
                id, M, (double)world[0], (double)world[1], (double)world[2],
                (double)local[0], (double)local[1], (double)local[2]); backend_log(ln); }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("create-timeline: native-birth SEH (entity finalize)"); }
    }
}

/* kind=2: stage the prefab into editor+0x209a8 (like mkcmd) THEN paste-INSTANTIATE it (engine PasteInstantiate
 * FUN_14054f950) so it is actually PLACED in the live map -- the create-from-scratch SPAWN. Plain mkcmd stays
 * kind=1 (stage-only: the user Ctrl+V's it). PasteInstantiate reads editor+0x209a8, instantiates + registers
 * each entity + AddToSelection's it, at a camera-relative grab transform; it does NOT consume the slot, so we
 * call it ONCE per staged prefab.
 *
 * BIRTH-IN-MODULE: a normally-placed entity is registered into the module it is dropped in; PasteInstantiate
 * instead files into the GLOBAL bucket, whose index is read from *(lm+0x758) (the module count) inside the
 * register wrapper -- the SOLE reader of that field during the entire paste. So to make the SPAWN land in the
 * active module M we point *(lm+0x758) at M for the duration of the call and restore it immediately after
 * (swap-safe: nothing else in the paste reads +0x758). PasteInstantiate then places at a WORLD grab transform,
 * but a module entity is stored module-LOCAL AND must be a "settled member" (owning-module back-ref +0x338 +
 * finalize) or the engine's per-entity refresh re-derives + clobbers its +0x288 -- so afterward we replicate
 * native module birth via ae_finalize_module_birth (world->local + SetOwningModule + Finalize). M<0 / out-of-range
 * => the spawn stays GLOBAL, unchanged (no finalize).
 * Class-independent: ANY spawned inherit/classname is anchored, not just timelines. SEH-guarded (a garbage slot /
 * map-not-loaded would AV in the engine); the restore always runs. Returns 1 iff staged AND instantiate fired. */
static int ae_mkcmd_instantiate_one(const char *prefab_text)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed || !prefab_text) return 0;
    if (!ae_mkcmd_one(prefab_text)) return 0;          /* stage the clean prefab into editor+0x209a8 first */
    if (!g_paste_instantiate) { backend_log("create-timeline: PasteInstantiate unresolved -- staged only"); return 0; }
    void *staging = (void *)(ed + PASTE_STAGING_OFF);

    void *lm = NULL;
    if (!ae_read_ptr(ed + ED_MAP_OBJ_OFF, &lm) || lm == NULL) { backend_log("create-timeline: no loaded map"); return 0; }
    int M = ae_active_module_for_spawn(ed, lm);
    volatile uint32_t *modcnt = (volatile uint32_t *)((uint8_t *)lm + LM_INSTANCES_CNT_OFF);
    uint32_t saved = 0;
    int anchor = (ae_read_u32((const void *)modcnt, &saved) && M >= 0 && (uint32_t)M < saved);

    int ok = 0;
    if (anchor) *modcnt = (uint32_t)M;                 /* the register wrapper reads THIS as the bucket key */
    __try { g_paste_instantiate(staging, (void *)ed); ok = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("create-timeline: PasteInstantiate SEH (slot/map not ready?)"); ok = 0; }
    if (anchor) *modcnt = saved;                       /* ALWAYS restore the true module count */
    if (anchor && ok) ae_finalize_module_birth(ed, lm, M);  /* make it a proper module member (local xform + owning-module + finalize) */

    char line[96];
    _snprintf_s(line, sizeof line, _TRUNCATE, "create-timeline: instantiate module=%d (anchor=%d)", M, anchor);
    backend_log(line);
    return ok;
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
    if (iface && iface->vtbl && iface->vtbl->toast)
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
        int ok = (items[i].kind == 2) ? ae_mkcmd_instantiate_one(items[i].text)
               : (items[i].kind == 1) ? ae_mkcmd_one(items[i].text)
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
        char populated = g_prefab_populate(prefab, (void *)ed);   /* fill from editor selection */
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

/* ============================================================ slot export + install ================ */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab)
{
    if (serialize_entity) *serialize_entity = slot_serialize_entity;
    if (apply_edit)       *apply_edit       = slot_schedule_apply;
    if (read_prefab)      *read_prefab      = slot_read_prefab;
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
    /* PasteInstantiate (create-from-scratch SPAWN): sig-resolved (portable) with the known_rva hook-tolerant
     * fallback baked into the sig DB; the RVA fallback below covers a clean scan-miss on this build. */
    g_paste_instantiate = (paste_instantiate_fn) sig_addr_by_name(results, n, "PasteInstantiate");
    g_world_to_local    = (world_to_local_fn)    sig_addr_by_name(results, n, "WorldToModuleLocal");
    g_view_forward      = (view_forward_fn)      sig_addr_by_name(results, n, "ViewForward");
    g_module_contain_xform = (module_contain_xform_fn) sig_addr_by_name(results, n, "ModuleContainTransform");
    g_seg_vs_aabb       = (seg_vs_aabb_fn)       sig_addr_by_name(results, n, "SegVsAabb");
    g_module_w2l_full   = (module_w2l_full_fn)   sig_addr_by_name(results, n, "ModuleWorldToLocal");
    g_set_owning_module = (set_owning_module_fn) sig_addr_by_name(results, n, "SetOwningModule");
    g_entity_finalize   = (entity_finalize_fn)   sig_addr_by_name(results, n, "EntityFinalize");

    /* the prefab-from-selection serialize engine fns (+0xb0). These jumptable/inline-prone leaves
     * resolve by FALLBACK RVA off module_base (re-derive-tagged like the editor singleton); a wrong/shifted
     * offset just makes the serialize SEH-fail -> a clean 0-length result, never a crash. */
    if (module_base) {
        g_prefab_ctor     = (prefab_ctor_fn)    (module_base + PREFAB_CTOR_RVA);
        g_prefab_populate = (prefab_populate_fn)(module_base + PREFAB_POPULATE_RVA);
        g_prefab_dtor     = (prefab_dtor_fn)    (module_base + PREFAB_DTOR_RVA);
        if (!g_paste_instantiate)   /* sig scan missed -> this-build RVA fallback (re-derive-tagged) */
            g_paste_instantiate = (paste_instantiate_fn)(module_base + PASTE_INSTANTIATE_RVA);
        if (!g_world_to_local)      /* sig scan missed -> this-build RVA fallback (re-derive-tagged) */
            g_world_to_local = (world_to_local_fn)(module_base + WORLD_TO_LOCAL_RVA);
        if (!g_view_forward)        g_view_forward = (view_forward_fn)(module_base + VIEW_FORWARD_RVA);
        if (!g_module_contain_xform) g_module_contain_xform = (module_contain_xform_fn)(module_base + MODULE_CONTAIN_XFORM_RVA);
        if (!g_seg_vs_aabb)         g_seg_vs_aabb = (seg_vs_aabb_fn)(module_base + SEG_VS_AABB_RVA);
        if (!g_module_w2l_full)     g_module_w2l_full = (module_w2l_full_fn)(module_base + MODULE_W2L_FULL_RVA);
        if (!g_set_owning_module)   g_set_owning_module = (set_owning_module_fn)(module_base + SET_OWNING_MODULE_RVA);
        if (!g_entity_finalize)     g_entity_finalize = (entity_finalize_fn)(module_base + ENTITY_FINALIZE_RVA);
    }

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 wave B: apply-engine install -- ser=%d deser=%d commit=%d cmdsys=%p buf_cmd=%p add_cmd=%p",
        ae_serialize_bound(), ae_deserialize_bound(), ae_commit_bound(),
        g_cmdsys, (void *)g_buffer_cmd, (void *)g_add_command);
    backend_log(line);
    return ae_serialize_bound() && ae_deserialize_bound() && ae_commit_bound();
}
