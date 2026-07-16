/* typeinfo.c -- see typeinfo.h. The type-introspection console commands
 * (cs_fieldinfo, sh_type). Ports of OG XINPUT1_3 FUN_180021db0 / FUN_180021090.
 *
 * Both handlers share sh_commands' console ABI (idCmdArgs / cmd_argv / sh_printf) + the SEH byte-copy
 * (sh_safe_read) via commands.h, so there is ONE Printf wrapper + ONE safe-read across the whole command
 * surface. sh_type's clipboard copy reuses sh_clipboard_set (cs_fieldinfo does NOT copy). The reflection
 * deps (the declMgr accessor at the hardcoded RVA 0x17F7030 + vtable+0x80, FindTypeInfoByName,
 * FindEnumByName) are resolved/cached by sh_typeinfo_install.
 *
 * Every engine deref is SEH-guarded and non-null gated -- a wrong/shifted build offset degrades to a clean
 * printed error, never a crash. Both field/enum walks carry an iteration CAP so a non-terminating (never-
 * NULL-name) garbage record cannot spin forever, mirroring sh_commands' LISTRES_COUNT_CAP discipline.
 *
 * Clean-room: ported from our own RE (the foundation report). Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "typeinfo.h"
#include "commands.h"
#include "clipboard.h"
#include "backend_log.h"
#include "class_universe.h"  /* SH_CLASS_UNIVERSE[] -- the dropdown candidate list for sh_validclasses */

/* ------------------------------------------------------------------------ engine fn typedefs ------ */

/* The declMgr accessor at the HARDCODED RVA 0x17F7030. NOT a trivial `mov rax,[rip+x]; ret` -- the engine
 * bytes there are a real lazy-init singleton accessor (`53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF ...`:
 * push rbx; sub rsp,0x30; SEH cookie slot; lazy-init the declMgr singleton; return its ptr in RAX). Its
 * fixed prologue is shared by ~47 .text functions and only becomes unique via the build-volatile RIP
 * displacement -- which is WHY it is not signature-able (a stable sig can't pin the volatile disp).
 * Resolved off g_doom_base, the established precedent (the DECL_MGR_ACCESSOR_RVA / the reference
 * implementation _declMgrAccessor -- both hardcoded data RVAs, no sig). 0-arg, returns the declMgr object in RAX. */
typedef void *(*declmgr_getter_fn)(void);
#define DECLMGR_ACCESSOR_KNOWN_RVA  0x17F7030u

/* declMgr -> reflection/type-info manager: vtable slot +0x80 (the reflection accessor; matches
 * the reference implementation declMgr.readPointer().add(0x80).readPointer()). __fastcall(self) -> reflect. */
#define VSLOT_REFLECT_ACCESSOR  0x80

/* FindTypeInfoByName(reflect, name, scope=0) -> the type record (sig "FindTypeInfoByName" 0x1A1D590).
 * Ghidra labels it void(longlong*,char*,char*) -- a DECOMPILER MISS (the recursive %s::%s scope lookup
 * defeats return-register recovery); the live caller FUN_1409c79d0 PROVES a non-void record return. We
 * call it 3-arg returning rec*; rec==NULL means "not found" (sh_type then falls through to FindEnumByName). */
typedef void *(*find_typeinfo_fn)(void *reflect, const char *name, void *scope);
/* FindEnumByName(reflect, name) -> the enum record (sig "FindEnumByName" 0x1A1DA20). Cleanly returns the
 * record (return recovered). enumRec==NULL => "Couldn't find type". */
typedef void *(*find_enum_fn)(void *reflect, const char *name);

/* --------------------------------------------------------------- field/enum record sub-offsets ----
 * SHARED record (cs_fieldinfo + sh_type CLASS branch). CONFIRMED LIVE (FUN_1409c79d0): field array @
 * rec+0x20, stride 0x48, name @ field+0x10. OG-handler-only (BUILD-SPECIFIC, live-confirm at FIRE):
 * offset @ +0x18, size @ +0x1c, varType @ +0x00, varOps @ +0x08, comment @ +0x28.
 *
 * The CLASS-branch per-field render reads THREE strings (re-derived against OG FUN_180021090's field
 * loop, its decompile L240-253 + the two fmt literals @0x374e0/0x37518):
 *   field+0x00 = varType (the PRIMARY type string -- OG `pcVar13 = *puVar5`, always the 1st %s)
 *   field+0x08 = varOps  (the pointer/array qualifier -- the strstr("*") target, OG `puVar5[1]`)
 *   field+0x10 = varName (OG `puVar5[2]`, also the loop terminator)
 * matching the engine's idlib schema idTypeInfoTools field-metadata (..,varType,varOps,varName,..).
 * cs_fieldinfo never touches +0x00/+0x08 -- it only needs name/offset/size. */
#define REC_FIELDS_OFF      0x20    /* type record -> field array base (CONFIRMED LIVE) */
#define REC_SUPER_OFF       0x08    /* type record -> superclass name char* (sh_type; OG-only) */
#define FIELD_STRIDE        0x48    /* field-record stride (CONFIRMED LIVE) */
#define FIELD_NAME_OFF      0x10    /* field -> varName char* (CONFIRMED LIVE; loop terminator on NULL/empty) */
#define FIELD_OFFSET_OFF    0x18    /* field -> offset (uint) (OG-only, BUILD-SPECIFIC) */
#define FIELD_SIZE_OFF      0x1c    /* field -> size   (uint) (OG-only, BUILD-SPECIFIC) */
#define FIELD_VARTYPE_OFF   0x00    /* field -> varType char* (primary type; OG arg1, always printed) */
#define FIELD_VAROPS_OFF    0x08    /* field -> varOps  char* (qualifier; the strstr("*") target) */
#define FIELD_COMMENT_OFF   0x28    /* field -> comment char* (OG-only, BUILD-SPECIFIC) */

/* ENUM-member record (sh_type ENUM branch). CONFIRMED LIVE (FUN_140440230): members array @ enumRec+0x10,
 * stride 0x10, member name @ +0, value(uint) @ +8. OG-handler-only: enum NAME @ enumRec+0x00. */
#define ENUM_NAME_OFF       0x00    /* enum record -> enum NAME char* (OG-only, BUILD-SPECIFIC) */
#define ENUM_MEMBERS_OFF    0x10    /* enum record -> members array base (CONFIRMED LIVE) */
#define ENUM_MEMBER_STRIDE  0x10    /* enum-member stride (CONFIRMED LIVE) */
#define EMEMBER_NAME_OFF    0x00    /* member -> name char* (CONFIRMED LIVE; loop terminator on NULL) */
#define EMEMBER_VALUE_OFF   0x08    /* member -> value (uint) (CONFIRMED LIVE) */

/* Iteration CAP -- a never-terminating (garbage / shifted) record must not spin forever. Both the field
 * walk and the enum walk bound their loops by this (same stale-record discipline as LISTRES_COUNT_CAP). */
#define TI_WALK_CAP   4096u

/* sh_type's accumulation buffer (the OG writes a fixed 0x800 stack buffer per Printf; we accumulate the
 * whole dump into one fixed buffer + copy it to the clipboard, like sh_spawninfo's 0x800 buf). */
#define TI_DUMP_CAP   0x4000

/* ------------------------------------------------------------------------- module state ----------- */

static const uint8_t   *g_doom_base    = NULL;   /* for the declMgr accessor at base + 0x17F7030 */
static find_typeinfo_fn g_find_type    = NULL;   /* FindTypeInfoByName (sig 0x1A1D590) */
static find_enum_fn     g_find_enum    = NULL;   /* FindEnumByName     (sig 0x1A1DA20) */
static volatile LONG    g_installed    = 0;      /* one-shot install latch */

/* ----------------------------------------------------------- SHARED declMgr-object accessor -------
 * Returns the raw declMgr singleton object (the accessor at base + 0x17F7030, SEH-guarded). NON-static:
 * sh_superscriptop [12] in commands.c REUSES this to reach the engine event-manager (declMgr vtable
 * slot +0x90 -> evMgr) -- it shares sh_typeinfo's ONE declMgr accessor rather than re-resolving the RVA.
 * NULL on a NULL base or any fault. */
void *sh_typeinfo_get_declmgr(void)
{
    if (!g_doom_base) return NULL;
    /* VALIDATE the accessor RVA BEFORE calling it. On the post-April-2024 DOOM build
     * DECLMGR_ACCESSOR_KNOWN_RVA (0x17F7030) is STALE -- it now lands inside idImpactManager::Serialize, so
     * calling it runs unrelated engine code with garbage args and DOOM faults a frame later (live-confirmed
     * 2026-07-15: the class/inherit dropdown enumerate on entity-select crashed at the engine decl-find
     * 0x18017a0, reached through this bad accessor). The real accessor's prologue is fixed: push rbx; sub
     * rsp,0x30; SEH-cookie store = 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF. If the bytes don't match, the
     * RVA moved on this build -> refuse (return NULL) so every typeinfo user (enum dropdowns, sh_type,
     * cs_fieldinfo, sh_superscriptop) degrades to its static fallback instead of crashing. RE-DERIVE the RVA
     * per build to restore the live registry. */
    static const uint8_t expect[] = { 0x53,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF };
    const uint8_t *acc = g_doom_base + DECLMGR_ACCESSOR_KNOWN_RVA;
    __try {
        for (size_t i = 0; i < sizeof expect; i++) {
            if (acc[i] != expect[i]) {
                static LONG s_warned = 0;
                if (InterlockedCompareExchange(&s_warned, 1, 0) == 0)
                    backend_log("B2: declMgr accessor RVA 0x17F7030 STALE on this build (prologue mismatch) -- "
                                "type registry DISABLED (dropdowns use static list; sh_type unavailable). Re-derive per build.");
                return NULL;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    __try {
        declmgr_getter_fn getter = (declmgr_getter_fn)acc;
        return getter();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* ------------------------------------------------------------- SEH-guarded reflection helpers -----
 * declMgr accessor -> declMgr; reflect = (*(*declMgr + 0x80))(declMgr). A wrong accessor RVA, a NULL
 * declMgr, or a wrong vtable slot degrades to NULL (the handler then prints "type manager unavailable"). */
static void *ti_get_reflect(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    if (!declmgr) return NULL;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;      /* *declMgr = the vtable */
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_REFLECT_ACCESSOR);  /* vtable[+0x80] */
        if (!fn) return NULL;
        return fn(declmgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* ----------------------------------------------------- decl-type instance enumerator (Timeline combo) -
 * Pack the decl-instance NAMES of a decl-type into out_buf (consecutive NUL-terminated strings, double-NUL
 * end -- the SAME packed-string ABI the +0x110 slot_enum_decls_of_resclass returns). The Timeline-Editor's
 * decl comboboxes use THIS, not engine GetDeclsOfType: GetDeclsOfType is the engine's ASSET registry
 * (idImage/idMD6Anim/...) and LOGS "Unknown resource class '%s'" on a decl-type miss (the console spam).
 * The non-logging path (OG XINPUT +0x100 FUN_180006eb0): reflect = declMgr->[+0x80]; node = FindByName
 * (g_find_enum = engine 0x1A1DA20, a hash lookup that returns 0 SILENTLY on a miss); the decl instances are
 * at *(node+0x10) -- {name-char-ptr, _} pairs, stride 0x10, terminated by a NULL name. Returns 1 + *out_count
 * on >=1 name, else 0. SEH-guarded + TI_WALK_CAP-bounded: a miss / shifted node degrades to a clean 0 -- no
 * log, no crash (the frontend then leaves the combo editable, faithful to the OG miss branch). */
int sh_typeinfo_enum_decls_of_type(const char *declType, char *out_buf, int cap, int *out_count)
{
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!declType || !declType[0] || !out_buf || cap <= 1 || !g_find_enum) return 0;

    void *reflect = ti_get_reflect();
    if (!reflect) return 0;
    void *node = NULL;
    __try { node = g_find_enum(reflect, declType); }
    __except (EXCEPTION_EXECUTE_HANDLER) { node = NULL; }
    if (!node) return 0;

    int written = 0, names = 0;
    __try {
        void **list = *(void ***)((const uint8_t *)node + 0x10);   /* *(node+0x10) = the instance list */
        for (uint32_t i = 0; list && i < TI_WALK_CAP; i++) {
            const char *nm = (const char *)list[(size_t)i * 2];     /* {name,_} pairs -> stride 0x10 (2 qwords) */
            if (!nm) break;                                          /* NULL name terminates the list */
            int nlen = (int)strlen(nm);
            if (nlen <= 0 || nlen > 250) continue;
            if (written + nlen + 1 > cap - 1) break;
            memcpy(out_buf + written, nm, (size_t)nlen);
            out_buf[written + nlen] = '\0';
            written += nlen + 1;
            names++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep whatever we copied before the fault */ }

    out_buf[written] = '\0';   /* double-NUL end marker */
    if (out_count) *out_count = names;
    return names > 0 ? 1 : 0;
}

/* FindTypeInfoByName(reflect, name, NULL) -> rec*, SEH-guarded. NULL on any fault / missing sig. */
static void *ti_find_type(void *reflect, const char *name)
{
    if (!g_find_type || !reflect || !name) return NULL;
    __try {
        return g_find_type(reflect, name, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* FindEnumByName(reflect, name) -> enumRec*, SEH-guarded. NULL on any fault / missing sig. */
static void *ti_find_enum(void *reflect, const char *name)
{
    if (!g_find_enum || !reflect || !name) return NULL;
    __try {
        return g_find_enum(reflect, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* SEH-guarded scalar/pointer reads at record+offset. NULL/0 on any fault. */
static const char *ti_read_cstr(const void *base, size_t off)
{
    __try { return *(const char * const *)((const uint8_t *)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *ti_read_ptr(const void *base, size_t off)
{
    __try { return *(void * const *)((const uint8_t *)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int ti_read_u32(const void *base, size_t off, uint32_t *out)
{
    __try { *out = *(const uint32_t *)((const uint8_t *)base + off); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* ------------------------------------------------------- LAYER C: class-lineage compatibility check ----
 * Does `className` derive from (or equal) `baseName`, walking the engine type hierarchy BY NAME -- the SAME
 * walk the engine's decl validator does (the decompiled FUN_141a201d0: FindTypeInfoByName -> the superclass
 * name @ rec+0x08, up the chain until baseName is found or the chain ends). The bscls/bsin prevention guard
 * (iface_engine.c) calls this to REJECT an incompatible class change UP FRONT -- before the engine's decl
 * reparse raises the fatal "Class X does not derive from Y" Error(6), which an INNER engine handler catches
 * before idCommonLocal::Frame so the fault-shield cannot recover it (prevent-not-recover,
 * error-dispatcher-and-recovery.md).
 * Returns: 1 = derives, 0 = does NOT derive (incl. an UNRESOLVABLE className -- the engine validator treats
 * an unknown class as "does not derive" too), -1 = type system UNAVAILABLE (reflect NULL; the caller must
 * NOT reject on -1, only on a definite 0). Bounded (64 levels) + SEH-guarded (via the ti_* helpers). */
/* The decl-VALIDATOR's reflection context: manager = *(base + 0x4DF9648); reflect = manager->vtable[+0x240]
 * (manager). The validator (FUN_141a201d0 -> FUN_141a1d590 = FindTypeInfoByName) walks types via THIS
 * reflect -- which resolves the SnapMap entity classes (logic_base / idSnapMapUserFilter) that the
 * declMgr->[+0x80] reflect sh_typeinfo's sh_type uses MISSES. RE'd from the validator disasm: the
 * `mov rcx,[rip+0x364bf48]` @ 0x17ad6f9 resolves to 0x17ad700 + 0x364bf48 = 0x4DF9648 (the live decl/
 * resource manager -- ~100 code xrefs). NOTE: the earlier 0x3BF8648 here was an arithmetic error (ZERO
 * xrefs -> NULL at runtime -> this getter returned NULL and the guard silently used the ti_get_reflect
 * fallback). Build-specific (global RVA + vtable slot) -> SEH-guarded; NULL on any fault (caller falls back
 * to ti_get_reflect). */
#define VALIDATOR_MGR_RVA         0x4DF9648u
#define VSLOT_VALIDATOR_REFLECT   0x240
static void *ti_get_validator_reflect(void)
{
    if (!g_doom_base) return NULL;
    __try {
        void *mgr = *(void * const *)(g_doom_base + VALIDATOR_MGR_RVA);
        if (!mgr) return NULL;
        const uint8_t *vtbl = *(const uint8_t * const *)mgr;
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_VALIDATOR_REFLECT);
        if (!fn) return NULL;
        return fn(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

int sh_typeinfo_class_derives(const char *className, const char *baseName)
{
    if (!className || !baseName || !className[0] || !baseName[0]) return -1;
    void *reflect = ti_get_validator_reflect();      /* the validator's context (resolves SnapMap classes) */
    if (!reflect) reflect = ti_get_reflect();        /* fallback: the declMgr->[+0x80] reflect */
    if (!reflect) return -1;
    const char *cur = className;
    for (int i = 0; i < 64 && cur && cur[0]; i++) {
        if (strcmp(cur, baseName) == 0) return 1;          /* baseName found in the ancestry chain */
        void *rec = ti_find_type(reflect, cur);
        if (!rec) return 0;                                /* `cur` is not a known type -> it cannot derive
                                                            * from baseName (the engine validator treats an
                                                            * unresolvable class as "does not derive" too).
                                                            * reflect==NULL already returned -1 above, so this
                                                            * NULL means the type genuinely is not found. */
        cur = ti_read_cstr(rec, REC_SUPER_OFF);            /* rec+0x08 = the superclass name (walk up) */
    }
    return 0;                                              /* chain exhausted without baseName */
}

/* Resolve the inherit decl's base class name Y -- the class an entity's className MUST derive from for the
 * engine decl validator to accept it. Via the engine's PURE decl find (FUN_1418017a0: read-lock -> hash ->
 * probe -> cached-decl-or-NULL -> unlock; NO load/parse/global-mutation/INT3 -- verified DIRECT, vs the
 * load-or-create FUN_1417b36f0 the validator uses, which has FatalError+INT3 traps and is NOT safe to call)
 * over the resource-mgr ctx at base+0x59BD8F0, then idDeclEntityDef.className @ +0x60 (the validator's
 * vtbl+0xb0 = `return *(this+0x60)`). The inherit decl is already loaded (the entity exists), so the pure
 * find returns it. RVA-tagged (the find + ctx are non-sig-able resource-mgr internals -- re-derive per build
 * via the validator @ 0x17ad682/0x17ad689). SEH-guarded: any fault / not-found inherit -> NULL (caller
 * fail-opens). Copies Y into buf; returns buf or NULL. */
#define RESOURCE_MGR_CTX_RVA   0x59BD8F0u   /* resource-mgr ctx object (validator lea @ 0x17ad682) */
#define DECL_PURE_FIND_RVA     0x18017A0u   /* FUN_1418017a0(ctx,name) -> cached decl-or-NULL (pure hash find) */
#define DECL_CLASSNAME_OFF     0x60u        /* idDeclEntityDef.className char* (vtbl+0xb0 = return *(this+0x60)) */
const char *sh_typeinfo_inherit_base(const char *inheritName, char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
    if (!g_doom_base || !inheritName || !inheritName[0] || !buf || cap < 2) return NULL;
    /* GUARD the hardcoded engine RVAs (DECL_PURE_FIND_RVA 0x18017A0 + RESOURCE_MGR_CTX_RVA 0x59BD8F0). On the
     * post-April-2024 build 0x18017A0 is NO LONGER the pure decl-find -- calling that wrong code corrupts the
     * SEH frame (so even the __try below can't catch it) and DOOM hard-faults (live-confirmed: entity-select
     * -> dropdown enum -> here -> crash @ 0x18017a0). Tie safety to the declMgr-accessor prologue check: if
     * the live type registry is unreachable on this build, these sibling decl RVAs are stale too -> skip the
     * call and fail-open (caller uses the universal "idEntity" set). RE-DERIVE 0x18017A0 + 0x59BD8F0 per build
     * to restore live inherit-base resolution. */
    if (!sh_typeinfo_get_declmgr()) return NULL;
    __try {
        typedef void *(*decl_find_fn)(void *ctx, const char *name);   /* FUN_141800a40 calls it with 2 args */
        void *ctx = (void *)(g_doom_base + RESOURCE_MGR_CTX_RVA);
        decl_find_fn find = (decl_find_fn)(g_doom_base + DECL_PURE_FIND_RVA);
        void *decl = find(ctx, inheritName);
        if (!decl) return NULL;
        const char *cn = *(const char * const *)((const uint8_t *)decl + DECL_CLASSNAME_OFF);
        if (!cn || !cn[0]) return NULL;
        lstrcpynA(buf, cn, (int)cap);
        return buf[0] ? buf : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; return NULL; }
}

/* -------------------------------------------------- LIVE reflection type-registry walk (enumerate all) ----
 * The registry is a NULL-name-sentinel flat array reachable from the SAME reflect sh_type uses: P =
 * *(reflect+0) (the container global), type-record array B = *(P+0x20), records stride 0x38, className @
 * rec+0x00 (NULL = end), superclass name @ rec+0x08. RE'd from the engine: FindTypeInfoByName 0x1A1D590
 * returns *(*(reflect+0)+0x20)+idx*0x38; the registry builder 0x1A1EEE0 + registrar 0x1A1CCF0 iterate the
 * same array; the idEntity-derived subset reproduces OG's frozen 892 string-for-string (verified). Reuses
 * ti_get_reflect() -- ZERO new sigs/offsets. */
#define REGISTRY_TYPEBASE_OFF   0x20      /* container P -> type-record array base (*(P+0x20)) */
#define REGISTRY_RECORD_STRIDE  0x38      /* per-record stride */
#define REGISTRY_NAME_OFF       0x00      /* record -> className char* (NULL name terminates the array) */
#define REGISTRY_SUPER_OFF      0x08      /* record -> superclass name char* ("" for a root) */
#define REGISTRY_WALK_CAP       65536u    /* stale/garbage-array guard (this build has ~10,190 records) */
#define TYPE_CONTAINER_RVA      0x3082b10u /* the reflection container object P (reflect+0 holds &this). Used as
                                           * the thread-safe fallback when reflect is null (the Qt UI thread):
                                           * P is a fixed static global, so B=*(P+0x20) is a pure raw read.
                                           * BUILD-SPECIFIC -- re-derive per build: it's *(reflect+0) on the
                                           * game thread, per our RE of the reflection registry). */

/* Root B = the type-record array base, THREAD-SAFELY. Primary (portable): reflect (game thread) -> P=*(reflect).
 * Fallback (Qt UI thread, where the reflect vtable accessor returns null): the container global P=base+0x3082b10
 * (a fixed static object -- raw read, no vtable call). Either way B=*(P+0x20). NULL only if neither roots. */
static const uint8_t *ti_type_array_base(void)
{
    const uint8_t *P = NULL;
    void *reflect = ti_get_reflect();
    if (reflect) {
        __try { P = *(const uint8_t * const *)reflect; }
        __except (EXCEPTION_EXECUTE_HANDLER) { P = NULL; }
    }
    if (P == NULL && g_doom_base) P = g_doom_base + TYPE_CONTAINER_RVA;   /* UI-thread fallback (fixed global) */
    if (P == NULL) return NULL;
    const uint8_t *B = NULL;
    __try { B = *(const uint8_t * const *)(P + REGISTRY_TYPEBASE_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { B = NULL; }
    return B;
}

int sh_typeinfo_collect_classnames(const char **out_names, int cap)
{
    if (!out_names || cap <= 0) return -1;
    const uint8_t *B = ti_type_array_base();
    if (!B) return -1;                                /* pre-boot / unrooted -> caller falls back */
    int n = 0;
    __try {
        for (uint32_t i = 0; i < REGISTRY_WALK_CAP && n < cap; i++) {
            const uint8_t *rec = B + (size_t)i * REGISTRY_RECORD_STRIDE;
            const char *name = *(const char * const *)(rec + REGISTRY_NAME_OFF);
            if (name == NULL) break;                  /* NULL-name sentinel = end of array */
            out_names[n++] = name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* a shifted/garbage array degrades to whatever we collected -- never a crash */
    }
    return n;
}

int sh_typeinfo_collect_records(sh_ti_record *out, int cap)
{
    if (!out || cap <= 0) return -1;
    const uint8_t *B = ti_type_array_base();
    if (!B) return -1;
    int n = 0;
    __try {
        for (uint32_t i = 0; i < REGISTRY_WALK_CAP && n < cap; i++) {
            const uint8_t *rec = B + (size_t)i * REGISTRY_RECORD_STRIDE;
            const char *name = *(const char * const *)(rec + REGISTRY_NAME_OFF);
            if (name == NULL) break;
            out[n].name  = name;
            out[n].super = *(const char * const *)(rec + REGISTRY_SUPER_OFF);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

/* Enumerate the LIVE entityDef decl manager (the valid-INHERIT set). The mgr @ RESOURCE_MGR_CTX_RVA (0x59BD8F0
 * -- the SAME ctx sh_typeinfo_inherit_base uses) is a flat array: count @ mgr+0x28, array @ mgr+0x20, each
 * element an idDeclEntityDef* whose name (the decl PATH) is the generic idDecl name slot @ decl+0x08. Pure raw
 * reads -> thread-safe on the Qt UI thread. */
#define ENTITYDEF_MGR_ARRAY_OFF  0x20
#define ENTITYDEF_MGR_COUNT_OFF  0x28
#define DECL_NAME_OFF            0x08
int sh_typeinfo_collect_inherits(const char **out_names, int cap)
{
    if (!out_names || cap <= 0 || !g_doom_base) return -1;
    int n = 0;
    __try {
        const uint8_t *mgr = g_doom_base + RESOURCE_MGR_CTX_RVA;
        int count = *(const int *)(mgr + ENTITYDEF_MGR_COUNT_OFF);
        const uint8_t * const *arr = *(const uint8_t * const * const *)(mgr + ENTITYDEF_MGR_ARRAY_OFF);
        if (arr == NULL || count <= 0 || (uint32_t)count > REGISTRY_WALK_CAP) return -1;  /* stale-mgr guard */
        if (count > cap) count = cap;
        for (int i = 0; i < count; i++) {
            const uint8_t *decl = arr[i];
            if (decl == NULL) continue;
            const char *name = *(const char * const *)(decl + DECL_NAME_OFF);
            if (name != NULL && name[0] != '\0') out_names[n++] = name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

/* ----------------------------------------------------------------------------- handlers ----------
 * Non-static (extern-declared in commands.c) so CMD_TABLE references them directly. */

/* [10] cs_fieldinfo <type> <field> (READ-ONLY) -- find the type record, walk its field array (stride
 * 0x48, name @ +0x10, loop-terminate on NULL/empty), and on the named field print "Size N, offset M".
 * Port of OG FUN_180021db0. No clipboard. */
void h_cs_fieldinfo(idCmdArgs *a)
{
    const char *type  = cmd_argv(a, 1);
    const char *field = cmd_argv(a, 2);
    if (type == NULL || field == NULL) {
        sh_printf("usage: cs_fieldinfo <type> <field>\n");
        return;
    }

    void *reflect = ti_get_reflect();
    if (reflect == NULL) {
        sh_printf("cs_fieldinfo: type manager unavailable.\n");
        return;
    }

    void *rec = ti_find_type(reflect, type);
    if (rec == NULL) {
        sh_printf("cs_fieldinfo: couldn't find type '%s'.\n", type);
        return;
    }

    const uint8_t *fields = (const uint8_t *)ti_read_ptr(rec, REC_FIELDS_OFF);
    if (fields == NULL) {
        sh_printf("cs_fieldinfo: type '%s' has no fields.\n", type);
        return;
    }

    for (uint32_t i = 0; i < TI_WALK_CAP; i++) {
        const uint8_t *f = fields + (size_t)i * FIELD_STRIDE;
        const char *name = ti_read_cstr(f, FIELD_NAME_OFF);
        if (name == NULL || name[0] == '\0') break;            /* OG loop terminator: NULL/empty name */
        if (strcmp(name, field) == 0) {
            uint32_t size = 0, off = 0;
            if (!ti_read_u32(f, FIELD_SIZE_OFF, &size) || !ti_read_u32(f, FIELD_OFFSET_OFF, &off)) {
                sh_printf("cs_fieldinfo: field '%s' size/offset unreadable.\n", field);
                return;
            }
            sh_printf("Size %d, offset %d\n", size, off);      /* OG verbatim (the OG handler @0x21db0) */
            return;
        }
    }
    sh_printf("cs_fieldinfo: type '%s' has no field '%s'.\n", type, field);
}

/* A tiny SEH-safe append into the fixed dump buffer (truncates on overflow; never overruns). */
static void ti_dump_append(char *buf, size_t cap, size_t *len, const char *s)
{
    if (s == NULL || *len >= cap - 1) return;
    size_t room = cap - 1 - *len;
    size_t add  = strlen(s);
    if (add > room) add = room;
    memcpy(buf + *len, s, add);
    *len += add;
    buf[*len] = '\0';
}

/* Emit a possibly-multi-KB string through sh_printf, which truncates EACH call at its 1024-byte stack
 * buffer (commands.c). OG sh_type (FUN_180021090) printed field-by-field -- each line well under the cap --
 * but we accumulate the whole struct into one buffer for the clipboard copy, so a single sh_printf("%s",dump)
 * is silently cut off at ~1KB (idMover stops mid-field at crushDislodgeForceMult). Chunk the on-screen emit
 * under the cap, breaking on newlines so a chunk never splits a field line. The clipboard still gets the full
 * dump. */
static void ti_emit_long(const char *s)
{
    if (s == NULL) return;
    const size_t CHUNK = 1000;            /* < sh_printf's 1024 buf; "%s" expands the content 1:1 */
    char piece[1024];
    size_t n = strlen(s), i = 0;
    while (i < n) {
        size_t take = (n - i < CHUNK) ? (n - i) : CHUNK;
        if (take == CHUNK) {              /* break at the last newline in the window (keep lines whole) */
            size_t br = take;
            while (br > 0 && s[i + br - 1] != '\n') br--;
            if (br > 0) take = br;        /* no newline in the window -> emit the full CHUNK as-is */
        }
        memcpy(piece, s + i, take);
        piece[take] = '\0';
        sh_printf("%s", piece);
        i += take;
    }
}

/* [3] sh_type <name> [-v] -- dump a CLASS's fields or an ENUM's members as C-struct/enum text, print it,
 * and copy it to the clipboard. Port of OG FUN_180021090.
 *   reflect = declMgr->reflect; rec = FindTypeInfoByName(reflect,name);
 *   if rec  -> CLASS branch (Inherits + per-field "type name;")
 *   else en = FindEnumByName(reflect,name); if en -> ENUM branch ("enum NAME { name = val, ... };")
 *   else "Couldn't find type %s!".
 * OG always prints a trailing "//offset N size M" on each field; here the default is CLEAN and the
 * optional "-v" flag restores that offset/size (build-specific info -- handy for memory work, noise for
 * browsing). The clipboard copy matches the on-screen text (both come from the one `dump` buffer). */
void h_sh_type(idCmdArgs *a)
{
    const char *type = cmd_argv(a, 1);
    if (type == NULL) {
        sh_printf("No type provided!\n");                      /* OG verbatim (the OG handler @0x21090) */
        return;
    }
    /* clone extension: an optional "-v" (arg 2) restores the per-field reflection offset/size that OG
     * always prints. Default = clean (no //offset size). Only the CLASS branch has offset/size, so -v is
     * a no-op for enums. arg 2 never collides with OG (OG sh_type takes only the type name). */
    const char *vflag = cmd_argv(a, 2);
    int verbose = (vflag != NULL && _stricmp(vflag, "-v") == 0);

    void *reflect = ti_get_reflect();
    if (reflect == NULL) {
        sh_printf("sh_type: type manager unavailable.\n");
        return;
    }

    static char dump[TI_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char tmp[1024];

    void *rec = ti_find_type(reflect, type);
    if (rec != NULL) {
        /* ---- CLASS branch ---- */
        const char *super = ti_read_cstr(rec, REC_SUPER_OFF);
        sh_printf("Inherits %s\n", (super && super[0]) ? super : "(none)");

        const char *cname = ti_read_cstr(rec, ENUM_NAME_OFF);  /* class NAME @ rec+0x00 */
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "struct %s {\n", (cname && cname[0]) ? cname : type);
        ti_dump_append(dump, sizeof dump, &dlen, tmp);
        if (super && super[0]) {
            _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s base;\n", super);
            ti_dump_append(dump, sizeof dump, &dlen, tmp);
        }

        const uint8_t *fields = (const uint8_t *)ti_read_ptr(rec, REC_FIELDS_OFF);
        for (uint32_t i = 0; fields != NULL && i < TI_WALK_CAP; i++) {
            const uint8_t *f = fields + (size_t)i * FIELD_STRIDE;
            const char *fname = ti_read_cstr(f, FIELD_NAME_OFF);      /* +0x10 varName (loop terminator) */
            if (fname == NULL || fname[0] == '\0') break;             /* OG terminator: cmp [rsi+0x10],0 */
            const char *vartype = ti_read_cstr(f, FIELD_VARTYPE_OFF); /* +0x00 varType (primary type) */
            const char *varops  = ti_read_cstr(f, FIELD_VAROPS_OFF);  /* +0x08 varOps  (qualifier) */
            const char *fcmt    = ti_read_cstr(f, FIELD_COMMENT_OFF); /* +0x28 comment */
            uint32_t foff = 0, fsize = 0;
            ti_read_u32(f, FIELD_OFFSET_OFF, &foff);
            ti_read_u32(f, FIELD_SIZE_OFF, &fsize);
            if (vartype == NULL) vartype = "?";
            if (varops  == NULL) varops  = "";

            /* OG fmt-selects on strstr(varOps,"*") -- the QUALIFIER, not the type. Both forms print
             * THREE %s; the arg ORDER differs per form (copy OG exactly, @0x21090 L240-253):
             *   star    (varOps has '*'): "\t%s%s %s" = (varType, varOps, varName)
             *   no-star                 : "\t%s %s%s" = (varType, varName, varOps)
             * The trailing ";" + optional "//offset N size M" is appended below so the offset/size can be
             * gated on -v without duplicating both format arms. */
            int is_ptr = (strstr(varops, "*") != NULL);
            if (is_ptr)
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s%s %s", vartype, varops, fname);
            else
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s %s%s", vartype, fname, varops);
            ti_dump_append(dump, sizeof dump, &dlen, tmp);
            /* default: a clean "type name;"; -v restores OG's build-specific offset/size comment. */
            if (verbose)
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ";//offset %d size %d\n", foff, fsize);
            else
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ";\n");
            ti_dump_append(dump, sizeof dump, &dlen, tmp);
            if (fcmt && fcmt[0]) {
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t// %s\n", fcmt);
                ti_dump_append(dump, sizeof dump, &dlen, tmp);
            }
        }
        ti_dump_append(dump, sizeof dump, &dlen, "};\n");

        ti_emit_long(dump);
        sh_printf("Dumped type is a Class\n");
        if (sh_clipboard_set(dump))
            sh_printf("sh_type: copied type '%s' to the clipboard.\n", type);
        return;
    }

    /* ---- ENUM branch (FindTypeInfoByName returned NULL) ---- */
    void *en = ti_find_enum(reflect, type);
    if (en == NULL) {
        sh_printf("Couldn't find type %s!\n", type);           /* OG verbatim */
        return;
    }

    const char *ename = ti_read_cstr(en, ENUM_NAME_OFF);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "enum %s {\n", (ename && ename[0]) ? ename : type);
    ti_dump_append(dump, sizeof dump, &dlen, tmp);

    const uint8_t *members = (const uint8_t *)ti_read_ptr(en, ENUM_MEMBERS_OFF);
    for (uint32_t i = 0; members != NULL && i < TI_WALK_CAP; i++) {
        const uint8_t *m = members + (size_t)i * ENUM_MEMBER_STRIDE;
        const char *mname = ti_read_cstr(m, EMEMBER_NAME_OFF);
        if (mname == NULL || mname[0] == '\0') break;           /* loop terminator: NULL/empty name */
        uint32_t mval = 0;
        ti_read_u32(m, EMEMBER_VALUE_OFF, &mval);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s = %d,\n", mname, mval);  /* OG verbatim */
        ti_dump_append(dump, sizeof dump, &dlen, tmp);
    }
    ti_dump_append(dump, sizeof dump, &dlen, "};\n");

    ti_emit_long(dump);
    sh_printf("Dumped type is a Enum\n");
    if (sh_clipboard_set(dump))
        sh_printf("sh_type: copied enum '%s' to the clipboard.\n", type);
}

/* Filter+print one candidate C for sh_validclasses: emit if C == Y or C derives from Y (the LIVE engine
 * ancestry check, the SAME derive-rule the apply-guard uses -> the list == exactly what a Save accepts).
 * Bumps *count on emit; sets *y_seen when C == Y. */
static void vc_emit(const char *C, const char *Y, int *count, int *y_seen)
{
    if (!C || !C[0]) return;
    int is_y = (strcmp(C, Y) == 0);
    if (is_y) *y_seen = 1;
    if (is_y || sh_typeinfo_class_derives(C, Y) == 1) {
        sh_printf("  %s\n", C);
        (*count)++;
    }
}

/* [+] sh_validclasses <inherit> -- the class-dropdown ENUMERATOR. Resolve Y = the inherit's base className
 * (sh_typeinfo_inherit_base), then list every registered className that DERIVES from Y. The candidate set is
 * the LIVE reflection type registry (sh_typeinfo_collect_classnames -- every registered idTypeInfo, not a
 * frozen decl-corpus list), so classes with NO editor decl (idBillboard, idTarget_Command, ...) ARE surfaced;
 * the filter (sh_typeinfo_class_derives) is the engine's own ancestry walk, so the list == exactly what a Save
 * accepts. Portable: auto-tracks DOOM patches, ZERO hardcoded class data. Fallback: if the live registry is
 * unreachable (pre-boot), serve the static class_universe.h candidate set (the degraded, decl-corpus subset).
 * Picking an idEntity-rooted inherit (snapmaps/unknown / target/default) lists ALL entity classes. */
#define SH_REGISTRY_MAX  16384   /* candidate-buffer cap (this build ~10,190 registered types) */
void h_sh_validclasses(idCmdArgs *a)
{
    const char *inherit = cmd_argv(a, 1);
    if (inherit == NULL || !inherit[0]) {
        sh_printf("usage: sh_validclasses <inherit>   (e.g. snapmaps/unknown -> every class; the inherit's base type Y gates the list)\n");
        return;
    }
    char ybuf[256];
    const char *Y = sh_typeinfo_inherit_base(inherit, ybuf, sizeof ybuf);
    if (Y == NULL || !Y[0]) {
        sh_printf("sh_validclasses: could not resolve inherit '%s' to a base class (missing decl or empty class).\n", inherit);
        return;
    }
    sh_printf("inherit '%s' -> base type Y = '%s'; engine-valid classes (derive from Y):\n", inherit, Y);

    static const char *names[SH_REGISTRY_MAX];       /* main-thread-serial console handler -> static is safe */
    int count = 0, y_seen = 0;
    int k = sh_typeinfo_collect_classnames(names, SH_REGISTRY_MAX);
    if (k > 0) {                                       /* LIVE registry (complete + portable) */
        for (int i = 0; i < k; i++) vc_emit(names[i], Y, &count, &y_seen);
        if (k >= SH_REGISTRY_MAX)
            sh_printf("  (registry list truncated at %d -- raise SH_REGISTRY_MAX)\n", SH_REGISTRY_MAX);
    } else {                                           /* fallback: static decl-corpus candidate set */
        sh_printf("  (live type registry unavailable -- using the static candidate set)\n");
        for (int i = 0; i < SH_CLASS_UNIVERSE_N; i++) vc_emit(SH_CLASS_UNIVERSE[i], Y, &count, &y_seen);
    }
    if (!y_seen) {   /* Y itself derives from itself -- emit it if the candidate set didn't contain it */
        sh_printf("  %s\n", Y);
        count++;
    }
    sh_printf("(%d valid classes for inherit '%s')\n", count, inherit);
}

/* ------------------------------------------------------------------------------- install ---------- */

int sh_typeinfo_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */
    if (module_base == NULL) {
        backend_log("B2: typeinfo install SKIPPED -- module base NULL");
        return 0;
    }

    g_doom_base = module_base;
    g_find_type = (find_typeinfo_fn)sig_addr_by_name(results, n, "FindTypeInfoByName");
    g_find_enum = (find_enum_fn)sig_addr_by_name(results, n, "FindEnumByName");

    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: typeinfo install -- find_type=%p find_enum=%p declmgr_acc=base+0x17f7030 "
        "(cs_fieldinfo/sh_type wired)",
        (void *)g_find_type, (void *)g_find_enum);
    backend_log(line);
    return 1;
}
