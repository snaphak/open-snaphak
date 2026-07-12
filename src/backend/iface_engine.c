/* iface_engine.c -- see iface_engine.h. The BACKEND engine-touch bodies for the UI-interface vtable
 * the LIGHT engine touches the SnapStack STORE-ops need.
 *
 * Faithful port of the reference implementation's editor bridge (the live-proven mechanism):
 *   - editor singleton  = *module_base + 0x3056748* is the INLINE idSnapEditorLocal object (NOT a ptr;
 *     the editor-singleton RVA (see the re-derive recipe) -- the OBJECT, in-place ctor 0x51A8E0). A hardcoded
 *     DATA RVA, exactly like cmdSystem/cvarSystem (non-unique data global, not sig-able).
 *   - selection object  = editor+0x204d0 (ptr); ids @ sel+0x80, count @ sel+0x88; hovered @ sel+0x2c.
 *   - screen (toast)    = editor+0x21088 (ptr; Toast arg0).
 *   - loaded-map / entity array = *(editor+0x204c8); entity-ptr array @ arrObj+0x6a0, count @ arrObj+0x6a8.
 *   - entity[id] valid  = entity[id]+8 != 0; def-subobj = entity[id]+0x158.
 *   - classname (filtcls): *(ent+8)->+0x1c8->+0x38 decl-SOURCE blob, parse `class = "..."`.
 *   - inherit  (filtinh): *(ent+0x158)->+0x38 decl-SOURCE blob, parse `inherit = "..."`.
 *   - id-string (id_to_string / mkcmd): the entity name -- not byte-captured on this build, so we fall
 *     back to the decimal id (faithful per the reference implementation entityIdString; the serialize-name path is bound later).
 * The ENGINE FUNCTIONS (AddToSelection 0x59f210 / ClearSelection 0x59fa00 / Toast 0xcfa0b0 + the idStr
 * ctor/dtor for the toast args) are resolved by SIGNATURE from the shared sig DB -- never a hardcoded RVA.
 *
 * EVERY editor deref is SEH-guarded + non-null gated; a wrong/shifted-build offset degrades to a clean
 * no-op (empty selection / push-0 / "" classname), never a crash. Clean-room; zero OG SnapHak bytes.
 */
#include <windows.h>
#include <shlobj.h>            /* SHGetFolderPathA -- the +0xc0 prefab path resolver (OG FUN_18000ce50) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snaphak_iface.h"
#include "iface_engine.h"
#include "apply_engine.h"   /* the heavy slots (serialize/schedule-apply/read-prefab) */
#include "signatures.h"
#include "backend_log.h"
#include "typeinfo.h"       /* sh_typeinfo_class_derives + the LIVE registry walks (collect_records/inherits) */
#include "valid_class_map.h" /* SH_VCM_* -- the class-dropdown static snapshot (used only if the live walk fails) */
#include "wiring_cleandirect.h" /* sh_wiring_cleandirect_generation -- the wire-any connect-edit counter (+0x288) */

/* ---- editor-struct field offsets (this-live-build; ported from the reference implementation, SEH-guarded) ------------ */
/* EDITOR_SINGLETON_RVA: the INLINE idSnapEditorLocal OBJECT (NOT a pointer) at module_base + this. A
 * NON-SIG-ABLE DATA GLOBAL (a .data object, no unique code fingerprint), like cmdSystem/cvarSystem ->
 * recipe-tagged base+RVA literal. The fault-shield carries the SAME object + the SAME recipe
 * (fault_shield/engine_layout.h RVA_EDITOR_SINGLETON). RE-DERIVE per build: it is the inline
 * idSnapEditorLocal singleton, IN-PLACE-CONSTRUCTED by its ctor at 0x51A8E0 -- decompile that ctor
 * (decompile RVA 0x51A8E0 on the new build); its `this` (the rcx it writes the vtable +
 * fields through) IS this object's address; RVA = that - module_base. (RVA derived from the live
 * editor singleton; see the re-derive recipe above.) */
#define EDITOR_SINGLETON_RVA   0x3056748u   /* inline idSnapEditorLocal object = module_base + this (in-place ctor 0x51A8E0; re-derive per-build) */
#define ED_SEL_OBJ_OFF         0x204d0      /* editor+0x204d0 -> selection object ptr */
#define ED_CAMERA_ORIGIN_OFF   0x170        /* editor+0x170 -> camera-origin vec3 {x,y,z} (3 floats). DIRECT: the OG
                                             * obtains it via editor_vtable[+0xd8](editor) where the engine method
                                             * (RVA 0x5238c0, idSnapEditorLocal::GetCameraOrigin) is `return this+0x170`
                                             * -> we read/write the field DIRECT (no vtable call -> drops the slot-index
                                             * dependency = more portable). RE-DERIVE per build: read the qword at
                                             * editor_vtable(0x2049fd8)+0xd8 -> the method RVA -> decompile -> `return this+OFF`. */
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ED_SCREEN_OFF          0x21088      /* editor+0x21088 -> menu-screen object (Toast arg0) */
#define ED_ENTITY_MODE_OFF     0x23618      /* editor+0x23618 -> editor state id (1=ModuleMode top-level /
                                            * 2=EntityMode tabbed-IN-a-module). DIRECT: OG XINPUT1_3 is-EntityMode
                                            * FUN_180007f30 reads `*(int*)(DAT_18003e5c0+0x23618)==2`; the SetState
                                            * writer 0x5298A0 + resolver 0x523F50 (editor-state-machine.md). The
                                            * Create-New-Timeline gate (the feature spawns/morphs a timeline host,
                                            * valid ONLY while tabbed inside a module). RE-DERIVE per build via the
                                            * state-machine recipe. */
#define SEL_IDS_OFF            0x80         /* selObj+0x80 -> int* selected ids */
#define SEL_COUNT_OFF          0x88         /* selObj+0x88 -> int selected count */
#define SEL_HOVERED_OFF        0x2c         /* selObj+0x2c -> looked-at/hovered entity id */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
/* the loaded-map MODULE table -- for the OG Entities-list id-string "<modidx>_<modname>/<inherit>_<id>" (port of
 * FUN_180003ba0 + FUN_180003c80). All build-specific (same loaded-map object the entity array lives in; re-derive
 * per build alongside ARR_ENT_*). */
#define LM_ENTPOS_ARR_OFF      0x708        /* loaded-map+0x708 -> entity-id-by-placement-position array (u32) */
#define LM_ENTPOS_CNT_OFF      0x710        /* loaded-map+0x710 -> placement-position count (u32) */
#define LM_MODBOUND_ARR_OFF    0x720        /* loaded-map+0x720 -> per-module cumulative-position boundaries (s32, sorted) */
#define LM_MODBOUND_CNT_OFF    0x728        /* loaded-map+0x728 -> module-boundary count (s32) */
#define LM_MODTABLE_OFF        0x750        /* loaded-map+0x750 -> module table (stride 0x98) */
#define MOD_STRIDE             0x98         /* module-table entry stride */
#define MOD_NAME_OFF           0x48         /* module entry+0x48 -> module name char* */
#define LM_ENTINST_ARR_OFF     0x6f0        /* loaded-map+0x6f0 -> ptr to the AUTHORITATIVE per-entity instance(module)
                                            * index array (i32 indexed by entity id). The registrar (FUN_1405a4520)
                                            * writes *(*(lm+0x6f0)+id*4) on create; the world-builder rebuilds it from
                                            * the instanceEntities CSR on load. instanceIdx == module-COUNT is the
                                            * engine's GLOBAL/no-module sentinel. O(1) + authoritative (vs the old
                                            * position-boundary heuristic, which could mislabel a global entity).
                                            * RE'd DIRECT from our own decompile. */
#define LM_INSTANCES_CNT_OFF   0x758        /* loaded-map+0x758 -> instances(modules) COUNT (== the no-module sentinel) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid (the +0x28 rule) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object */

/* dev-layer visibility gate (DIRECT, live build DOOMx64vk.exe.unpacked.exe). The SnapMap editor gates entity
 * visibility on the `snapEdit_enableDevLayer` cvar via a per-entity LAYER BITMASK:
 *   activeMask = enableDevLayer ? (devLayerMask|1) : 1;  entity visible iff (entity->layerBits & activeMask).
 * (engine pick paths FUN_14059a520 / FUN_14059b160 + the cvar change-callback FUN_140522eb0.) The clone mirrors
 * it for its Entities/Timelines lists: hide iff (cvar off AND (layerBits & 1)==0); show-all when on.
 * RE-DERIVE per build: xref the "snapEdit_enableDevLayer" string -> the register call's cvar obj (+0x30 is its
 * int value); a pick fn reads `entity+0x160 & (enableDevLayer ? layersDecl+0x9c|1 : 1)`. */
#define ENT_LAYER_BITS_OFF     0x160        /* entity[id]+0x160 -> layer bitmask (uint) */
#define DEVL_CVAR_VALUE_OFF    0x30         /* idCVar+0x30 -> current integer value (== cvars.c value note) */
#define DEVL_CVAR_NAME_OFF     0x40         /* idCVar+0x40 -> registered name char* (== cvars.c IDCVAR_NAME_OFF) */
#define DEVL_CVARSYS_SLOT_RVA  0x55b7290u   /* *(module_base+RVA) -> idCVarSystemLocal* (documented fallback; the
                                             * portable CmdSystemLea decode lives in cvars.c sh_resolve_cvarsys) */
#define DEVL_CVARSYS_ARR_OFF   0x08         /* cvarSys+0x08 -> FULL idCVar** array */
#define DEVL_CVARSYS_CNT_OFF   0x10         /* cvarSys+0x10 -> FULL count (u32) */
#define DEVL_CVAR_LIST_CAP     100000u      /* stale-cvarSys guard */
#define DEVL_CVAR_NAME         "snapEdit_enableDevLayer"
#define ENT_DECL_OFF           0x8          /* *(ent+8) -> the entity's decl object (classname blob root) */
#define DECL_BLOB_A_OFF        0x1c8        /* declObj+0x1c8 -> ... */
#define DECL_BLOB_B_OFF        0x38         /* ...+0x38 -> the decl-SOURCE text blob ptr */
#define IDSTR_SIZE             0x30         /* sizeof(idStr) for the toast title/text temporaries */

/* ---- data-tab field offsets (this-live-build; SEH-guarded) ------------------------------------- */
#define ENT_DISPLAYNAME_LEN_OFF 0x178      /* entity[id]+0x178 -> displayname len (u32). OG FUN_180007230 */
#define ENT_DISPLAYNAME_PTR_OFF 0x180      /* entity[id]+0x180 -> displayname data ptr */
#define ENT_DISPLAYNAME_FIELD   0x170      /* entity[id]+0x170 -> displayname idStr field (SET target) */
#define DEFSUB_SRC_PTR_OFF      0x140      /* defsub+0x140 -> canonical decl-source text data ptr (vt+0x30 get) */
#define DEFSUB_SRC_LEN_OFF      0x138      /* defsub+0x138 -> canonical decl-source text len (s32) */
#define DEFSUB_CLASS_OFF        0x60       /* defsub+0x60 -> classname idStr (SET target) */
#define DEFSUB_INHERIT_OFF      0x58       /* defsub+0x58 -> inherit idStr (SET target) */
#define ED_SEL_OBJ_OFF_C3       0x204d0    /* editor+0x204d0 -> selection object (Delete guard) */

/* RemoveFromSelection (Delete, +0x130) -- engine 0x59fda0. Resolved by FALLBACK RVA off g_doom_base (a
 * jumptable-dispatch leaf the byte-sig scanner cannot reliably anchor); tagged for per-build re-derive
 * exactly like the editor singleton. RE-DERIVE: the engine call inside OG XINPUT1_3 FUN_1800073c0
 * (decompile RVA 0x73c0 on the new build) -> `(DAT_18003e120 + 0x59fda0)`. */
#define REMOVE_FROM_SEL_RVA     0x59fda0u

/* idStr::operator=(const char*) -- engine 0x19fd5f0. The displayName field (entity+0x170) is a FULL idStr
 * (len@+0x178 / data@+0x180 / allocFlags@+0x188), so it MUST be assigned with operator= (which manages the
 * SSO/heap buffer + sets len/data). It is NOT the same as IdStrAssign 0x1a03e10 -- that is the idPoolStr
 * assign: it interns the string and writes a single pooled POINTER at field+0x00, leaving len/data UNTOUCHED.
 * Using 0x1a03e10 on the displayName wrote a pool ptr over the idStr's first qword and never set len/data, so
 * the read (len@+0x178 / data@+0x180) kept seeing the old empty string -> the box never updated (the
 * 2026-06-27 "displayname doesn't save" bug). className/inherit (defsub+0x60/+0x58) ARE idPoolStr, so they
 * correctly stay on 0x1a03e10. Fallback RVA off module_base (re-derive: OG XINPUT1_3 FUN_1800072a0 [the +0x128
 * slot] -> `(DAT_18003e120 + 0x19fd5f0)(entity+0x170, data)`; decompiling 0x19fd5f0 shows the
 * len/data/realloc idStr::operator= body, distinct from 0x1a03e10's pool-ptr write). */
#define IDSTR_OPASSIGN_RVA      0x19fd5f0u

/* (+0x110 enum-decls-of-resclass): the typed decl-manager node walk -- SAME shape sh_listres
 * uses (GetDeclsOfType(typeName) -> node; array @ node+0x20, count @ node+0x28; each decl's name
 * @ *decl+8). The engine GetDeclsOfType is SIGNATURE-resolved off the shared sig DB ("GetDeclsOfType"). */
#define DECLNODE_ARRAY_OFF      0x20        /* decl-manager node -> decl-pointer array */
#define DECLNODE_COUNT_OFF      0x28        /* decl-manager node -> decl count (u32) */
#define DECL_NAME_OFF           0x08        /* decl object -> name char* (*decl + 8) */
#define DECLNODE_COUNT_CAP      (1u << 20)  /* stale-node guard (same cap sh_listres uses) */

#define SEL_MAX_IDS            65536        /* sanity cap on a selection/array count (stale-obj guard) */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

/* ---- engine fn typedefs (signature-resolved) ---------------------------------------------------- */
typedef void  (*add_to_sel_fn)(void *selObj, int id);          /* AddToSelection 0x59f210 */
typedef void  (*clear_sel_fn)(void *selObj);                   /* ClearSelection 0x59fa00 */
typedef void  (*toast_fn)(void *screen, void *titleIdStr, void *textIdStr); /* Toast 0xcfa0b0 */
typedef void *(*idstr_ctor_fn)(void *self, const char *cstr);  /* IdStrCtor 0x19fcef0 */
typedef void  (*idstr_dtor_fn)(void *self);                    /* IdStrDtor 0x19fd120 */
/* the engine setters the Entity-State Save + the Delete need (all sig-resolved off the shared
 * sig DB, except RemoveFromSelection which is a fallback-RVA re-derive leaf). */
typedef void  (*idstr_assign_fn)(void *dstField, const char *cstr);          /* IdStrAssign 0x1a03e10 (idPoolStr ptr-write: className/inherit) */
typedef void  (*idstr_opassign_fn)(void *idStrField, const char *cstr);      /* idStr::operator= 0x19fd5f0 (FULL idStr: displayName) */
typedef void  (*decl_src_rebuild_fn)(void *defsub, const char *src, int one);/* DeclSourceRebuild 0x17ae560 */
typedef void  (*remove_from_sel_fn)(void *selObj, int id);                    /* RemoveFromSelection 0x59fda0 */
typedef void *(*get_decls_fn)(const char *type_name);                         /* GetDeclsOfType (sig DB) */

/* ---- module state (resolved once at install) ---------------------------------------------------- */
static const uint8_t *g_editor   = NULL;   /* module_base + 0x3056748 (the inline editor object) */
static add_to_sel_fn  g_add_sel  = NULL;
static clear_sel_fn   g_clear_sel= NULL;
static toast_fn       g_toast    = NULL;
static idstr_ctor_fn  g_idstr_ctor = NULL;
static idstr_dtor_fn  g_idstr_dtor = NULL;
/* Entity-State engine fns */
static idstr_assign_fn    g_idstr_assign = NULL;   /* +0x78/+0x80 set className/inherit (idPoolStr fields) */
static idstr_opassign_fn  g_idstr_opassign = NULL; /* +0x128 set displayName (FULL idStr field entity+0x170) */
static decl_src_rebuild_fn g_decl_rebuild = NULL;  /* +0x40 Save-to-Decl rebuild */
static remove_from_sel_fn g_remove_sel    = NULL;  /* +0x130 Delete */
static get_decls_fn       g_get_decls     = NULL;  /* +0x110 enum-decls-of-resclass (GetDeclsOfType) */
static const uint8_t     *g_module_base   = NULL;  /* cached for the dev-layer cvar read (cvarSys RVA fallback) */
static void              *g_devlayer_cvar = NULL;  /* cached snapEdit_enableDevLayer idCVar* (lazy; dev-layer gate) */
static volatile LONG  g_installed = 0;

/* ---- SEH-guarded primitive reads (a shifted offset degrades to a clean fail, never a crash) ----- */
static int ie_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ie_read_s32(const void *src, int *out)
{
    __try { *out = *(const int *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ie_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* "editor up" guard, matching the reference implementation editorSession: the loaded-map ptr (+0x204c8) is non-null in-editor.
 * Returns the editor object base, or NULL when the editor isn't live (so every op fails CLEANLY). */
static const uint8_t *editor_session(void)
{
    if (!g_editor) return NULL;
    void *mapObj = NULL;
    if (!ie_read_ptr(g_editor + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return g_editor;
}

/* +0x88 editor-ready poll: 1 in the live editor, 0 in the HUB/menu. The OG gates the Qt window's visibility +
 * the per-frame dispatch on this -- the SnapHak Studio window opens on editor-entry and HIDES on exit.
 * NB: this must NOT use editor_session() (the loaded-map ptr +0x204c8) -- that ptr PERSISTS as a stale value
 * in the HUB after a map was loaded, so it false-positives there (the window stayed up on exit). The editor
 * SCREEN object (+0x21088, the Toast target) goes NULL on editor->HUB exit -- the reliable editor-vs-HUB
 * signal. Live A/B (window-hide diff): editor -> non-null, HUB -> null; map + selection both persist.
 * RE-DERIVE per build: snapshot the editor edit-session region (0x20400..0x21100) in-editor vs in-HUB; the
 * offset that flips non-null(editor)->null(HUB) is this (also the documented ED_SCREEN_OFF). */
static int slot_editor_ready(sh_iface *self)
{
    (void)self;
    if (!g_editor) return 0;
    void *screen = NULL;
    if (!ie_read_ptr(g_editor + ED_SCREEN_OFF, &screen)) return 0;
    return screen != NULL ? 1 : 0;
}

/* +0x1c0 IS-ENTITY-MODE: 1 when the player is TABBED INSIDE a module (EntityMode, editor+0x23618==2), else 0
 * (top-level ModuleMode is ==1). The Create-New-Timeline gate -- the feature may only spawn/morph a timeline
 * host while in a module; the Qt button grays out otherwise. OG XINPUT1_3 FUN_180007f30. Uses editor_session()
 * (= g_editor with the map-loaded guard) so it cleanly returns 0 in the HUB / off-editor / on a fault. */
static int slot_is_entity_mode(sh_iface *self)
{
    (void)self;
    const uint8_t *ed = editor_session();
    if (!ed) return 0;
    int m = 0;
    if (!ie_read_s32(ed + ED_ENTITY_MODE_OFF, &m)) return 0;
    return m == 2 ? 1 : 0;
}

/* the selection object (editor+0x204d0), or NULL. */
static void *selection_object(void)
{
    const uint8_t *ed = editor_session();
    if (!ed) return NULL;
    void *sel = NULL;
    if (!ie_read_ptr(ed + ED_SEL_OBJ_OFF, &sel)) return NULL;
    return sel;   /* may be NULL -- caller guards */
}

/* +0x00 SET the editor camera-origin vec3 (the Camera Origin X/Y/Z + Lock-Position write-back). OG FUN_1800064a0:
 * (editor_vtable[+0xd8])(editor) returns editor+0x170 -> writes 3 floats; we write the field DIRECT. SEH-guarded;
 * off-editor (editor_session NULL) -> a clean no-op. */
static void slot_set_editor_vec3(sh_iface *self, const float *xyz)
{
    (void)self;
    const uint8_t *ed = editor_session();
    if (!ed || !xyz) return;
    float *dst = (float *)((uintptr_t)ed + ED_CAMERA_ORIGIN_OFF);
    __try { dst[0] = xyz[0]; dst[1] = xyz[1]; dst[2] = xyz[2]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x08 GET the editor camera-origin vec3 (the Entity-State Camera-Origin read-sync). OG FUN_180006500. Clean
 * zeroed read off-editor / on fault. */
static void slot_get_editor_vec3(sh_iface *self, float *out_xyz)
{
    (void)self;
    if (!out_xyz) return;
    out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.0f;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    const float *src = (const float *)(ed + ED_CAMERA_ORIGIN_OFF);
    __try { out_xyz[0] = src[0]; out_xyz[1] = src[1]; out_xyz[2] = src[2]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* the entity-ptr array {array, count} off the loaded-map object; 0 on no map / fault. */
static int entity_array(void **out_array, uint32_t *out_count)
{
    const uint8_t *ed = editor_session();
    if (!ed) return 0;
    void *arrObj = NULL;
    if (!ie_read_ptr(ed + ED_MAP_OBJ_OFF, &arrObj) || arrObj == NULL) return 0;
    void    *array = NULL;
    uint32_t count = 0;
    if (!ie_read_ptr((const uint8_t *)arrObj + ARR_ENT_ARRAY_OFF, &array)) return 0;
    if (!ie_read_u32((const uint8_t *)arrObj + ARR_ENT_COUNT_OFF, &count)) return 0;
    if (array == NULL || count > ENT_COUNT_CAP) return 0;
    *out_array = array;
    *out_count = count;
    return 1;
}

/* one entity ptr by id (the 8-byte array slot); NULL if out of range / fault. */
static void *entity_ptr(void *array, uint32_t count, int id)
{
    if (id < 0 || (uint32_t)id >= count) return NULL;
    void *e = NULL;
    if (!ie_read_ptr((const uint8_t *)array + (size_t)id * 8, &e)) return NULL;
    return e;   /* may be NULL */
}

/* parse `<key> = "..."` out of a decl-source text blob into buf (SEH-guarded blob read). Returns buf on a
 * hit (empty string on a miss). The blob is a small C-string; we read it defensively then regex-lite scan. */
static const char *parse_decl_field(const void *blob_ptr_addr, const char *key, char *buf, int cap)
{
    if (cap > 0) buf[0] = '\0';
    void *blob = NULL;
    if (!ie_read_ptr(blob_ptr_addr, &blob) || blob == NULL) return buf;
    char text[1024];
    int got = 0;
    __try {
        const char *p = (const char *)blob;
        int i = 0;
        for (; i < (int)sizeof(text) - 1 && p[i]; i++) text[i] = p[i];
        text[i] = '\0';
        got = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { got = 0; }
    if (!got) return buf;

    /* find `key` then the next '"..."' after a '='. */
    const char *k = strstr(text, key);
    if (!k) return buf;
    const char *eq = strchr(k, '=');
    if (!eq) return buf;
    const char *q1 = strchr(eq, '"');
    if (!q1) return buf;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return buf;
    int len = (int)(q2 - (q1 + 1));
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    memcpy(buf, q1 + 1, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* ================================================================ the vtable slot bodies =========== */

/* +0x150 GET selection: fill out_ids[0..max) with the editor's selected ids; returns the count written. */
static int slot_get_selection(sh_iface *self, int *out_ids, int max)
{
    (void)self;
    void *sel = selection_object();
    if (!sel || !out_ids || max <= 0) return 0;
    int count = 0;
    if (!ie_read_s32((const uint8_t *)sel + SEL_COUNT_OFF, &count)) return 0;
    if (count <= 0 || count > SEL_MAX_IDS) return 0;
    void *arr = NULL;
    if (!ie_read_ptr((const uint8_t *)sel + SEL_IDS_OFF, &arr) || arr == NULL) return 0;
    int n = count < max ? count : max;
    int written = 0;
    for (int i = 0; i < n; i++) {
        /* the reference implementation readSelection reads each id as readU32 (unsigned); read it the same way for
         * lock-step fidelity, then store into the int* out-buffer (real entity ids are small
         * non-negative, so the value is identical -- this just matches the reference implementation's read width). */
        uint32_t id = 0;
        if (!ie_read_u32((const uint8_t *)arr + (size_t)i * 4, &id)) break;
        out_ids[written++] = (int)id;
    }
    return written;
}

/* +0x148 CLEAR selection (psel). */
static void slot_clear_selection(sh_iface *self)
{
    (void)self;
    void *sel = selection_object();
    if (!sel || !g_clear_sel) return;
    __try { g_clear_sel(sel); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x138 ADD to selection (popsel). */
static void slot_add_to_selection(sh_iface *self, int id)
{
    (void)self;
    void *sel = selection_object();
    if (!sel || !g_add_sel) return;
    __try { g_add_sel(sel, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x198 hovered id (phov): selObj+0x2c. <0 / fault -> -1. */
static int slot_hovered_id(sh_iface *self)
{
    (void)self;
    void *sel = selection_object();
    if (!sel) return -1;
    int hovered = -1;
    if (!ie_read_s32((const uint8_t *)sel + SEL_HOVERED_OFF, &hovered)) return -1;
    return hovered;
}

/* +0x28 IS-VALID id: entity[id]+8 != 0 (pr's validity rule). */
static int slot_is_valid_id(sh_iface *self, int id)
{
    (void)self;
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return 0;
    void *vflag = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_VALID_OFF, &vflag)) return 0;
    return vflag != NULL;
}

/* +0x10 ENTITY COUNT: the loaded-map entity array count. */
static int slot_entity_count(sh_iface *self)
{
    (void)self;
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    return (int)count;
}

/* the entity's MODULE INDEX -- AUTHORITATIVE per-entity read. The engine stores each entity's instance(module)
 * index in the array pointed to at lm+0x6f0 (registrar FUN_1405a4520 writes *(*(lm+0x6f0)+id*4) on create; the
 * world-builder rebuilds it from the instanceEntities CSR on load). A genuinely-new / orphan / global entity
 * carries instanceIdx == module-COUNT (the engine's no-module sentinel). Returns the real module index, or -1 for
 * global/no-module. O(1) -- no cache needed -- consistent in-session and post-reload, and never mislabels a global
 * entity. All reads SEH-guarded: a stale offset yields -1 (the "(no module)" form), never a crash.
 *
 * REPLACES the old position-boundary heuristic (build_module_cache: walk lm+0x708 placement positions vs lm+0x720
 * boundaries). That heuristic was a best-effort port that could assign a WRONG module to a global entity (e.g. a
 * just-spawned host showing a module-path it does not actually belong to). RE: spawn-entity-registration-re +
 * module-spatial-resolve-re proved module membership IS this per-entity instance index (the engine performs no
 * spatial fold), so reading it directly is both simpler and correct. */
static int id_module_index(const uint8_t *lm, uint32_t id)
{
    void *idxArr = NULL; int instIdx = 0, modCnt = 0;
    if (!ie_read_ptr(lm + LM_ENTINST_ARR_OFF, &idxArr) || idxArr == NULL)  return -1;
    if (!ie_read_s32((const uint8_t *)idxArr + (size_t)id * 4, &instIdx))  return -1;
    if (!ie_read_s32(lm + LM_INSTANCES_CNT_OFF, &modCnt) || modCnt <= 0)   return -1;
    return (instIdx >= 0 && instIdx < modCnt) ? instIdx : -1;   /* instIdx == modCnt => global/no-module */
}

/* +0x18 resolve id -> string (the Entities-list item + the Entity-State id box). TWO tiers:
 *  - OG module-path (port of FUN_180003c80): "<modidx>_<modname>/<inherit>_<id>" -- the module is resolved via
 *    the loaded-map module table (id_module_index = FUN_180003ba0); the module NAME is a DOUBLE deref
 *    *(*(modTable + idx*0x98) + 0x48) -- the table at +0x750 is an array of module-object PTRS, not inline
 *    structs (a single deref reads empty). Live-validated 2026-06-23: entity 56 ->
 *    "0_blank_room_4x/snapmaps/visblockers/industrial/cap_05_56".
 *  - DESCRIPTIVE fallback: "<id>: <classname>" (a bare decimal is not useful) -- the classname is read from
 *    defsub+0x60 (the live-proven slot_get_classname field), for entities with no module. Degrades to the bare
 *    decimal only if even the classname is unreadable. All SEH-guarded; a stale offset never crashes. */
static const char *slot_id_to_string(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (!buf || cap <= 0) return "";
    buf[0] = '\0';
    char clsbuf[128] = {0};   /* the entity classname (defsub+0x60) -- zero-init keeps it NUL-terminated */
    char inhbuf[160] = {0};   /* the entity inherit slug (defsub+0x58) -- BOTH the module-path tail AND the no-module form use it */
    __try {
        /* read className (defsub+0x60) + inherit (defsub+0x58) once -- the ingredients for both branches. */
        void *array = NULL; uint32_t count = 0;
        if (entity_array(&array, &count)) {
            void *ent = entity_ptr(array, count, id), *defsub = NULL, *cp = NULL, *ip = NULL;
            if (ent && ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) && defsub) {
                if (ie_read_ptr((const uint8_t *)defsub + DEFSUB_CLASS_OFF, &cp) && cp) {
                    const char *c = (const char *)cp;
                    int k = 0; for (; k < (int)sizeof(clsbuf) - 1 && c[k]; k++) clsbuf[k] = c[k];
                }
                if (ie_read_ptr((const uint8_t *)defsub + DEFSUB_INHERIT_OFF, &ip) && ip) {
                    const char *c = (const char *)ip;
                    int k = 0; for (; k < (int)sizeof(inhbuf) - 1 && c[k]; k++) inhbuf[k] = c[k];
                }
            }
        }
        /* AUTHORITATIVE module-path: id_module_index reads the engine's per-entity instance index (-1 = global). */
        const uint8_t *ed = editor_session();
        void *lm = NULL;
        if (ed && ie_read_ptr(ed + ED_MAP_OBJ_OFF, &lm) && lm) {
            int modIdx = id_module_index((const uint8_t *)lm, (uint32_t)id);
            if (modIdx >= 0) {
                /* module NAME = *(*(modTable + idx*0x98) + 0x48) -- DOUBLE deref (the table is module-object PTRs;
                 * a single deref lands mid-struct + reads empty). Live-validated 2026-06-23. */
                const char *modName = NULL;
                void *modTable = NULL, *modObj = NULL, *np = NULL;
                if (ie_read_ptr((const uint8_t *)lm + LM_MODTABLE_OFF, &modTable) && modTable &&
                    ie_read_ptr((const uint8_t *)modTable + (size_t)modIdx * MOD_STRIDE, &modObj) && modObj &&
                    ie_read_ptr((const uint8_t *)modObj + MOD_NAME_OFF, &np))
                    modName = (const char *)np;
                if (modName && modName[0]) {
                    _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%d_%s/%s_%d",
                                modIdx, modName, inhbuf[0] ? inhbuf : "NULL", id);
                    if (buf[0]) return buf;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    /* NO module (the engine's global bucket): a just-created / orphan / not-yet-saved entity -- OG-FAITHFUL (the
     * engine performs no spatial fold; a fresh entity is genuinely module-less in-session and gains its module
     * path on save). Show the inherit + id + an explicit "(no module)" so it reads as an intentional new/global
     * entity, NOT the alarming "<id>: <className>" that looked like ID corruption. RE: module-spatial-resolve-re. */
    if (inhbuf[0])      _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s_%d (no module)", inhbuf, id);
    else if (clsbuf[0]) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s_%d (no module)", clsbuf, id);
    else                _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%d (no module)", id);
    return buf;
}

/* PUBLIC wrapper over slot_id_to_string -- resolve an entity id to its full module-qualified id-string (the
 * `targets`-field ref form "<modidx>_<modname>/<inherit>_<id>"). A GLOBAL entity yields the "<inherit>_<id>
 * (no module)" form, which has no resolvable ref -- the caller must reject that. Used by the sh_target_any
 * targets-write (Fix B, apply_engine). */
const char *ie_resolve_id_string(int id, char *buf, int cap)
{
    return slot_id_to_string(NULL, id, buf, cap);
}

/* +0x48 classname (filtcls + the Entity-State read-sync): read defsub+0x60 (the className idStr's data ptr)
 * DIRECTLY, matching the OG (FUN_1800068e0 reads *(defsub+0x60)). This reflects a MORPH IMMEDIATELY (the RAW
 * field the +0x268 atomic apply / the re-assert writes), unlike the resolved-decl blob *(ent+8)+0x1c8+0x38
 * which LAGS until a decl re-resolve (declsource-rebuild-trace: the blob refreshes only on the rebuild's
 * notify, so after a morph the box would show the OLD class until a re-resolve). Same direct read
 * sh_iface_class_inherit_ok uses (live-proven this session). */
static const char *slot_get_classname(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return buf;
    __try {
        const char *cn = *(const char * const *)((const uint8_t *)defsub + DEFSUB_CLASS_OFF);
        if (cn) lstrcpynA(buf, cn, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (cap > 0) buf[0] = '\0'; }
    return buf;
}

/* +0x50 inherit (filtinh): *(ent+0x158)->+0x38 decl-source blob, parse `inherit = "..."`. */
static const char *slot_get_inherit(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return buf;
    return parse_decl_field((const uint8_t *)defsub + DECL_BLOB_B_OFF, "inherit", buf, cap);
}

/* +0x1b8 TOAST(title,text): build two idStr temporaries, call Toast(screen,title,text), free. Faithful to
 * the reference implementation showToast (the 48-byte idStr stack objects + the ctor/dtor pairing). */
static void slot_toast(sh_iface *self, const char *title, const char *text)
{
    (void)self;
    /* diagnostic: log every toast (title/text) to the backend log BEFORE the engine-side guards -- a robust,
     * non-transient signal that an op fired + its exact text (the in-game toast is too brief to screenshot, and
     * may not render at all if the editor menu-screen ptr is null in some modes). Clone-side only, not a game change. */
    {
        char _tl[256];
        _snprintf_s(_tl, sizeof _tl, _TRUNCATE, "C2 toast: \"%s\" / \"%s\"", title ? title : "", text ? text : "");
        backend_log(_tl);
    }
    if (!g_toast || !g_idstr_ctor || !g_idstr_dtor) return;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    void *screen = NULL;
    if (!ie_read_ptr(ed + ED_SCREEN_OFF, &screen) || screen == NULL) return;
    /* idStr temporaries on the stack (48 bytes each). */
    uint8_t tStr[IDSTR_SIZE], xStr[IDSTR_SIZE];
    memset(tStr, 0, sizeof tStr);
    memset(xStr, 0, sizeof xStr);
    __try {
        g_idstr_ctor(tStr, title ? title : "");
        g_idstr_ctor(xStr, text  ? text  : "");
        g_toast(screen, tStr, xStr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    /* free heap the idStr may have attached (dtor is SSO/heap-guarded). order mirrors the reference implementation (x then t). */
    __try { g_idstr_dtor(xStr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { g_idstr_dtor(tStr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ================================================================ DATA-tab slot bodies ===
 * Faithful ports of the OG XINPUT1_3 slot bodies (FUN_180007230/65b0/6a20/6ab0/72a0/6850/6ba0/6bc0/73c0).
 * Every editor deref is SEH-guarded + non-null gated -> a shifted-build offset degrades to a clean no-op. */

/* resolve entity[id]+0x158 (the def-subobj, the Save-to-Decl commit target). NULL on no map / fault. */
static void *defsub_for_id(int id)
{
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return NULL;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return NULL;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return NULL;
    return defsub;
}

/* +0x58 GET displayname (Entity-State read): entity[id]+0x178 len / +0x180 data. OG FUN_180007230. */
static const char *slot_get_displayname(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    uint32_t len = 0;
    void *data = NULL;
    if (!ie_read_u32((const uint8_t *)ent + ENT_DISPLAYNAME_LEN_OFF, &len)) return buf;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DISPLAYNAME_PTR_OFF, &data) || data == NULL) return buf;
    if (len == 0 || len > (uint32_t)(cap - 1)) len = (len > (uint32_t)(cap - 1)) ? (uint32_t)(cap - 1) : len;
    __try {
        uint32_t i = 0;
        const char *p = (const char *)data;
        for (; i < len; i++) buf[i] = p[i];
        buf[i] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    return buf;
}

/* +0x30 GET decl-source (Entity-State read, the QPlainTextEdit): defsub+0x140 data / +0x138 len. OG
 * FUN_1800065b0. The clone reads the SAME canonical decl-source the Save-to-Decl rebuild re-emits. */
static const char *slot_get_declsource(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *defsub = defsub_for_id(id);
    if (!defsub) return buf;
    int len = 0;
    void *data = NULL;
    if (!ie_read_s32((const uint8_t *)defsub + DEFSUB_SRC_LEN_OFF, &len)) return buf;
    if (!ie_read_ptr((const uint8_t *)defsub + DEFSUB_SRC_PTR_OFF, &data) || data == NULL) return buf;
    if (len <= 0) return buf;
    if (len > cap - 1) len = cap - 1;
    __try {
        const char *p = (const char *)data;
        int i = 0;
        for (; i < len; i++) buf[i] = p[i];
        buf[i] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    return buf;
}

/* +0x78 SET classname: IdStrAssign(defsub+0x60, cstr). OG FUN_180006a20 -> FUN_180004140. The OG guards a
 * non-empty string; we match (empty -> skip).
 *
 * LAYER C (crash prevention, 2026-06-22): the OG Save-to-Decl does NO compatibility check -- a class that
 * does not derive from the inherit's base type then FATALLY faults the engine's decl reparse with "Class X
 * does not derive from Y" Error(6). That Error is caught by an INNER engine handler BEFORE idCommonLocal::
 * Frame, so the fault-shield can NOT recover it (prevent-not-recover; error-dispatcher-and-recovery.md). So
 * we apply the engine's EXACT rule up front (sh_iface_class_inherit_ok): the class is accepted iff it derives
 * from the inherit decl's base type Y (sh_typeinfo_inherit_base + the validator's own type-hierarchy walk).
 * The reject is NON-fatal: log it + leave the change unchanged. An unresolvable inherit/type (fail-open) does
 * NOT block. This ALLOWS any valid class+inherit pair -- users can morph an entity into any family the engine
 * would accept (incl. cross-family + sibling morphs) -- and rejects ONLY the combos that would crash. */
/* LAYER C shared guard: would entity `id`'s class+inherit be ACCEPTED by the engine decl validator after this
 * change? `newClass`/`newInherit` are the values being set (NULL/empty = that field is unchanged -- read the
 * live value from defsub+0x60 className / defsub+0x58 inherit). The validator's rule: the className must
 * derive from the inherit decl's base type Y. Returns 1 = OK or uncertain (apply), 0 = definite "does not
 * derive" (REJECT + logged -- this is the combo that fatally faults the reparse). Fail-open on any uncertainty
 * (defsub/inherit/type unresolvable, empty). Shared by slot_set_classname (+0x78), slot_set_inherit (+0x80),
 * and ae_apply_one (the bss-apply path, which deserializes a patched JSON -> the validator). */
int sh_iface_class_inherit_ok(int id, const char *newClass, const char *newInherit)
{
    void *defsub = defsub_for_id(id);
    if (!defsub) return 1;
    /* The class + inherit this change RESULTS IN: the new value where supplied, else the entity's live value
     * (defsub+0x60 className / +0x58 inherit -- the idStr data ptrs the SET writes; slot_get_classname reads
     * BLANK on this build, reference-entity-layout-offsets-build-specific). */
    char clsbuf[256], inhbuf[256]; clsbuf[0] = '\0'; inhbuf[0] = '\0';
    const char *cls = (newClass   && newClass[0])   ? newClass   : NULL;
    const char *inh = (newInherit && newInherit[0]) ? newInherit : NULL;
    if (!cls || !inh) {
        __try {
            if (!cls) { const char *p = *(const char * const *)((const uint8_t *)defsub + DEFSUB_CLASS_OFF);
                        if (p) { lstrcpynA(clsbuf, p, (int)sizeof clsbuf); cls = clsbuf[0] ? clsbuf : NULL; } }
            if (!inh) { const char *p = *(const char * const *)((const uint8_t *)defsub + DEFSUB_INHERIT_OFF);
                        if (p) { lstrcpynA(inhbuf, p, (int)sizeof inhbuf); inh = inhbuf[0] ? inhbuf : NULL; } }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!cls || !inh) return 1;                       /* can't determine the resulting pair -> fail-open */
    char ybuf[256];
    const char *Y = sh_typeinfo_inherit_base(inh, ybuf, sizeof ybuf);   /* the inherit's required base class */
    if (!Y || !Y[0]) return 1;                        /* inherit decl not resolvable -> fail-open */
    if (strcmp(cls, Y) == 0) return 1;                /* class IS the base -> trivially derives */
    if (sh_typeinfo_class_derives(cls, Y) == 0) {     /* definite "does not derive" -> the fatal combo */
        char msg[360];
        _snprintf_s(msg, sizeof msg, _TRUNCATE,
            "B2 iface: class/inherit change REJECTED -- class '%s' does not derive from inherit '%s's base "
            "type '%s' (would fatally fault the engine decl reparse); apply skipped.", cls, inh, Y);
        backend_log(msg);
        return 0;
    }
    return 1;                                          /* derives, or uncertain -> apply */
}

static void slot_set_classname(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_assign || !cstr || !cstr[0]) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    if (!sh_iface_class_inherit_ok(id, cstr, NULL)) return;  /* LAYER C: new class vs the CURRENT inherit's base */
    __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x80 SET inherit: IdStrAssign(defsub+0x58, cstr). OG FUN_180006ab0 -> FUN_180004070. */
static void slot_set_inherit(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_assign || !cstr || !cstr[0]) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    if (!sh_iface_class_inherit_ok(id, NULL, cstr)) return;  /* LAYER C: the CURRENT class vs the NEW inherit's base */
    __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x268 (clone-extension slot 0) ATOMIC class+inherit set. The per-slot +0x78/+0x80 guards each validate ONE
 * field against the OTHER field's CURRENT (live) value -- so a cross-family MORPH (change BOTH) trips the guard
 * at the half-applied intermediate even when the FINAL pair is valid. This slot checks the FINAL pair ONCE
 * (both args non-NULL -> sh_iface_class_inherit_ok validates the pair directly, which also SIDESTEPS the
 * build-specific defsub+0x60/+0x58 live-read the single-field guards depend on), then writes BOTH idStr fields
 * directly (no per-slot guard). cls/inh NULL/empty = keep that field (degrades to the single-field semantics).
 * Returns 1 = at least one field written (applied), 0 = rejected (the fatal combo) / no map / unbound. The
 * CALLER drives the ONE +0x40 decl-rebuild after a 1, and MUST skip the rebuild on a 0 (the rejected fatal
 * pair -- else the rebuild reparses the fatal headers and re-introduces the crash the guard just prevented). */
static int slot_apply_class_inherit(sh_iface *self, int id, const char *cls, const char *inh)
{
    (void)self;
    if (!g_idstr_assign) return 0;
    void *defsub = defsub_for_id(id);
    if (!defsub) return 0;
    const char *c = (cls && cls[0]) ? cls : NULL;
    const char *h = (inh && inh[0]) ? inh : NULL;
    if (!c && !h) return 0;                                   /* nothing to do */
    if (!sh_iface_class_inherit_ok(id, c, h)) return 0;       /* the fatal combo -> reject, no write */
    int wrote = 0;
    /* inherit first, then class -- mirrors the bsincls order so the caller's single +0x40 rebuild re-emits a
     * defsub whose BOTH fields are already the new values. */
    if (h) { __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, h); wrote = 1; }
             __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (c) { __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF,   c); wrote = 1; }
             __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return wrote;
}

/* +0x128 SET displayname: IdStrAssign(entity[id]+0x170, cstr). OG FUN_1800072a0 (engine 0x19fd5f0, an idStr
 * assign-from-cstr sibling -- we reuse the sig-resolved IdStrAssign 0x1a03e10, same (idStr* dst, cstr)
 * ABI). The OG does NOT guard the string non-empty here (it assigns even an empty displayname). */
static void slot_set_displayname(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_opassign) return;   /* displayName = a FULL idStr -> operator= (0x19fd5f0), NOT the pool assign */
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return;
    __try { g_idstr_opassign((uint8_t *)ent + ENT_DISPLAYNAME_FIELD, cstr ? cstr : ""); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x40 REBUILD+SET decl-source (the Save-to-Decl route): DeclSourceRebuild(defsub, cstr, 1). OG
 * FUN_180006850 -> FUN_180003fa0 (engine 0x17ae560). Re-emits the canonical inherit/class/pool header from
 * defsub+0x58/+0x60 + appends the edit body. The OG guards defsub != 0 + a non-null source ptr. */
static void slot_rebuild_declsource(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_decl_rebuild || !cstr) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    /* DIAG (remove before release): confirm the +0x40 rebuild path ran + dump the EXACT source committed,
     * so the acctargets decl-source splice can be validated as well-formed decl syntax. This slot is shared
     * by the manual Save + class/inherit handlers too, so it also traces those. */
    {
        char dbg[512];
        int slen = (int)strlen(cstr);
        _snprintf_s(dbg, sizeof dbg, _TRUNCATE, "+0x40 rebuild id=%d srclen=%d head='%.220s'", id, slen, cstr);
        backend_log(dbg);
        if (slen > 180) {
            char dbg2[160];
            _snprintf_s(dbg2, sizeof dbg2, _TRUNCATE, "+0x40 rebuild id=%d tail='%.100s'", id, cstr + slen - 100);
            backend_log(dbg2);
        }
    }
    __try { g_decl_rebuild(defsub, cstr, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x130 REMOVE id from selection (Entities ctx-menu Delete): gated on editor+0x204d0 != 0 && id != -1.
 * OG FUN_1800073c0 -> engine 0x59fda0 (RemoveFromSelection). */
static void slot_remove_from_selection(sh_iface *self, int id)
{
    (void)self;
    if (!g_remove_sel || id == -1) return;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    void *sel = NULL;
    if (!ie_read_ptr(ed + ED_SEL_OBJ_OFF_C3, &sel) || sel == NULL) return;
    __try { g_remove_sel(sel, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x110 ENUMERATE the decls of a resource class (the Timeline-Editor constrained decl-comboboxes). The
 * frontend (FUN_18000994c port) reduces an idDecl* arg-type-name to its lowercased resource-class string
 * (e.g. "idDeclSoundShader*" -> "soundshader") and calls this. We GetDeclsOfType(res_class), walk the typed
 * decl-manager node (array @ +0x20, count @ +0x28, each decl's name @ *decl+8 -- the SAME node shape
 * sh_listres walks), and PACK the names into out_buf as consecutive NUL-terminated strings.
 * Returns 1 on success (+ *out_count set), 0 on unknown type / no decls / unresolved GetDeclsOfType (the
 * frontend then falls back to a plain string box -- faithful to the OG cVar8=='\0' branch). SEH-guarded:
 * a stale/shifted node degrades to a clean 0. */
static int slot_enum_decls_of_resclass(sh_iface *self, const char *res_class, char *out_buf, int cap,
                                       int *out_count)
{
    (void)self;
    /* REPOINTED off engine GetDeclsOfType: that is the engine's ASSET registry (idImage/idMD6Anim/...) and
     * LOGS "Unknown resource class '%s'" to the in-game console on a decl-type miss -- the spam seen when
     * clicking timeline events with decl-pointer args. The decl-combo feed is the NON-LOGGING declManager
     * decl-type enumerator in sh_typeinfo (reflect = declMgr->[+0x80]; FindByName + the instance-list walk;
     * RE: timeline-decl-resclass-re, OG XINPUT +0x100 FUN_180006eb0). `res_class` IS the decl-type
     * short-name (the frontend reduces the arg type-name to it, e.g. "sound"/"projectile"); an unknown type
     * degrades to 0 SILENTLY -> the combo stays editable, no console spam. (g_get_decls/GetDeclsOfType stays
     * resolved for any future asset-registry use but is no longer on this path.) */
    return sh_typeinfo_enum_decls_of_type(res_class, out_buf, cap, out_count);
}

/* pack a NUL string into out_buf at *pw (double-NUL-able); bumps *pw + returns 1 if it fit, else 0. */
static int vcm_pack(char *out_buf, int cap, int *pw, const char *s)
{
    int nlen = (int)strlen(s);
    if (*pw + nlen + 1 > cap - 1) return 0;          /* leave room for the trailing arena NUL */
    memcpy(out_buf + *pw, s, (size_t)nlen);
    out_buf[*pw + nlen] = '\0';
    *pw += nlen + 1;
    return 1;
}

/* ---- local derive-from-Y over the walked type records. The Qt UI thread (where these dropdowns run) cannot
 * call the engine's reflect-based derive check, so we sort the LIVE registry records by name once and chain
 * each candidate's super up the tree by bsearch -- all raw reads, thread-safe. ---- */
static int ec_rec_cmp(const void *a, const void *b)
{
    return strcmp(((const sh_ti_record *)a)->name, ((const sh_ti_record *)b)->name);
}
static const char *ec_super_of(const sh_ti_record *sorted, int n, const char *name)
{
    sh_ti_record key; key.name = name; key.super = NULL;
    const sh_ti_record *r = (const sh_ti_record *)bsearch(&key, sorted, (size_t)n, sizeof(sh_ti_record), ec_rec_cmp);
    return r ? r->super : NULL;
}
/* Does C derive from (or == ) Y, chaining via the records' super names? Bounded (128 levels). */
static int ec_derives(const sh_ti_record *sorted, int n, const char *C, const char *Y)
{
    const char *cur = C;
    for (int g = 0; cur && cur[0] && g < 128; g++) {
        if (strcmp(cur, Y) == 0) return 1;
        cur = ec_super_of(sorted, n, cur);
    }
    return 0;
}
/* the static-snapshot fallback (used only if the live registry is unreachable) -- the old corpus map. */
static int ec_fallback_valid_classes(const char *inherit, char *out_buf, int cap, int *written, int *names)
{
    const char *ey = NULL;
    for (int i = 0; i < SH_VCM_INHERIT_Y_N; i++)
        if (strcmp(SH_VCM_INHERIT_Y[i].inherit, inherit) == 0) { ey = SH_VCM_INHERIT_Y[i].y; break; }
    if (!ey) return 0;
    for (int i = 0; i < SH_VCM_Y_CLASSES_N; i++)
        if (strcmp(SH_VCM_Y_CLASSES[i].y, ey) == 0) {
            const vcm_yc *e = &SH_VCM_Y_CLASSES[i];
            for (int j = 0; j < e->n; j++) { if (!vcm_pack(out_buf, cap, written, e->classes[j])) break; (*names)++; }
            return 1;
        }
    return 0;
}

/* +0x270 (clone-extension slot 1) ENUMERATE the valid classes for `inherit` (the linked class dropdown) ->
 * packed NUL-terminated strings (double-NUL end). Resolves Y = the inherit's base className; an EMPTY or
 * unresolvable inherit -> "idEntity" (the universal entity set -- the engine accepts a class-only entity, so
 * an empty inherit admits any idEntity class, per our RE of the engine). Then walks the LIVE
 * reflection type registry and packs every className that == Y or derives from Y. COMPLETE + THREAD-SAFE: the
 * walk roots the type array via the container global on the Qt UI thread (reflect is null there), and
 * derive-from-Y chains LOCALLY via each record's super -- NO reflect-based engine call. Fallback: if the live
 * registry is unreachable, the static valid_class_map corpus snapshot. (Replaces the old SH_CLASS_UNIVERSE(412)
 * candidates + live derive-check, which returned null off the game thread -> fell back to the 70-group map.) */
static int slot_enum_valid_classes(sh_iface *self, const char *inherit, char *out_buf, int cap, int *out_count)
{
    (void)self;
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!out_buf || cap <= 1) return 0;

    char ybuf[256];
    const char *Y = NULL;
    if (inherit && inherit[0]) Y = sh_typeinfo_inherit_base(inherit, ybuf, sizeof ybuf);
    if (!Y || !Y[0]) Y = "idEntity";      /* empty/unresolvable inherit -> the universal class-only set */

    static sh_ti_record recs[SH_REGISTRY_MAX];   /* dropdown repopulate is UI-thread-serial -> static ok */
    int k = sh_typeinfo_collect_records(recs, SH_REGISTRY_MAX);
    int written = 0, names = 0;
    if (k > 0) {
        qsort(recs, (size_t)k, sizeof(sh_ti_record), ec_rec_cmp);   /* sorted -> bsearch super chain + alpha output */
        for (int i = 0; i < k; i++) {
            const char *C = recs[i].name;
            if (C && C[0] && ec_derives(recs, k, C, Y)) {           /* C==Y is handled inside ec_derives */
                if (!vcm_pack(out_buf, cap, &written, C)) break;
                names++;
            }
        }
    } else if (inherit && inherit[0]) {
        ec_fallback_valid_classes(inherit, out_buf, cap, &written, &names);   /* live registry unreachable */
    }

    out_buf[written] = '\0';               /* double-NUL end marker */
    if (out_count) *out_count = names;
    return names > 0 ? 1 : 0;
}

/* +0x278 (clone-extension slot 2) ENUMERATE the complete valid-INHERIT set -- every loaded entityDef (the
 * inherit dropdown). Raw walk of the entityDef decl manager (sh_typeinfo_collect_inherits) -> thread-safe on
 * the UI thread. Packs the decl paths (sorted + adjacent-deduped) into out_buf. Returns 0 if the manager is
 * unreachable (the frontend then keeps its static list). Replaces the frozen 272-entry inherit list with the
 * engine's full ~2,500. */
static int ec_cstr_ptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static int slot_enum_inherits(sh_iface *self, char *out_buf, int cap, int *out_count)
{
    (void)self;
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!out_buf || cap <= 1) return 0;

    static const char *names[SH_REGISTRY_MAX];   /* UI-thread-serial -> static ok */
    int k = sh_typeinfo_collect_inherits(names, SH_REGISTRY_MAX);
    if (k <= 0) return 0;
    qsort((void *)names, (size_t)k, sizeof(const char *), ec_cstr_ptr_cmp);   /* alpha + adjacent-dedup (cast: C4090) */
    int written = 0, cnt = 0;
    for (int i = 0; i < k; i++) {
        if (i > 0 && strcmp(names[i], names[i - 1]) == 0) continue;   /* dedup (sorted) */
        if (!vcm_pack(out_buf, cap, &written, names[i])) break;
        cnt++;
    }
    out_buf[written] = '\0';
    if (out_count) *out_count = cnt;
    return cnt > 0 ? 1 : 0;
}

/* +0xc0 RESOLVE prefab path: %USERPROFILE%\snaphak\<prefix><name>.json. OG FUN_18000ce50:
 * SHGetFolderPathA(CSIDL_PROFILE=0x28) + "/snaphak/" + prefix + name. `prefix` = "prefabs/" (the OG passes
 * the prefabs\ literal). The `.json` suffix is appended by the FRONTEND (matching the OG, which does
 * FUN_1800050c4(prefix,name) + FUN_1800051bc(...,".json") -- here we resolve the DIR+prefix and the caller
 * concatenates name+".json"). To keep ONE call faithful to FUN_18000ce50 (path = profile + "/snaphak/" +
 * prefix), the FRONTEND passes prefix="prefabs/" and name=<name>.json so out_path is the full file path.
 * Pure Win32 (no engine fns). Returns 1 on success. */
static int slot_resolve_prefab_path(sh_iface *self, const char *prefix, const char *name,
                                    char *out_path, int cap)
{
    (void)self;
    if (!out_path || cap <= 0) return 0;
    out_path[0] = '\0';
    char profile[MAX_PATH];
    profile[0] = '\0';
    /* SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile) -- the user-profile dir. */
    HRESULT hr = SHGetFolderPathA(NULL, 0x28 /*CSIDL_PROFILE*/, NULL, 0, profile);
    if (FAILED(hr)) return 0;
    _snprintf_s(out_path, (size_t)cap, _TRUNCATE, "%s/snaphak/%s%s",
                profile, prefix ? prefix : "", name ? name : "");
    return out_path[0] != '\0';
}

/* Lazily resolve + cache the snapEdit_enableDevLayer idCVar* by walking the cvarSys FULL list by name. Pure
 * memory reads + strcmp -> thread-safe on the Qt UI thread. Returns NULL if unreachable (caller fail-safes to
 * "not hidden"). cvarSys via the documented RVA fallback off the cached module base. */
static void *resolve_devlayer_cvar(void)
{
    if (g_devlayer_cvar) return g_devlayer_cvar;
    if (!g_module_base) return NULL;
    void *cvarSys = NULL;
    if (!ie_read_ptr(g_module_base + DEVL_CVARSYS_SLOT_RVA, &cvarSys) || cvarSys == NULL) return NULL;
    void    *arr = NULL;
    uint32_t cnt = 0;
    if (!ie_read_ptr((const uint8_t *)cvarSys + DEVL_CVARSYS_ARR_OFF, &arr) || arr == NULL) return NULL;
    if (!ie_read_u32((const uint8_t *)cvarSys + DEVL_CVARSYS_CNT_OFF, &cnt) || cnt == 0 ||
        cnt > DEVL_CVAR_LIST_CAP) return NULL;
    for (uint32_t i = 0; i < cnt; i++) {
        void *cv = NULL;
        if (!ie_read_ptr((const uint8_t *)arr + (size_t)i * 8, &cv) || cv == NULL) continue;
        void *namep = NULL;
        if (!ie_read_ptr((const uint8_t *)cv + DEVL_CVAR_NAME_OFF, &namep) || namep == NULL) continue;
        __try {
            if (strcmp((const char *)namep, DEVL_CVAR_NAME) == 0) { g_devlayer_cvar = cv; return cv; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* bad name ptr -> skip */ }
    }
    return NULL;
}

/* +0x280 (ext 3): is editor entity `id` currently HIDDEN by the dev-layer gate? 1 = hide it from the lists,
 * 0 = show. Mirrors the engine: hide iff snapEdit_enableDevLayer==0 AND (entity->layerBits & 1)==0. Every read
 * SEH-guarded; any fault / cvar-on / editor-down / unresolved-cvar -> 0 (never hide on uncertainty). */
static int slot_id_dev_layer_hidden(sh_iface *self, int id)
{
    (void)self;
    if (id < 0) return 0;
    /* cvar ON -> reveal everything (no dev-layer hiding). Unresolved cvar -> fall through (fail-safe show). */
    void *cv = resolve_devlayer_cvar();
    if (cv != NULL) {
        int enabled = 0;
        if (ie_read_s32((const uint8_t *)cv + DEVL_CVAR_VALUE_OFF, &enabled) && enabled != 0) return 0;
    }
    void    *array = NULL;
    uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    void *ent = entity_ptr(array, count, id);
    if (ent == NULL) return 0;
    uint32_t bits = 0;
    if (!ie_read_u32((const uint8_t *)ent + ENT_LAYER_BITS_OFF, &bits)) return 0;
    return (bits & 1u) == 0 ? 1 : 0;   /* not in the base layer -> dev-layer -> hidden when the cvar is off */
}

/* +0x288 ext 4: the wire-any connect-edit GENERATION counter. Bumped by wiring_cleandirect.c each time the
 * wire-any hook processes a target pick. A wire connect nets no entity-COUNT change, so the Studio entity
 * list (rebuilt only on a count change) leaves the chain's module-name labels stale until a manual refresh.
 * The UI think-loop polls THIS alongside entity_count and forces a list rebuild when it changes -- so the
 * labels auto-settle after a wire (via the wire_rebuild_frames re-scan window). */
static int slot_wire_edit_generation(sh_iface *self) { (void)self; return sh_wiring_cleandirect_generation(); }

/* ================================================================ install ========================== */

int sh_iface_engine_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    if (module_base) {
        g_editor = module_base + EDITOR_SINGLETON_RVA;   /* recipe-tagged data RVA (inline ctor 0x51A8E0; see the #define) */
        g_module_base = module_base;                     /* for the dev-layer cvar read (cvarSys RVA fallback) */
    }

    g_add_sel    = (add_to_sel_fn)sig_addr_by_name(results, n, "AddToSelection");
    g_clear_sel  = (clear_sel_fn)sig_addr_by_name(results, n, "ClearSelection");
    g_toast      = (toast_fn)sig_addr_by_name(results, n, "Toast");
    g_idstr_ctor = (idstr_ctor_fn)sig_addr_by_name(results, n, "IdStrCtor");
    g_idstr_dtor = (idstr_dtor_fn)sig_addr_by_name(results, n, "IdStrDtor");
    /* the Entity-State setters (sig-resolved) + the Delete remover (fallback RVA). */
    g_idstr_assign = (idstr_assign_fn)    sig_addr_by_name(results, n, "IdStrAssign");
    g_decl_rebuild = (decl_src_rebuild_fn)sig_addr_by_name(results, n, "DeclSourceRebuild");
    g_get_decls    = (get_decls_fn)       sig_addr_by_name(results, n, "GetDeclsOfType"); /* +0x110 */
    if (module_base) {
        g_remove_sel = (remove_from_sel_fn)(module_base + REMOVE_FROM_SEL_RVA); /* re-derive-tagged fallback */
        g_idstr_opassign = (idstr_opassign_fn)(module_base + IDSTR_OPASSIGN_RVA); /* re-derive-tagged fallback (displayName) */
    }

    /* Bind the vtable slots. We bind a body even when its engine fn is unresolved -- the body null-checks
     * the fn (toast/clear/add no-op cleanly; the pure-read slots don't need an engine fn at all). */
    sh_iface_engine_slots slots;
    memset(&slots, 0, sizeof slots);
    slots.set_editor_vec3    = slot_set_editor_vec3;          /* +0x00  (camera) */
    slots.get_editor_vec3    = slot_get_editor_vec3;          /* +0x08  (camera) */
    slots.entity_count       = slot_entity_count;
    slots.id_to_string       = slot_id_to_string;
    slots.is_valid_id        = slot_is_valid_id;
    slots.editor_ready_poll  = slot_editor_ready;            /* +0x88  (window gate) */
    slots.get_classname_copy = slot_get_classname;
    slots.get_inherit_copy   = slot_get_inherit;
    slots.add_to_selection   = slot_add_to_selection;
    slots.clear_selection    = slot_clear_selection;
    slots.get_selection      = slot_get_selection;
    slots.hovered_id         = slot_hovered_id;
    slots.is_entity_mode     = slot_is_entity_mode;          /* +0x1c0 (Create-New-Timeline gate / Qt gray-out) */
    slots.toast              = slot_toast;
    /* the DATA-tab slots (Entity-State read/write + Prefabs path + Delete). */
    slots.get_declsource_copy    = slot_get_declsource;       /* +0x30  */
    slots.rebuild_set_declsource = slot_rebuild_declsource;   /* +0x40  */
    slots.get_displayname        = slot_get_displayname;      /* +0x58  */
    slots.set_classname          = slot_set_classname;        /* +0x78  */
    slots.set_inherit            = slot_set_inherit;          /* +0x80  */
    slots.set_displayname        = slot_set_displayname;      /* +0x128 */
    slots.resolve_prefab_path    = slot_resolve_prefab_path;  /* +0xc0  */
    slots.remove_from_selection  = slot_remove_from_selection;/* +0x130 */
    /* the Timeline-Editor constrained decl-combobox enumerator (+0x110). */
    slots.enum_decls_of_resclass = slot_enum_decls_of_resclass;/* +0x110 */
    /* clone-extension: the atomic class+inherit morph. */
    slots.apply_class_inherit    = slot_apply_class_inherit;  /* +0x268 ext 0 */
    /* clone-extension: the class-dropdown enumerator. */
    slots.enum_valid_classes     = slot_enum_valid_classes;   /* +0x270 ext 1 */
    /* clone-extension: the inherit-dropdown enumerator (the complete entityDef set). */
    slots.enum_inherits          = slot_enum_inherits;        /* +0x278 ext 2 */
    /* clone-extension: the dev-layer entity-hidden query (Entities/Timelines list filter). */
    slots.id_dev_layer_hidden    = slot_id_dev_layer_hidden;  /* +0x280 ext 3 */
    /* clone-extension: the wire-any connect-edit generation counter (entity-list re-read signal). */
    slots.wire_edit_generation   = slot_wire_edit_generation; /* +0x288 ext 4 */
    /* fold in the heavy apply-chain slots (serialize entity +0xc8 / schedule-apply +0xd0 /
     * read-prefab +0xb8). sh_apply_engine_install must have run first (dllmain orders it before this) so
     * its engine fns are resolved; the slot bodies themselves null-check + degrade if a dep is missing. */
    sh_apply_engine_get_slots(&slots.serialize_entity, &slots.apply_edit, &slots.read_prefab,
                              &slots.apply_sync);   /* +0x290 SYNCHRONOUS inline apply (OG-faithful) */
    /* the +0xb0 serialize-SELECTION->prefab slot also lives in the apply engine (it needs the
     * serialize engine fns). Fold it into the same bind. */
    sh_apply_engine_get_serialize_selection(&slots.serialize_selection);
    sh_iface_bind_engine_slots(&slots);

    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2: iface engine slots bound (editor=%p add_sel=%p clear_sel=%p toast=%p idstr=%p/%p)",
        (void *)g_editor, (void *)g_add_sel, (void *)g_clear_sel, (void *)g_toast,
        (void *)g_idstr_ctor, (void *)g_idstr_dtor);
    backend_log(line);
    return 10;
}
