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
#include "snapstack.h"          /* sh_snapstack_push_ids_backend -- the SnapStack stack push (+0x2A0) */

/* from entity.c -- REUSE its build-portable gameMgr-global-slot decoder (GameMgrLea RIP-relative MOV) for
 * the MapGetter-based map DIAG below. Declared extern (not via entity.h) to avoid header coupling; the
 * decode is stateless/idempotent, so calling it here AND in the entity install is harmless (logs twice). */
extern const uint8_t *sh_resolve_gamemgr_slot(const sig_result *results, size_t n, const uint8_t *module_base);

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
/* EDITOR_VTABLE_RVA: the idSnapEditorLocal VTABLE (module_base + this). This is the DEFINITIVE editor
 * identity -- the fingerprint scan requires *E == module_base+EDITOR_VTABLE_RVA, which uniquely picks the
 * real editor (the old "any module vtable" fingerprint matched a DECOY object on the post-April-2024 build
 * -> its +0x204d0 was a menu-screen string table, not the selection -> every panel read off a bogus base ->
 * empty panels). RE-DERIVE per build: GetCameraOrigin is `lea rax,[rcx+0x170]; ret` (byte sig
 * 48 8D 81 70 01 00 00 C3) and sits at editor_vtable+0xd8; find that function, find the .rdata qword that
 * points to it (the vtable slot), EDITOR_VTABLE_RVA = (that slot - 0xd8) - module_base. This build:
 * vtable @ Ghidra 0x142360588 -> RVA 0x2360588 (GetCameraOrigin @ 0x141183600 sits at slot +0xd8). */
#define EDITOR_VTABLE_RVA      0x2360588u
/* GetCameraOrigin (lea rax,[rcx+0x170];ret) appears in TWO .rdata vtables -- 0x2360588 and 0x28796e8. One is
 * the editor's PRIMARY vtable (at editor+0), the other a related/secondary class. We accept EITHER as the
 * editor-identity tell in the pointer-indirection scan (the map+sel real-object checks then confirm). */
#define EDITOR_VTABLE_RVA2     0x28796e8u
/* EDITOR_GLOBAL_PTR_RVA: a .data slot holding the pointer to the (in-module, 8-aligned) editor object. Gives
 * INSTANT resolution -- editor = *(module_base+this) -- vs the ~2.8s pointer-scan that starves the WebView
 * message pump. Re-derive per build from the "editor via GLOBAL PTR: slot RVA=..." log line (the scan is the
 * portable fallback if this slot ever fails to validate). This build: 0x2F8B238. */
#define EDITOR_GLOBAL_PTR_RVA  0x2F8B238u
/* NEW-BUILD SAFETY GATE: on the post-April-2024 build the edit-mode ENTITY-ARRAY + SELECTION internal layout
 * is NOT yet re-derived (map+0x6a0 elements lack module vtables; sel ids/count read 0 at both 0x68/0x70 and
 * 0x80/0x88). So the entity "ids" the UI list produces are not valid engine ids -- feeding one to the engine
 * AddToSelection corrupts editor state and DOOM faults a frame later (unguardable by our SEH). Until the real
 * entity/selection offsets are confirmed live, gate the engine-MUTATING selection calls to a safe no-op. Flip
 * to 1 once verified. Read-only slots (list/count/classname) stay active -- they are SEH-safe. */
#define SH_NEWBUILD_SEL_VERIFIED 1   /* live-verified 2026-07-15: sel ids@+0x80 count@+0x88 (count matched the 5 picked; ids are valid array indices) */
/* ENTITY-WRITE gate: the edit/save/delete paths call engine WRITE fns, some via STALE hardcoded RVAs on the
 * new build -- displayname IdStrOpAssign (IDSTR_OPASSIGN_RVA 0x19fd5f0) + RemoveFromSelection
 * (REMOVE_FROM_SEL_RVA 0x59fda0) both moved; live-confirmed crashes (delete @0x59fdbf; save -> engine C++
 * throw @0x33da45). And even with the RVAs re-derived the engine may REJECT a write (the C++ throw), so the
 * whole write surface needs re-derivation + investigation. Until then, gate the write slots to a safe no-op
 * so editing can't crash DOOM. Reads + selection(add, sig-resolved) + camera stay live. Flip to 1 per write
 * path as each is re-derived + verified. */
#define SH_NEWBUILD_WRITE_VERIFIED 1   /* classname/inherit/decl setters use SIG-RESOLVED engine fns (IdStrAssign,
                                        * DeclSourceRebuild) -- correct addr on this build + validation guarded.
                                        * EXPERIMENT: enabled to see if save works once displayname is excluded. */
/* (SH_NEWBUILD_STALERVA_OK removed.) It gated the two writers that reached the engine through a stale
 * hard-coded RVA: displayname (IdStrOpAssign) and delete (RemoveFromSelection). Both are now resolved
 * WITHOUT a build constant -- IdStrOpAssign as a prologue-validated delta off the signature-resolved
 * IdStrCtor, RemoveFromSelection by its own signature (verified unique on both builds). Neither has an
 * address to be stale, so there is nothing left for the gate to protect: an unresolved function is NULL
 * and each call site already null-checks into a clean no-op. */
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
static size_t             g_module_size    = 0;     /* DOOMx64vk SizeOfImage (from PE hdr); bounds the editor scan */
static int                g_editor_verified = 0;    /* 1 once g_editor is confirmed real (boot RVA ok OR scan hit) */
/* editor-scan DIAG cascade (see sh_scan_for_editor): last scan's survivor counts per fingerprint stage. */
static unsigned g_scan_diag_wsec, g_scan_diag_vt, g_scan_diag_mode, g_scan_diag_screen,
                g_scan_diag_map, g_scan_diag_sel, g_scan_diag_full, g_scan_diag_ents;
static const uint8_t *g_scan_cand[6];   /* up to 6 full-fingerprint candidate addresses (DIAG disambiguation) */
static int            g_scan_cand_n;
static void              *g_devlayer_cvar = NULL;  /* cached snapEdit_enableDevLayer idCVar* (lazy; dev-layer gate) */
/* MapGetter-based map DIAG deps (new-build entity-array verification -- see sh_diag_dump_gmmap). */
static const uint8_t     *g_ie_gamemgr_slot = NULL; /* gameMgr global SLOT (deref lazily; non-null only in a live map/playtest) */
static void            *(*g_ie_map_getter)(void *) = NULL; /* MapGetter(gameMgr) -> the live gameMgr SnapMap */
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
static int ie_read_u16(const void *src, uint16_t *out)
{
    __try { *out = *(const uint16_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ie_read_f32x3(const void *src, float out[3])
{
    __try { out[0]=((const float*)src)[0]; out[1]=((const float*)src)[1]; out[2]=((const float*)src)[2]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* SEH-guarded C-string copy for DIAG logging (a bad/shifted ptr degrades to "" rather than crashing). */
static int ie_read_cstr(const void *src, char *dst, size_t cap)
{
    if (cap == 0) return 0;
    __try {
        const char *s = (const char *)src;
        size_t i = 0;
        for (; i + 1 < cap && s[i] != '\0'; i++) dst[i] = s[i];
        dst[i] = '\0';
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = '\0'; return 0; }
}
/* a camera-origin coord is SANE iff finite + within a generous map bound. The `> && <` form is false for
 * NaN/Inf too (all comparisons with NaN are false), so this one test rejects garbage + out-of-range in one. */
static int ie_cam_sane(const float c[3])
{
    for (int i = 0; i < 3; i++) if (!(c[i] > -1.0e7f && c[i] < 1.0e7f)) return 0;
    return 1;
}

/* ------------------------------------------------------------ editor-singleton FINGERPRINT RESOLVE ---
 * The editor object (idSnapEditorLocal) is a DATA global: no unique code byte-shape to signature. It was
 * historically a hardcoded RVA (EDITOR_SINGLETON_RVA), which the April 2024 DOOM patch relocated -> the UI
 * window never showed on the new build (slot_editor_ready read a stale address -> garbage screen ptr). Fix:
 * since the backend runs IN-PROCESS, it can FIND the object by its STRUCTURE instead of a fixed address --
 * inherently build-independent (no RVA to ever re-derive; more portable than a signature). We scan the
 * module's writable sections for the one object whose field layout matches the editor's known shape:
 *   [+0]        vtable ptr INTO the module (C++ object; GetCameraOrigin lives at vtbl+0xd8)
 *   [+0x23618]  editor state id == 1 (ModuleMode) or 2 (EntityMode)   -- only ever these in-editor
 *   [+0x21088]  screen (Toast) object ptr, non-null   -- the in-editor signal
 *   [+0x204c8]  loaded-map object ptr, non-null
 *   [+0x204d0]  selection object ptr, non-null
 * plus a hardening check that screen+map are themselves real engine objects (module vtable at their +0).
 * That 7-way fingerprint is effectively unique; the scan requires EXACTLY ONE match (ambiguous -> refuse,
 * so a false object can never be adopted). The fingerprint only matches while the editor is UP (screen/map
 * non-null), which is exactly when the UI needs the address, so the scan self-times: it finds nothing at the
 * HUB/menu and resolves the instant the editor comes up. All reads SEH-guarded (a bad candidate is skipped,
 * never a crash). Offsets are the SAME ones the rest of this file already depends on -- no new assumptions. */
static int ie_ptr_in_module(const void *p)
{
    return g_module_base && g_module_size &&
           (const uint8_t *)p >= g_module_base &&
           (const uint8_t *)p <  g_module_base + g_module_size;
}

static int editor_fingerprint_ok(const uint8_t *E)
{
    void *vt = NULL, *map = NULL, *sel = NULL, *mv = NULL, *sv = NULL; float cam[3];
    /* SEMANTIC fingerprint (engine-code-confirmed offsets). The old fingerprint's flaw was checking only
     * sel != NULL: the DECOY it adopted had a non-null +0x204d0 that pointed at a menu-screen STRING TABLE,
     * not a real selection object. Requiring BOTH map (+0x204c8) AND selection (+0x204d0) to be REAL C++
     * objects (a module vtable at their +0) rejects that decoy -- the string table's first qword is ASCII,
     * not a module pointer. Camera-origin (+0x170) finite is the final disambiguator. (mode@+0x23618 dropped:
     * that offset is unverified on this build and its check rejected the TRUE editor while the decoy passed.) */
    if (!ie_read_ptr(E, &vt) || !ie_ptr_in_module(vt)) return 0;                          /* E is a C++ object */
    if (!ie_read_ptr(E + ED_MAP_OBJ_OFF, &map) || map == NULL) return 0;
    if (!ie_read_ptr(map, &mv) || !ie_ptr_in_module(mv)) return 0;                         /* map is a real object */
    if (!ie_read_ptr(E + ED_SEL_OBJ_OFF, &sel) || sel == NULL) return 0;
    if (!ie_read_ptr(sel, &sv) || !ie_ptr_in_module(sv)) return 0;                         /* sel is a real object (rejects decoy) */
    if (!ie_read_f32x3(E + ED_CAMERA_ORIGIN_OFF, cam) || !ie_cam_sane(cam)) return 0;
    return 1;
}

/* Scan the module's WRITABLE PE sections (.data/.bss) for the one editor-fingerprint match. Returns the
 * object address, or NULL (no match yet / ambiguous). Parses the PE header for SizeOfImage + the section
 * table; all reads SEH-guarded. Cheap in practice: the vtable-in-module check (a NEAR read) rejects almost
 * every 16-byte slot before the far field reads happen. */
static const uint8_t *sh_scan_for_editor(void)
{
    if (!g_module_base) return NULL;
    uint32_t e_lfanew = 0;
    if (!ie_read_u32(g_module_base + 0x3C, &e_lfanew) || e_lfanew == 0 || e_lfanew > 0x1000) return NULL;
    const uint8_t *nt = g_module_base + e_lfanew;   /* IMAGE_NT_HEADERS ("PE\0\0") */
    uint16_t numSec = 0, optSize = 0;
    if (!ie_read_u16(nt + 6,  &numSec)  || numSec == 0 || numSec > 96) return NULL;   /* FileHeader.NumberOfSections */
    if (!ie_read_u16(nt + 20, &optSize) || optSize == 0)               return NULL;   /* FileHeader.SizeOfOptionalHeader */
    const uint8_t *secTab = nt + 24 + optSize;      /* 4 sig + 20 file-hdr + optional-hdr */

    const uint8_t *found = NULL;
    int matches = 0;
    unsigned c_wsec = 0, c_vt = 0, c_mode = 0, c_screen = 0, c_map = 0, c_sel = 0, c_full = 0, c_ents = 0;
    /* PHASE B (pointer-indirection): the editor is HEAP-allocated on the new build (the direct object scan
     * finds 56 in-module look-alikes, and the exact editor vtable has 0 in-module objects). But a GLOBAL
     * POINTER to it lives in .data/.bss. So we also walk every 8-aligned qword slot, deref it, and check the
     * TARGET's vtable == an editor vtable (0x2360588 / 0x28796e8). That uniquely finds the heap editor via
     * its global pointer -- the vtable is the definitive identity (the 56-way ambiguity of the field
     * fingerprint doesn't apply once we key on the exact vtable). We record the winning P AND its slot RVA. */
    const void *EDVT1 = (const void *)(g_module_base + EDITOR_VTABLE_RVA);
    const void *EDVT2 = (const void *)(g_module_base + EDITOR_VTABLE_RVA2);
    const uint8_t *found_ptr = NULL, *found_slot = NULL; int found_vt = 0;
    const void *ptr_cands[8]; int n_ptr = 0;   /* distinct editor objects reached via a global ptr */
    unsigned c_slots = 0, c_pnn = 0, c_pvt = 0;
    g_scan_cand_n = 0;
    for (uint16_t s = 0; s < numSec; s++) {
        const uint8_t *sec = secTab + (size_t)s * 40;   /* IMAGE_SECTION_HEADER = 40 bytes */
        uint32_t vsize = 0, vaddr = 0, chars = 0;
        if (!ie_read_u32(sec + 8,  &vsize) || vsize == 0) continue;   /* VirtualSize    */
        if (!ie_read_u32(sec + 12, &vaddr))               continue;   /* VirtualAddress */
        if (!ie_read_u32(sec + 36, &chars))               continue;   /* Characteristics */
        if (!(chars & 0x80000000u)) continue;                          /* IMAGE_SCN_MEM_WRITE only */
        c_wsec++;
        const uint8_t *start = g_module_base + vaddr;
        const uint8_t *end   = start + vsize;
        /* --- PHASE A: direct in-module object scan (16-aligned; kept for builds w/ an in-module editor). --- */
        for (const uint8_t *E = start; E + ED_SEL_OBJ_OFF + 8 <= end; E += 16) {
            void *vt = NULL, *map = NULL, *sel = NULL, *mv = NULL, *sv = NULL; float cam[3];
            if (!ie_read_ptr(E, &vt) || (vt != EDVT1 && vt != EDVT2)) continue;              c_vt++;
            if (!ie_read_ptr(E + ED_MAP_OBJ_OFF, &map) || map == NULL) continue;             c_map++;
            if (!ie_read_ptr(map, &mv) || !ie_ptr_in_module(mv)) continue;                   c_screen++;
            if (!ie_read_ptr(E + ED_SEL_OBJ_OFF, &sel) || sel == NULL) continue;             c_sel++;
            if (!ie_read_ptr(sel, &sv) || !ie_ptr_in_module(sv)) continue;                   c_full++;
            if (!ie_read_f32x3(E + ED_CAMERA_ORIGIN_OFF, cam) || !ie_cam_sane(cam)) continue; c_ents++;
            if (!found) found = E;
            if (matches < 1000000) matches++;
        }
        /* --- PHASE B: pointer-indirection scan (8-aligned; catches the heap editor via its global ptr). --- */
        for (const uint8_t *S = start; S + 8 <= end; S += 8) {
            void *P = NULL, *vt = NULL, *map = NULL, *sel = NULL, *mv = NULL, *sv = NULL;
            if (!ie_read_ptr(S, &P) || P == NULL) continue;                                  c_slots++;
            if (!ie_read_ptr(P, &vt) || (vt != EDVT1 && vt != EDVT2)) continue;              c_pnn++;
            /* DEFINITIVE identity: P's vtable is an editor vtable. dedup DISTINCT P (aliased slots share one). */
            int seen = 0;
            for (int k = 0; k < n_ptr; k++) if (ptr_cands[k] == P) { seen = 1; break; }
            if (!seen && n_ptr < (int)(sizeof ptr_cands / sizeof ptr_cands[0])) {
                ptr_cands[n_ptr++] = P;
                if (!found_ptr) { found_ptr = (const uint8_t *)P; found_slot = S; found_vt = (vt == EDVT1) ? 1 : 2; }
            }
            /* SOFT full-confirm counter (map+sel real objects) -- DIAG only, NOT required to adopt (the hit
             * may be a secondary-base subobject where +0x204c8/+0x204d0 are shifted). */
            if (ie_read_ptr((const uint8_t *)P + ED_MAP_OBJ_OFF, &map) && map &&
                ie_read_ptr(map, &mv) && ie_ptr_in_module(mv) &&
                ie_read_ptr((const uint8_t *)P + ED_SEL_OBJ_OFF, &sel) && sel &&
                ie_read_ptr(sel, &sv) && ie_ptr_in_module(sv)) c_pvt++;
        }
    }
    g_scan_diag_wsec = c_wsec; g_scan_diag_vt = c_vt; g_scan_diag_mode = c_pnn;   /* mode<-pnn (ptr w/ ed-vtable) */
    g_scan_diag_screen = c_screen; g_scan_diag_map = c_map; g_scan_diag_sel = c_pvt; /* sel<-pvt (ptr full-confirm) */
    g_scan_diag_full = c_full; g_scan_diag_ents = c_ents;
    /* prefer the pointer-indirection (heap) hit -- it is vtable-exact + map/sel-confirmed. Log the slot RVA
     * (a future build can hardcode *(module_base+slotRVA) instead of scanning). n_ptr>1 => still ambiguous. */
    if (found_ptr && n_ptr == 1) {
        char l[200];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "C2: editor via GLOBAL PTR: slot RVA=0x%llX -> editor=%p (heap) vtbl=EDVT%d mapSelConfirm=%u",
            (unsigned long long)((size_t)(found_slot - g_module_base)), (const void *)found_ptr, found_vt, c_pvt);
        backend_log(l);
        return found_ptr;
    }
    {   /* neither path uniquely resolved -- log the pointer-phase counts so the next step is clear. */
        char l[200];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "C2 ptr-scan: slots(nonnull)=%u ptrWithEdVtbl=%u ptrFullConfirm=%u distinctEditors=%d | phaseA(vtbl)=%u full=%u",
            c_slots, c_pnn, c_pvt, n_ptr, c_vt, c_ents);
        backend_log(l);
    }
    return (matches == 1) ? found : NULL;   /* else the direct in-module hit, if unique */
}

/* ONE-TIME map-object DIAG (new-build offset re-derivation): the entity array/count offsets inside the
 * loaded-map (*(editor+0x204c8)) moved on the post-April-2024 build. Scan the map for the entity-pointer
 * array -- an offset whose value points to >=4 consecutive objects that each carry a module vtable (real
 * entities) -- and log it plus the surrounding u32s (the count is one of those). Lets us read the new
 * ARR_ENT_ARRAY_OFF / ARR_ENT_COUNT_OFF straight from the log. All reads SEH-guarded; capped output. */
static void sh_diag_dump_map(const uint8_t *editor)
{
    void *map = NULL;
    char line[256];
    if (!ie_read_ptr(editor + ED_MAP_OBJ_OFF, &map) || map == NULL) { backend_log("C2 map-DIAG: no map object"); return; }
    _snprintf_s(line, sizeof line, _TRUNCATE, "C2 map-DIAG: editor RVA=0x%llX  map=%p (RVA 0x%llX)",
        (unsigned long long)((size_t)(editor - g_module_base)), map,
        ie_ptr_in_module(map) ? (unsigned long long)((const uint8_t *)map - g_module_base) : 0ull);
    backend_log(line);

    /* WIDE + RELAXED entity-array scan: an offset O whose value `arr` points to >=2 consecutive objects
     * each carrying a module vtable (real entities). For each hit, DERIVE the count by walking arr (non-null
     * entries with a module vtable, NULLs allowed, capped) and log it + the surrounding u32s -- the real
     * ARR_ENT_COUNT is whichever nearby u32 matches the walked count. */
    int logged = 0;
    for (unsigned O = 0; O <= 0x8000 && logged < 8; O += 8) {
        void *arr = NULL;
        if (!ie_read_ptr((const uint8_t *)map + O, &arr) || arr == NULL) continue;
        void *e0 = NULL, *v0 = NULL;
        if (!ie_read_ptr((const uint8_t *)arr, &e0) || e0 == NULL || !ie_read_ptr(e0, &v0) || !ie_ptr_in_module(v0)) continue;
        /* walk to derive the count -- a module vtable per slot, generous NULL-gap tolerance for a sparse
         * (by-id) array. The REAL entity array walks to many (hundreds); the 0x90-stride noise walked only 4. */
        unsigned walked = 0, gaps = 0;
        for (unsigned k = 0; k < 100000; k++) {
            void *ent = NULL, *evt = NULL;
            if (!ie_read_ptr((const uint8_t *)arr + (size_t)k * 8, &ent)) break;
            if (ent == NULL) { if (++gaps > 64) break; continue; }
            if (!ie_read_ptr(ent, &evt) || !ie_ptr_in_module(evt)) break;
            walked = k + 1;
        }
        if (walked < 16) continue;   /* filter the walked=4 struct-array noise; keep only real long arrays */
        uint32_t uM16 = 0, uM8 = 0, uP8 = 0, uP16 = 0;
        ie_read_u32((const uint8_t *)map + O - 16, &uM16);
        ie_read_u32((const uint8_t *)map + O - 8,  &uM8);
        ie_read_u32((const uint8_t *)map + O + 8,  &uP8);
        ie_read_u32((const uint8_t *)map + O + 16, &uP16);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C2 map-DIAG: LONG-arr @ map+0x%X arr=%p walked=%u | u32 [-16]=%u [-8]=%u [+8]=%u [+16]=%u",
            O, arr, walked, uM16, uM8, uP8, uP16);
        backend_log(line);
        /* identify the element TYPE: dump arr[0]'s vtable RVA + candidate entity fields (valid@+8,
         * defsub@+0x158, layer@+0x160) so we can tell the ENTITY list from module/decl lists. */
        void *ent0 = NULL, *evt0 = NULL, *f8 = NULL, *f158 = NULL, *f160 = NULL;
        if (ie_read_ptr((const uint8_t *)arr, &ent0) && ent0) {
            ie_read_ptr(ent0, &evt0);
            ie_read_ptr((const uint8_t *)ent0 + 0x8,   &f8);
            ie_read_ptr((const uint8_t *)ent0 + 0x158, &f158);
            ie_read_ptr((const uint8_t *)ent0 + 0x160, &f160);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "C2 map-DIAG:   ent0=%p vtblRVA=0x%llX [+8]=%p [+0x158]=%p [+0x160]=%p",
                ent0, (unsigned long long)(ie_ptr_in_module(evt0) ? (const uint8_t *)evt0 - g_module_base : 0),
                f8, f158, f160);
            backend_log(line);
        }
        logged++;
    }
    if (logged == 0) backend_log("C2 map-DIAG: NO long entity-array (>=16) in map[0..0x8000]");

    /* raw structural dump: the map object's first pointers + the old-offset (0x6a0) neighborhood, so the
     * layout can be reasoned about by hand if the auto-scan misses. */
    for (unsigned O = 0x00; O <= 0x60; O += 0x10) {
        void *p0 = NULL, *p1 = NULL;
        ie_read_ptr((const uint8_t *)map + O, &p0); ie_read_ptr((const uint8_t *)map + O + 8, &p1);
        _snprintf_s(line, sizeof line, _TRUNCATE, "C2 map-DIAG raw: map+0x%02X=%p  map+0x%02X=%p", O, p0, O + 8, p1);
        backend_log(line);
    }
    for (unsigned O = 0x690; O <= 0x6C0; O += 0x10) {
        void *p0 = NULL, *p1 = NULL;
        ie_read_ptr((const uint8_t *)map + O, &p0); ie_read_ptr((const uint8_t *)map + O + 8, &p1);
        _snprintf_s(line, sizeof line, _TRUNCATE, "C2 map-DIAG raw: map+0x%X=%p  map+0x%X=%p", O, p0, O + 8, p1);
        backend_log(line);
    }
}

/* ONE-TIME, FLAG-GATED dump of the DECRYPTED module image to disk (SteamStub has already decrypted .text by
 * the time the game runs), so it can be imported into Ghidra for proper DECOMPILATION-based offset
 * re-derivation -- the reliable way to read the new struct offsets straight from the engine's own code
 * (vs. ambiguous memory-scanning). Gated on %USERPROFILE%\snaphak\dump_module.flag so it only fires when
 * armed; latched so it runs at most once per process. Dumps base..+min(SizeOfImage, 0x7000000) -- ~112MB
 * covers .text/.rdata/.pdata/.data (the code + metadata); the .bss tail beyond that is just zeros. File
 * offset == RVA (dumped from base), so addresses map cleanly in Ghidra. Unreadable pages -> zero-filled. */
static void sh_diag_dump_module(void)
{
    static LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    if (!g_module_base || !g_module_size) { s_done = 0; return; }
    char *up = NULL; size_t n = 0;
    if (_dupenv_s(&up, &n, "USERPROFILE") != 0 || !up) return;
    char flag[MAX_PATH], outp[MAX_PATH];
    _snprintf_s(flag, sizeof flag, _TRUNCATE, "%s\\snaphak\\dump_module.flag", up);
    FILE *ff = NULL;
    if (fopen_s(&ff, flag, "rb") != 0 || !ff) { free(up); return; }   /* not armed -> skip */
    fclose(ff);
    _snprintf_s(outp, sizeof outp, _TRUNCATE, "%s\\snaphak\\doomx64vk_dump.bin", up);
    free(up);
    FILE *out = NULL;
    if (fopen_s(&out, outp, "wb") != 0 || !out) { backend_log("C2 module-dump: open failed"); return; }
    static uint8_t buf[0x10000];
    size_t cap = g_module_size > 0x7000000u ? 0x7000000u : g_module_size;
    size_t written = 0;
    for (size_t off = 0; off < cap; off += sizeof buf) {
        size_t chunk = (cap - off < sizeof buf) ? (cap - off) : sizeof buf;
        __try { memcpy(buf, g_module_base + off, chunk); }
        __except (EXCEPTION_EXECUTE_HANDLER) { memset(buf, 0, chunk); }
        fwrite(buf, 1, chunk, out);
        written += chunk;
    }
    fclose(out);
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 module-dump: wrote %zu bytes -> %s (module base=%p, SizeOfImage=0x%llX; file offset == RVA)",
        written, outp, (const void *)g_module_base, (unsigned long long)g_module_size);
    backend_log(line);
}

/* ONE-TIME MapGetter-based map DIAG (verifies the new-build entity-array offsets found by DECOMPILING the
 * map serializer FUN_14157b260 / per-entity FUN_14157af20). On the post-April-2024 build the entity array
 * is NOT at editor+0x204c8->+0x6a0 (that object now holds idDecl*Unlock lists); the serializer walks the
 * gameMgr SnapMap at:
 *     entity array ptr  @ map+0x48      entity count (int) @ map+0x50   (8-byte stride)   [v5 gate @ map+0x38]
 * and each entity carries:
 *     def sub-object     @ ent+0x20     (sub+0x38 = decl ptr, sub+0x40 = int flag, *(sub+0x8) = name idStr data)
 *     child-component arr @ ent+0x58     (count @ ent+0x60)
 *     property arrays     @ ent+0x08 (count +0x10)  and  ent+0xb0 (count +0xb8)   (0x30 stride)
 * This DIAG sources the map the SAME way the engine does -- MapGetter(gameMgr) -- and logs those fields for
 * the first few entities so they can be confirmed against what the OPEN editor shows. It ALSO tells us the
 * key open question: whether this serialize-array is the same entity set the editor UI/selection indexes,
 * or whether gameMgr is NULL in pure EDIT mode (only live during a playtest) -- in which case the edit-mode
 * entity list needs a different source than the gameMgr map.
 *
 * Retries cheaply (one ptr read) each editor poll while gameMgr is NULL (no live map yet); runs the actual
 * MapGetter call + dump exactly once, the first poll gameMgr is non-null. MapGetter may build a
 * DynamicSnapMap if absent -- benign, and in-editor a map already exists. All reads + the call SEH-guarded. */
static void sh_diag_dump_gmmap(void)
{
    static LONG s_done = 0;
    if (s_done) return;
    char line[320];
    if (!g_ie_map_getter || !g_ie_gamemgr_slot) {
        s_done = 1;   /* deps missing -> disable (logged once) */
        backend_log("C2 gm-map DIAG: MapGetter/gameMgr slot unresolved -- DIAG disabled");
        return;
    }
    void *gm = NULL;
    if (!ie_read_ptr(g_ie_gamemgr_slot, &gm) || gm == NULL) return;   /* no live map/playtest yet -- retry next poll */

    void *map = NULL;
    __try { map = g_ie_map_getter(gm); }
    __except (EXCEPTION_EXECUTE_HANDLER) { map = NULL; }
    s_done = 1;   /* gameMgr is live + the call is made -- one-shot from here (success or not) */
    if (map == NULL) { backend_log("C2 gm-map DIAG: MapGetter(gameMgr) returned NULL"); return; }

    void *arr = NULL; uint32_t cnt = 0; int mode = 0;
    ie_read_ptr((const uint8_t *)map + 0x48, &arr);   /* NEW-build entity-array ptr (was map+0x6a0) */
    ie_read_u32((const uint8_t *)map + 0x50, &cnt);   /* NEW-build entity count    (was map+0x6a8) */
    ie_read_s32((const uint8_t *)map + 0x38, &mode);  /* the v5 "loaded" gate MapWriter checks */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 gm-map DIAG: gm=%p map=%p (RVA 0x%llX) map+0x38=%d  map+0x48(arr)=%p  map+0x50(count)=%u",
        gm, map, ie_ptr_in_module(map) ? (unsigned long long)((const uint8_t *)map - g_module_base) : 0ull,
        mode, arr, cnt);
    backend_log(line);
    if (arr == NULL) { backend_log("C2 gm-map DIAG: entity array (map+0x48) is NULL"); return; }
    if (cnt > 200000u) { backend_log("C2 gm-map DIAG: count (map+0x50) implausible -- offset likely wrong"); return; }

    /* walk the first few entities + log the decompile-derived fields, so each can be matched to an editor entity. */
    uint32_t show = cnt < 5u ? cnt : 5u;
    for (uint32_t i = 0; i < show; i++) {
        void *ent = NULL;
        if (!ie_read_ptr((const uint8_t *)arr + (size_t)i * 8, &ent) || ent == NULL) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "C2 gm-map DIAG:  ent[%u] = NULL slot", i);
            backend_log(line); continue;
        }
        void *evt = NULL, *sub = NULL, *decl = NULL, *children = NULL, *props0 = NULL, *propsB = NULL, *nameStr = NULL;
        int subflag = 0, cCnt = 0, p0Cnt = 0, pBCnt = 0;
        char namebuf[96] = {0};
        ie_read_ptr(ent, &evt);                                    /* entity vtable (module ptr if a real object) */
        ie_read_ptr((const uint8_t *)ent + 0x20, &sub);            /* def sub-object (was +0x158) */
        ie_read_ptr((const uint8_t *)ent + 0x58, &children); ie_read_s32((const uint8_t *)ent + 0x60, &cCnt);
        ie_read_ptr((const uint8_t *)ent + 0x08, &props0);   ie_read_s32((const uint8_t *)ent + 0x10, &p0Cnt);
        ie_read_ptr((const uint8_t *)ent + 0xb0, &propsB);   ie_read_s32((const uint8_t *)ent + 0xb8, &pBCnt);
        if (sub) {
            ie_read_ptr((const uint8_t *)sub + 0x38, &decl);
            ie_read_s32((const uint8_t *)sub + 0x40, &subflag);
            if (ie_read_ptr((const uint8_t *)sub + 0x08, &nameStr) && nameStr) ie_read_cstr(nameStr, namebuf, sizeof namebuf);
        }
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C2 gm-map DIAG:  ent[%u]=%p vtblRVA=0x%llX sub(+0x20)=%p decl(+0x38)=%p flag=%d name='%s' "
            "children(+0x58)=%p/%d props0(+0x08)=%p/%d propsB(+0xb0)=%p/%d",
            i, ent, (unsigned long long)(ie_ptr_in_module(evt) ? (const uint8_t *)evt - g_module_base : 0ull),
            sub, decl, subflag, namebuf, children, cCnt, props0, p0Cnt, propsB, pBCnt);
        backend_log(line);
    }
}

/* p is a readable C++ object iff *p is a module pointer (its vtable). SEH-guarded via ie_read_ptr. */
static int ie_is_module_obj(const void *p)
{
    void *vt = NULL;
    return ie_read_ptr(p, &vt) && ie_ptr_in_module(vt);
}

/* NEW-BUILD entity-container FINDER. editor+0x204c8 is now the DECL/resource manager (its long arrays @
 * +0x45xx are decl DBs -- counts of hundreds), NOT the loaded-map; and MapGetter(gameMgr) is NULL in edit
 * mode. So the editor's live entity list hangs off a DIFFERENT editor pointer field. Find it by structure:
 * for each editor pointer field M that is a C++ object, scan M's inner fields for an (arrayPtr, count-@+8)
 * pair -- the idSnapMap layout the serializer FUN_14157b260 proved (array @ map+0x48, count @ map+0x50) --
 * whose first entries are ENTITIES: C++ objects each carrying a non-null def sub-object @ +0x20 (ent+0x20,
 * proven by the per-entity serializer FUN_14157af20). Logs the FULL path editor+<Eoff> -> M -> M+<inner> =
 * arr/count + entity[0]'s vtblRVA / subobj / def-name, so the new editor->map + array/count offsets can be
 * hardcoded. One-time; every read SEH-guarded; hits capped (decl arrays may also match -- the name= field
 * and count size disambiguate the real entity list). */
static void sh_diag_scan_editor_for_map(const uint8_t *editor)
{
    char line[320];
    backend_log("C2 ent-scan: sweeping editor pointer fields for the entity container (editor[0..0x28000])...");
    int hits = 0;
    for (unsigned Eoff = 0; Eoff <= 0x28000u && hits < 12; Eoff += 8) {
        void *M = NULL;
        if (!ie_read_ptr(editor + Eoff, &M) || M == NULL || !ie_is_module_obj(M)) continue;
        for (unsigned inner = 0x08; inner <= 0x780u && hits < 12; inner += 8) {
            void *arr = NULL; int cnt = 0;
            if (!ie_read_ptr((const uint8_t *)M + inner, &arr) || arr == NULL) continue;
            if (!ie_read_s32((const uint8_t *)M + inner + 8, &cnt) || cnt < 1 || cnt > 50000) continue;
            /* the first few array entries must be ENTITY-shaped: C++ object + non-null def-subobj @ +0x20. */
            int probe = cnt < 4 ? cnt : 4, ok = 1;
            void *e0vt = NULL, *e0sub = NULL;
            for (int k = 0; k < probe; k++) {
                void *ent = NULL, *evt = NULL, *sub = NULL;
                if (!ie_read_ptr((const uint8_t *)arr + (size_t)k * 8, &ent) || ent == NULL ||
                    !ie_read_ptr(ent, &evt) || !ie_ptr_in_module(evt) ||
                    !ie_read_ptr((const uint8_t *)ent + 0x20, &sub) || sub == NULL) { ok = 0; break; }
                if (k == 0) { e0vt = evt; e0sub = sub; }
            }
            if (!ok) continue;
            char nm[80] = {0}; void *ns = NULL;   /* entity[0]'s def name: *(sub+0x08) as a cstr */
            if (ie_read_ptr((const uint8_t *)e0sub + 0x08, &ns) && ns) ie_read_cstr(ns, nm, sizeof nm);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "C2 ent-scan HIT: editor+0x%X -> M=%p(RVA 0x%llX) M+0x%X=arr=%p count=%d | ent0 vtblRVA=0x%llX sub(+0x20)=%p name='%s'",
                Eoff, M, (unsigned long long)((const uint8_t *)M - g_module_base), inner, arr, cnt,
                (unsigned long long)((const uint8_t *)e0vt - g_module_base), e0sub, nm);
            backend_log(line);
            hits++;
        }
    }
    if (hits == 0)
        backend_log("C2 ent-scan: NO entity container matched (editor[0..0x28000] inner[0x08..0x780], count@+8, ent+0x20 subobj)");
}

/* ONE-TIME dump of the TRUE editor (now that the exact-vtable fingerprint finds it, not the decoy). Reads
 * the engine-code-confirmed fields -- map @ E+0x204c8, selection @ E+0x204d0 (ids @ sel+0x68, count @
 * sel+0x70 per the bounds methods FUN_1411836a0/37a0/3a00) -- plus entity-array CANDIDATES in the map, so
 * the remaining INTERNAL offsets (map entity array/count; sel ids/count moved 0x80/0x88 -> 0x68/0x70) can be
 * read straight from the log and hardcoded. Select a few entities first so sel count is non-zero. SEH-guarded. */
static void sh_diag_dump_true_editor(const uint8_t *E)
{
    char line[320];
    void *map = NULL, *sel = NULL, *mvt = NULL, *svt = NULL;
    ie_read_ptr(E, &mvt);   /* editor vtable (sanity) */
    ie_read_ptr(E + ED_MAP_OBJ_OFF, &map);
    ie_read_ptr(E + ED_SEL_OBJ_OFF, &sel);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 TRUE-ed: E=%p vtblRVA=0x%llX map(+0x204c8)=%p(RVA 0x%llX) sel(+0x204d0)=%p",
        (const void *)E, (unsigned long long)(ie_ptr_in_module(mvt) ? (const uint8_t *)mvt - g_module_base : 0),
        map, (unsigned long long)(ie_ptr_in_module(map) ? (const uint8_t *)map - g_module_base : 0), sel);
    backend_log(line);

    /* SELECTION: new (code) ids@+0x68 count@+0x70 vs old ids@+0x80 count@+0x88. */
    if (sel) {
        void *ids68 = NULL, *ids80 = NULL; int cnt70 = -1, cnt88 = -1;
        ie_read_ptr((const uint8_t *)sel + 0x68, &ids68); ie_read_s32((const uint8_t *)sel + 0x70, &cnt70);
        ie_read_ptr((const uint8_t *)sel + 0x80, &ids80); ie_read_s32((const uint8_t *)sel + 0x88, &cnt88);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C2 TRUE-ed: SEL svtbl? +0x68(ids)=%p +0x70(count)=%d | old +0x80(ids)=%p +0x88(count)=%d",
            ids68, cnt70, ids80, cnt88);
        backend_log(line);
        if (ids68 && cnt70 > 0 && cnt70 < 100000) {   /* dump the first selected ids (verify == what you picked) */
            int a=-1,b=-1,c=-1,d=-1;
            ie_read_s32((const uint8_t *)ids68 + 0,  &a); ie_read_s32((const uint8_t *)ids68 + 4,  &b);
            ie_read_s32((const uint8_t *)ids68 + 8,  &c); ie_read_s32((const uint8_t *)ids68 + 12, &d);
            _snprintf_s(line, sizeof line, _TRUNCATE, "C2 TRUE-ed: SEL ids[0..3]=%d,%d,%d,%d (count=%d)", a,b,c,d,cnt70);
            backend_log(line);
        }
    }

    /* MAP entity-array candidates: dump known-offset spots + SCAN map[0x08..0x1000] for a pointer-array whose
     * first entries are C++ objects (module vtable) -- the entity list. Log offset + derived count. */
    if (map) {
        void *p48=NULL,*p6a0=NULL,*p7d8=NULL; int c50=-1,c6a8=-1;
        ie_read_ptr((const uint8_t *)map + 0x48,  &p48);  ie_read_s32((const uint8_t *)map + 0x50,  &c50);
        ie_read_ptr((const uint8_t *)map + 0x6a0, &p6a0); ie_read_s32((const uint8_t *)map + 0x6a8, &c6a8);
        ie_read_ptr((const uint8_t *)map + 0x7d8, &p7d8);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C2 TRUE-ed: MAP +0x48=%p/+0x50=%d  +0x6a0=%p/+0x6a8=%d  +0x7d8=%p", p48,c50,p6a0,c6a8,p7d8);
        backend_log(line);
        int hits = 0;
        for (unsigned O = 0x08; O <= 0x1000 && hits < 8; O += 8) {
            void *arr = NULL; int cnt = 0;
            if (!ie_read_ptr((const uint8_t *)map + O, &arr) || arr == NULL) continue;
            if (!ie_read_s32((const uint8_t *)map + O + 8, &cnt) || cnt < 1 || cnt > 100000) continue;
            void *e0=NULL,*e1=NULL,*v0=NULL,*v1=NULL;
            if (!ie_read_ptr(arr, &e0) || e0==NULL || !ie_read_ptr(e0,&v0) || !ie_ptr_in_module(v0)) continue;
            ie_read_ptr((const uint8_t *)arr + 8, &e1); if (e1) ie_read_ptr(e1,&v1);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "C2 TRUE-ed: MAP arr@+0x%X=%p count(+8)=%d ent0=%p vt0RVA=0x%llX ent1=%p vt1RVA=0x%llX",
                O, arr, cnt, e0,
                (unsigned long long)(ie_ptr_in_module(v0) ? (const uint8_t *)v0 - g_module_base : 0),
                e1, (unsigned long long)(ie_ptr_in_module(v1) ? (const uint8_t *)v1 - g_module_base : 0));
            backend_log(line);
            hits++;
        }
        if (hits == 0) backend_log("C2 TRUE-ed: MAP no obvious entity ptr-array in [0x08..0x1000]");
    }
}

/* Resolve (once) + return the real editor object, or NULL if it isn't up yet. Fast path after resolution.
 * Before resolution: accept the boot RVA if it fingerprints (old build), else THROTTLED-scan for it (new
 * build) -- the scan only succeeds in-editor, so it naturally no-ops at the HUB and resolves on entry. Once
 * found, g_editor is corrected in place so every existing g_editor/editor_session user gets the right base. */
/* Public accessor -- see iface_engine.h. Exists so other translation units share THIS resolution instead
 * of each keeping a private `module_base + <editor RVA>` (apply_engine.c did exactly that, which is why
 * its editor pointer was stale on the current DOOM build while this one was correct). */
static const uint8_t *editor_base(void);   /* defined just below */
const uint8_t *sh_iface_editor(void) { return editor_base(); }

static const uint8_t *editor_base(void)
{
    sh_diag_dump_module();   /* flag-gated one-time module dump for the Ghidra decompilation workflow */
    sh_diag_dump_gmmap();    /* one-time (retries until a live map): verify the new-build entity offsets via MapGetter */
    if (g_editor_verified) return g_editor;

    /* FAST PATH: resolve INSTANTLY via the editor global-pointer slot (RVA re-derived per build). *slot ->
     * editor; validate its vtable is an editor vtable. This avoids the ~2.8s pointer-scan entirely (that scan
     * blocks the UI-polling thread -> WebView never paints / paints very late). Slot null => editor not up
     * yet (cheap return). The full scan below stays as the portable fallback if this slot ever fails. */
    if (g_module_base && EDITOR_GLOBAL_PTR_RVA) {
        void *P = NULL, *vt = NULL;
        if (ie_read_ptr(g_module_base + EDITOR_GLOBAL_PTR_RVA, &P)) {   /* slot readable */
            if (P &&
                ie_read_ptr(P, &vt) &&
                (vt == (const void *)(g_module_base + EDITOR_VTABLE_RVA) ||
                 vt == (const void *)(g_module_base + EDITOR_VTABLE_RVA2))) {
                g_editor = (const uint8_t *)P;
                g_editor_verified = 1;
                char l[160];
                _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2: editor via hardcoded GLOBAL-PTR slot (fast) @ %p (RVA 0x%llX)",
                    (const void *)P, (unsigned long long)((size_t)((const uint8_t *)P - g_module_base)));
                backend_log(l);
                sh_diag_dump_true_editor(g_editor);
                return g_editor;
            }
            if (P == NULL) return NULL;   /* the slot IS this build's, the editor is simply not up yet:
                                           * cheap no-op, and deliberately NOT the slow scan. */
            /* Slot is readable but its target is not an editor. That does NOT mean "no editor": this RVA is
             * a constant for ONE DOOM build, and it is below SizeOfImage on the other build too, so the read
             * SUCCEEDS there and returns unrelated data. Returning NULL here made the editor unresolvable on
             * any build this constant does not belong to -- the boot-RVA and scan paths below were never
             * reached. Fall through to them instead; they identify the editor by SHAPE, not by a constant. */
        }
        /* slot READ FAULTED (RVA out of this build's range) -> fall through to the portable pointer-scan. */
    }

    if (g_editor && editor_fingerprint_ok(g_editor)) {   /* boot RVA was right (old build) */
        g_editor_verified = 1;
        backend_log("C2: editor confirmed at boot RVA (fingerprint ok)");
        return g_editor;
    }

    static DWORD s_last_scan = 0;
    DWORD now = GetTickCount();
    if (s_last_scan != 0 && (now - s_last_scan) < 1000u) return NULL;   /* throttle: <=1 scan/sec pre-resolve */
    s_last_scan = now;

    const uint8_t *found = sh_scan_for_editor();
    if (found) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C2: editor resolved by fingerprint scan @ %p (RVA 0x%llX; boot RVA guess was 0x%X)",
            (const void *)found, (unsigned long long)((size_t)(found - g_module_base)),
            (unsigned)EDITOR_SINGLETON_RVA);
        backend_log(line);
        g_editor = found;
        g_editor_verified = 1;
        sh_diag_dump_true_editor(found);   /* one-time: dump the TRUE editor's map/sel/entity layout (new build) */
        return g_editor;
    }
    /* DIAG: log the cascade a few times so a failing scan is explainable (module-size + where the
     * fingerprint collapses). Capped so an in-menu / never-resolving state doesn't spam the log. When there
     * are surviving candidates (ambiguous), dump each one's distinguishing fields so the right editor can be
     * told apart (RVA, mode, camera x/y/z, map entity-count). */
    {
        static int s_diag_left = 8;
        if (s_diag_left > 0) {
            s_diag_left--;
            char line[240];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "C2 editor-scan DIAG: modsize=0x%llX wsec=%u | vt=%u mode=%u screen=%u map=%u sel=%u full=%u cam=%u (need cam==1)",
                (unsigned long long)g_module_size, g_scan_diag_wsec, g_scan_diag_vt, g_scan_diag_mode,
                g_scan_diag_screen, g_scan_diag_map, g_scan_diag_sel, g_scan_diag_full, g_scan_diag_ents);
            backend_log(line);
            for (int i = 0; i < g_scan_cand_n; i++) {
                const uint8_t *E = g_scan_cand[i];
                int mode = 0; uint32_t entCnt = 0; void *map = NULL, *entArr = NULL; float cam[3] = {0,0,0};
                unsigned long long mapRVA = 0;
                ie_read_s32(E + ED_ENTITY_MODE_OFF, &mode);
                ie_read_ptr(E + ED_MAP_OBJ_OFF, &map);
                if (map) {
                    mapRVA = ie_ptr_in_module(map) ? (unsigned long long)((const uint8_t *)map - g_module_base) : 0;
                    ie_read_ptr((const uint8_t *)map + ARR_ENT_ARRAY_OFF, &entArr);   /* map+0x6a0 raw */
                    ie_read_u32((const uint8_t *)map + ARR_ENT_COUNT_OFF, &entCnt);    /* map+0x6a8 raw */
                }
                { __try { cam[0]=*(const float*)(E+ED_CAMERA_ORIGIN_OFF); cam[1]=*(const float*)(E+ED_CAMERA_ORIGIN_OFF+4); cam[2]=*(const float*)(E+ED_CAMERA_ORIGIN_OFF+8); } __except(EXCEPTION_EXECUTE_HANDLER){} }
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "C2 editor-scan CAND[%d]: RVA=0x%llX mode=%d cam=(%.1f,%.1f,%.1f) map=%p(RVA0x%llX inMod=%d) [+0x6a0]=%p [+0x6a8]=%u",
                    i, (unsigned long long)((size_t)(E - g_module_base)), mode, cam[0], cam[1], cam[2],
                    map, mapRVA, ie_ptr_in_module(map) ? 1 : 0, entArr, entCnt);
                backend_log(line);
            }
        }
    }
    return NULL;   /* editor not up yet (or unresolved) -- callers treat as "editor down", a clean no-op */
}

/* "editor up" guard, matching the reference implementation editorSession: the loaded-map ptr (+0x204c8) is non-null in-editor.
 * Returns the editor object base, or NULL when the editor isn't live (so every op fails CLEANLY). */
static const uint8_t *editor_session(void)
{
    const uint8_t *ed = editor_base();
    if (!ed) return NULL;
    void *mapObj = NULL;
    if (!ie_read_ptr(ed + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return ed;
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
    /* editor_base() drives the one-time fingerprint resolve (see its comment) -- this poll is the frontend's
     * steady heartbeat, so it's also what times the scan: it resolves the instant the editor comes up. Once
     * resolved, g_editor is stable; the screen ptr then reflects the live in-editor / HUB signal. */
    const uint8_t *ed = editor_base();
    if (!ed) return 0;
    void *screen = NULL;
    if (!ie_read_ptr(ed + ED_SCREEN_OFF, &screen)) return 0;
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
    if (array == NULL || count == 0 || count > ENT_COUNT_CAP) return 0;
    /* VALIDATE the array actually holds ENTITIES before trusting it. IMPORTANT: an idSnapMap entity does NOT
     * start with a C++ vtable -- its +0x00 is an int TAG (live-confirmed: 2). The OG validity signal is
     * entity+0x08 != 0 (a non-null sub-object ptr). Scan the first few slots: NULL slots are allowed
     * (sparse-by-id); a non-null slot whose +0x08 is null/unreadable means this isn't the entity array ->
     * refuse. Require >=1 valid entity so an all-null/garbage array can't pass. (Live-verified 2026-07-15:
     * map+0x6a0 IS the entity array; className "idSnapMapUserFilter" @ entity+0x158->+0x60.) */
    {
        uint32_t probe = count < 16u ? count : 16u, real = 0;
        for (uint32_t k = 0; k < probe; k++) {
            void *e = NULL, *sub = NULL;
            if (!ie_read_ptr((const uint8_t *)array + (size_t)k * 8, &e)) return 0;
            if (e == NULL) continue;                                          /* sparse-by-id slot: allowed */
            if (!ie_read_ptr((const uint8_t *)e + 0x08, &sub) || sub == NULL) return 0;  /* entity+8==0 -> not an entity */
            real++;
        }
        if (real == 0) return 0;   /* nothing that looks like a real entity in the probe window */
    }
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
#if !SH_NEWBUILD_SEL_VERIFIED
    /* GATED on the new build: the entity list's ids are not yet confirmed valid engine ids; passing a bad one
     * to AddToSelection corrupts editor state -> DOOM faults a frame later (our SEH can't catch that). No-op
     * until the entity/selection layout is re-derived live. */
    { static LONG s = 0; if (InterlockedCompareExchange(&s, 1, 0) == 0)
        backend_log("C2: add_to_selection GATED (new-build entity/selection offsets unverified) -- no-op"); }
    (void)id; return;
#else
    void *sel = selection_object();
    if (!sel || !g_add_sel) return;
    __try { g_add_sel(sel, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
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
#if !SH_NEWBUILD_WRITE_VERIFIED
    (void)id; (void)cstr; return;   /* GATED: new-build write path unverified (engine C++ throw on save) */
#endif
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
#if !SH_NEWBUILD_WRITE_VERIFIED
    (void)id; (void)cstr; return;   /* GATED: new-build write path unverified */
#endif
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
#if !SH_NEWBUILD_WRITE_VERIFIED
    (void)id; (void)cls; (void)inh; return 0;   /* GATED: new-build write path unverified */
#endif
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
    /* UN-GATED: g_idstr_opassign is now derived+prologue-validated from IdStrCtor+0x700 (or NULL -> safe). */
    if (!g_idstr_opassign) return;   /* displayName = a FULL idStr -> operator= (IdStrCtor+0x700), NOT the pool assign */
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
#if !SH_NEWBUILD_WRITE_VERIFIED
    (void)id; (void)cstr; return;   /* GATED: new-build write path unverified (decl reparse may throw) */
#endif
    if (!g_decl_rebuild || !cstr) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    __try { g_decl_rebuild(defsub, cstr, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0x130 REMOVE id from selection (Entities ctx-menu Delete): gated on editor+0x204d0 != 0 && id != -1.
 * OG FUN_1800073c0 -> engine 0x59fda0 (RemoveFromSelection). */
static void slot_remove_from_selection(sh_iface *self, int id)
{
    (void)self;
    /* Un-gated: this was disabled because RemoveFromSelection was reached through a hard-coded RVA that is
     * stale on the current build (delete crashed at 0x59fdbf). It is now SIGNATURE-resolved and verified
     * unique on both builds, so the address is no longer a guess -- and g_remove_sel being NULL (a build
     * where the signature does not resolve) already makes this a clean no-op. */
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

/* +0x2A0 ext 7: push `ids` onto the backend-owned SnapStack stack `index` (dedup-on-push). Lets an
 * out-of-process frontend (the webview host) reach the SAME stack a `sh <subcommand>` console command
 * typed afterward will see -- the Qt frontend has no need of this (its own Entities-tab "Push to stack
 * 0" reaches its in-process g_stacks directly via sh_snapstack_push_ids in snapstack.cpp). */
static void slot_push_to_stack(sh_iface *self, int index, const int *ids, int count)
{
    (void)self;
    sh_snapstack_push_ids_backend(index, ids, count);
}

/* +0x2A8 ext 8: empty the backend-owned SnapStack stack `index` -- the out-of-process counterpart to
 * slot_push_to_stack above, lets the webview host's "Clear stack 0" context-menu action reach the same
 * stack without needing the DOOM console. Qt has no need of this either, for the same reason as push. */
static int slot_clear_stack(sh_iface *self, int index)
{
    (void)self;
    return sh_snapstack_clear_stack_backend(index);
}

/* ================================================================ install ========================== */

int sh_iface_engine_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    if (module_base) {
        g_editor = module_base + EDITOR_SINGLETON_RVA;   /* boot GUESS (old build's RVA); editor_base() confirms
                                                          * it or fingerprint-scans for the real object at editor entry */
        g_module_base = module_base;                     /* for the dev-layer cvar read (cvarSys RVA fallback) */
        /* SizeOfImage (PE optional header +0x38) -- bounds the editor fingerprint scan. SEH-guarded parse. */
        uint32_t e_lfanew = 0, sizeOfImage = 0;
        if (ie_read_u32(module_base + 0x3C, &e_lfanew) && e_lfanew != 0 && e_lfanew <= 0x1000 &&
            ie_read_u32(module_base + e_lfanew + 24 + 0x38, &sizeOfImage) && sizeOfImage != 0) {
            g_module_size = sizeOfImage;   /* nt(=base+e_lfanew) + 4 sig + 20 file-hdr + 0x38 = optional-hdr SizeOfImage */
        }
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
    /* MapGetter-based map DIAG deps: MapGetter (sig) + the gameMgr slot (reuse entity.c's portable decoder).
     * Either may be NULL -> the DIAG logs "unresolved" once and disables itself. */
    g_ie_map_getter   = (void *(*)(void *))sig_addr_by_name(results, n, "MapGetter");
    g_ie_gamemgr_slot = sh_resolve_gamemgr_slot(results, n, module_base);
    /* RemoveFromSelection (the Entities ctx-menu Delete) now resolves by SIGNATURE. It was a
     * `module_base + 0x59fda0` fallback, described as "a jumptable-dispatch leaf the byte-sig scanner
     * cannot reliably anchor" -- but it signs uniquely at 25 bytes on both DOOM builds; nothing had ever
     * extracted a pattern for it. Unresolved stays NULL and slot_delete already null-checks (see :1474). */
    g_remove_sel = (remove_from_sel_fn)sig_addr_by_name(results, n, "RemoveFromSelection");
    if (module_base) {
        /* IdStrOpAssign (idStr::operator=(char*), displayName write) = IdStrCtor + 0x700 -- a STABLE delta
         * across the April 2024 patch (old 0x19fcef0/0x19fd5f0, new 0x33a0f0/0x33a7f0, both +0x700). Derive
         * from the SIG-resolved IdStrCtor so it auto-adapts (the raw IDSTR_OPASSIGN_RVA 0x19fd5f0 is stale on
         * the new build). VALIDATE the target's prologue first -- a wrong-fn call corrupts the SEH frame so a
         * bad guess could not be caught; on mismatch leave NULL (displayname no-ops safely). */
        g_idstr_opassign = NULL;
        if (g_idstr_ctor) {
            static const uint8_t op_prologue[] =
                { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x40 };
            const uint8_t *cand = (const uint8_t *)g_idstr_ctor + 0x700;
            __try {
                int ok = 1;
                for (size_t i = 0; i < sizeof op_prologue; i++) if (cand[i] != op_prologue[i]) { ok = 0; break; }
                if (ok) g_idstr_opassign = (idstr_opassign_fn)cand;
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_idstr_opassign = NULL; }
        }
        /* NB: on prologue mismatch g_idstr_opassign stays NULL -> displayname no-ops (never a stale-RVA call). */
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
                              &slots.apply_sync,        /* +0x290 SYNCHRONOUS inline apply (OG-faithful) */
                              &slots.normalize_timeline_inherit); /* +0x298 palette-timeline portable-inherit */
    /* the +0xb0 serialize-SELECTION->prefab slot also lives in the apply engine (it needs the
     * serialize engine fns). Fold it into the same bind. */
    sh_apply_engine_get_serialize_selection(&slots.serialize_selection);
    /* clone-extension: push onto the backend-owned SnapStack stack (out-of-process frontends only). */
    slots.push_to_stack          = slot_push_to_stack;        /* +0x2A0 ext 7 */
    /* clone-extension: empty the backend-owned SnapStack stack (out-of-process frontends only). */
    slots.clear_stack            = slot_clear_stack;          /* +0x2A8 ext 8 */
    sh_iface_bind_engine_slots(&slots);

    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2: iface engine slots bound (editor=%p add_sel=%p clear_sel=%p toast=%p idstr=%p/%p)",
        (void *)g_editor, (void *)g_add_sel, (void *)g_clear_sel, (void *)g_toast,
        (void *)g_idstr_ctor, (void *)g_idstr_dtor);
    backend_log(line);
    return 10;
}
