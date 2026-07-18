/* cvars.c -- see cvars.h. The cvar registrar: the 9 OG cvars (clone of OG XINPUT1_3's static-init
 * cvar table + spine flush FUN_1800229b1 / FUN_180022610) + our own snaphak_user_overrides row.
 *
 * CVAR REGISTER ABI (DIRECT, from the cvar-register flush disasm @0x22610):
 *   ( CvarRegister )( self [embedded idCVar], name, default, typecode, desc, argComp )
 * We call the OUTER engine fn 0x1A04F00 (resolved as "CvarRegister"), NOT the inner idCVarSystem::
 * Register 0x1A05E70 -- the outer self-defaults the two engine .data globals, so we never touch them.
 * typecode (1=BOOL 2=INT 4=FLOAT) is passed VERBATIM as the engine `flags` arg (the engine massages the
 * bits internally). None of the 9 carry EXPOSE/NOCHEAT -> non-EXPOSE / gate-1-invisible (faithful OG).
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cvars.h"
#include "commands.h"   /* sh_decode_rip_slot / sh_safe_read -- the shared build-portable slot decoder */
#include "signatures.h"
#include "backend_log.h"

/* The engine cvar register fn: void register(void* self, const char* name, const char* def,
 *   uint32_t flags [== typecode], const char* desc, void* argComp). */
typedef void (*cvar_register_fn)(void *self, const char *name, const char *def,
                                 uint32_t flags, const char *desc, void *argComp);

/* The engine cvar NAME hash (sig "NameHash" = 0x1a00480): case-insensitive accumulator
 *   h = h*0x1f + tolower(c), the EXACT hash RegisterStaticVars (0x1a06a00) buckets each cvar with.
 * We call the engine's own hash (not a re-derive) so our findable-insert lands in the same bucket the
 * engine's gate-0 FindCvar reads. Returns the raw (unmasked) hash. */
typedef int (*name_hash_fn)(const char *name);

/* The idCVarSystem singleton + its FULL findable-table offsets.
 * ALL offsets DIRECT-confirmed against the RegisterStaticVars (0x1a06a00) disassembly: the FULL idList
 * is { ptr@+0x08, count@+0x10, capacity@+0x14 } (the +0x14 capacity is the count==cap grow gate at
 * 0x1a06af2/0x1a06b0d), the FULL idHashIndex is { hash[]@+0x38, indexChain[]@+0x40, hashSize@+0x48,
 * indexChainSize@+0x4c, hashMask@+0x54, lookupMask@+0x58 } (RegisterStaticVars' own bucket math + grow
 * gate at 0x1a06aa6/0x1a06ad4). This is the gate-0 findable table; the S0 cvar-unlock alias points the
 * gate-1 (DEV) table at it, so a findable-insert here surfaces at BOTH gates.
 *
 * cvarSys SINGLETON POINTER (the idCVarSystemLocal* .data global). Resolved build-portably like its three
 * backend siblings (cmdSystem / gameMgr / renderWorld) -- DECODE the slot from a sig'd accessor, keep the
 * base+RVA only as a logged fallback. See sh_resolve_cvarsys() below.
 *   - PRIMARY (portable): the cvarSys global sits at cmdSystem_slot + 0x10 (the two .data slots are
 *     adjacent: cmdSystem RVA 0x55b7280, cvarSys RVA 0x55b7290 == +0x10). We decode the cmdSystem slot
 *     from the CmdSystemLea sig (already in BACKEND_ENGINE_SIGNATURES -- the bot_add/bot_remove registrar
 *     whose prologue does `MOV RCX,[rip+cmdSystem]`) via the shared sh_decode_rip_slot, add 0x10, deref
 *     once. NO hardcoded RVA on this path; survives an RVA shift.
 *   - FALLBACK / RE-DERIVE recipe (auto-patcher / DOOM version bump): cvarSys = *(module_base + 0x55b7290).
 *     To re-find on a shifted build: decode the CmdSystemLea accessor's first RIP-relative load to the
 *     cmdSystem .data slot RVA, then cvarSys_RVA = that + 0x10 (== this literal on the pinned build).
 *     Mirror: cvar_unlock/engine_layout.h RVA_CVAR_SYSTEM_PTR carries the identical recipe. */
#define CVARSYS_SLOT_RVA          0x55b7290u   /* fallback only -- primary is the CmdSystemLea decode +0x10 */
#define CVARSYS_OFF_FROM_CMDSYS   0x10         /* cvarSys .data slot == cmdSystem .data slot + 0x10 (adjacent) */
#define CVARSYS_LIST_PTR_OFF      0x08    /* idList<idCVar*> base: list-ptr */
#define CVARSYS_LIST_COUNT_OFF    0x10    /* idList count (int) */
#define CVARSYS_LIST_CAP_OFF      0x14    /* idList capacity (int) -- count==cap => engine grows */
#define CVARSYS_HASH_OFF          0x38    /* idHashIndex hash[] (bucket heads, int*) */
#define CVARSYS_CHAIN_OFF         0x40    /* idHashIndex indexChain[] (int*) */
#define CVARSYS_INDEXCHAINSZ_OFF  0x4c    /* idHashIndex indexChainSize (int) -- chain[] length */
#define CVARSYS_HASHMASK_OFF      0x54    /* idHashIndex hashMask (uint) */
#define CVARSYS_LOOKUPMASK_OFF    0x58    /* idHashIndex lookupMask (uint) */

/* idCVar embedded-object name slot. CvarRegister/idCVarSystem::Register store the cvar NAME at obj+0x40
 * (the same string FindCvar/RegisterStaticVars hash via [cvar+0x40]); our self == &g_cvar_objs[i][0], so
 * the registered name lives at g_cvar_objs[i][0x40]. We hash CVARS[i].name (identical bytes) directly. */
#define IDCVAR_NAME_OFF           0x40

/* The cvar table: rows 0..8 are the 9 OG cvars, VERBATIM from our cvar-descriptor RE (name / default /
 * typecode 1=BOOL 2=INT 4=FLOAT / description); order matches the descriptor dump. Row 9
 * (snaphak_user_overrides) is OUR OWN addition (no OG counterpart) -- the user-override-layer kill
 * switch the overrides loader reads (overrides.c). */
typedef struct cvar_row {
    const char *name;
    const char *def;
    uint32_t    type;   /* 1=BOOL, 2=INT, 4=FLOAT (passed verbatim as the engine flags arg) */
    const char *desc;
} cvar_row;

static const cvar_row CVARS[] = {
    { "cs_dash_direction_multiplier",        "1.0",  4, "scale dash direction by this" },
    { "cs_dash_ground_velocity_multiplier",  "2.0",  4, "scale dash direction by this if on ground" },
    { "cs_dash_time_seconds",                "0.5",  4, "time period over which to apply the dash slices" },
    { "cs_num_dash_slices",                  "120",  2, "Num slices for applying dash velocity" },
    { "cs_mh_direction_multiplier",          "1.0",  4, "scale meathook direction by this" },
    { "cs_mh_movement_multiplier",           "10.0", 4, "scale meathook velocity by this much" },
    { "snaphak_pretty_on",                   "0",    1, "enables pretty printing of saved rawmap json" },
    { "snaphak_show_rmcount",                "0",    1, "draws the current number of rendermodels active" },
    { "snaphak_copy_reslist_to_clipboard",   "0",    1, "when sh_listres is used the contents will be copied to the clipboard" },
    { "snaphak_user_overrides",              "1",    1, "when 0, override files in your snaphak profile folder are ignored (built-in defaults and the game's own resources serve instead); use to bisect a broken override set" },
};
#define CVAR_COUNT ((int)(sizeof(CVARS) / sizeof(CVARS[0])))

/* Persistent, never-freed, 16-byte-aligned backing for the engine's embedded idCVar object. The engine
 * writes through this+0x80 and the descriptor cell spacing (~0xC0) bounds the object well under 0x400;
 * 0x400 is generous and 16-aligned for the engine's SSE init. The engine links each into its cvar list
 * for the process lifetime (OG never frees its static descriptors either), so this storage is static. */
__declspec(align(16)) static uint8_t g_cvar_objs[CVAR_COUNT][0x400];

/* One-shot latch -- CvarRegister has NO dedup (unconditional link into the list head), so a second
 * install pass would duplicate every row. Latch so OUR install fires exactly once. */
static volatile LONG g_installed = 0;

/* Register one cvar, SEH-guarded. Returns 1 on success, 0 if the engine call faulted. */
static int register_one(cvar_register_fn reg, int i)
{
    __try {
        reg(&g_cvar_objs[i][0], CVARS[i].name, CVARS[i].def, CVARS[i].type, CVARS[i].desc, NULL);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* DIRECT self-readback verification (independent of the production console's non-EXPOSE hiding):
 * idCVarSystem::Register (@0x1a05e70) populates the embedded idCVar object -- name@self+0x40,
 * default@+0x48, desc@+0x50, flags@+0x58 -- and links self+0x80 into the engine cvar list. We confirm
 * registration TOOK EFFECT by reading our (zero-initialized) backing block back: bit1 = name@+0x40 strcmp
 * matches our cvar name (a proper cvar entry); bit0 = the block mutated from zero (Register wrote into it).
 * Both together = DIRECT proof the engine registered the cvar, even though it stays gate-1-invisible. */
static int verify_one(int i)
{
    __try {
        const char *nm = *(const char * volatile *)(&g_cvar_objs[i][0x40]);
        int match = (nm != NULL && strcmp(nm, CVARS[i].name) == 0) ? 2 : 0;
        int mutated = 0;
        for (int b = 0; b < 0x100; b++) { if (g_cvar_objs[i][b]) { mutated = 1; break; } }
        return match | (mutated ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* idCVar.valueInteger offset (DIRECT, from the engine's idlib field schema): idCVar puts
 * valueInteger (and BOOL) at +0x30, cross-confirmed by the OG DAT_18003d2b8==(embedded idCVar)+0x30
 * arithmetic. Our self == &g_cvar_objs[i][0] (no descriptor wrapper), so value == *(int*)(block+0x30). */
#define IDCVAR_VALUE_INT_OFF 0x30

int sh_cvar_value_int(int index, int def)
{
    if (index < 0 || index >= CVAR_COUNT) return def;
    __try {
        return *(const volatile int32_t *)(&g_cvar_objs[index][IDCVAR_VALUE_INT_OFF]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return def;
    }
}

int sh_cvar_table_count(void)
{
    return CVAR_COUNT;
}

int sh_cvar_table_row(int index, const char **name, const char **def, const char **desc)
{
    if (index < 0 || index >= CVAR_COUNT) return 0;
    if (name) *name = CVARS[index].name;
    if (def)  *def  = CVARS[index].def;
    if (desc) *desc = CVARS[index].desc;
    return 1;
}

/* REGISTRATION-AWARE value read: like sh_cvar_value_int, but returns `def` until the engine has
 * actually populated the backing object (name@+0x40 matches our row). The plain read returns the raw
 * zero-initialized slot (0) before CvarRegister runs -- wrong for a default-1 cvar consulted on the
 * boot path (the overrides loader installs BEFORE the cvar flush, and engine resource opens fire in
 * between). This variant makes the pre-registration window read as the DEFAULT, not as 0. */
int sh_cvar_value_int_reg(int index, int def)
{
    if (index < 0 || index >= CVAR_COUNT) return def;
    __try {
        const char *nm = *(const char * volatile *)(&g_cvar_objs[index][IDCVAR_NAME_OFF]);
        if (nm == NULL || strcmp(nm, CVARS[index].name) != 0) return def;   /* not registered yet */
        return *(const volatile int32_t *)(&g_cvar_objs[index][IDCVAR_VALUE_INT_OFF]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return def;
    }
}

/* ----------------------------------------------------------------- FULL findable-table insert -----
 * THE FIX (root cause: our 9 cvars register into the pending list ONLY; the SOLE hasher
 * RegisterStaticVars (0x1a06a00) already ran at static init, so our LATE cvars are in NEITHER findable
 * table -> FindCvar misses -> "Unknown command"). After CvarRegister has built each embedded idCVar
 * object, we replay RegisterStaticVars' FULL-table insert for our 9 cvars: append the object pointer
 * into the FULL idList (cvarSys+0x08) and link it into the FULL idHashIndex (cvarSys+0x38) at the same
 * bucket the engine's hash yields. The S0 cvar-unlock alias then makes the gate-1 (~ console) table BE
 * this FULL table, so the cvars become recognized at both gates.
 *
 * We do NOT set CVAR_EXPOSE (OG's 9 are non-EXPOSE; the alias, not EXPOSE, is what gives gate-1 reach)
 * and we do NOT re-call RegisterStaticVars (its static-dup guard ExitProcess(2)es on a re-run). We BAIL
 * (logged skip, no realloc) if the FULL table has no spare room -- a ~6600-cvar table normally has a few
 * slots free; growing it would be out of scope + riskier than skipping. Every memory access is SEH-
 * guarded so a wrong offset degrades to a logged skip, never a crash/corruption. The one-shot install
 * latch guarantees this pass runs exactly once, so no cvar is double-linked.
 *
 * Returns the number of cvars inserted into the FULL table (0..9). cvarSys / hashfn NULL => 0 (logged). */
static int cvar_findable_insert_one(uint8_t *cvarSys, name_hash_fn hashfn, int i)
{
    __try {
        const char *name = CVARS[i].name;                       /* == the obj+0x40 name CvarRegister stored */
        void       *obj  = (void *)&g_cvar_objs[i][0];          /* our embedded idCVar object */

        int  count = *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
        int  cap   = *(volatile int *)(cvarSys + CVARSYS_LIST_CAP_OFF);
        int  ics   = *(volatile int *)(cvarSys + CVARSYS_INDEXCHAINSZ_OFF);
        if (count >= cap || count >= ics)
            return -1;                                          /* no spare room -- bail (caller logs) */

        /* idList append: list[count] = obj; count++  (mirrors 0x1a06b16-0x1a06b1e). The list-ptr SLOT is
         * read once (we hold the one-shot install latch -> single writer), SEH-guarded by the __try. */
        void **list = *(void ***)(cvarSys + CVARSYS_LIST_PTR_OFF);
        list[count] = obj;
        *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF) = count + 1;

        /* idHashIndex::Add: hb = (h & hashMask) & lookupMask; chain[count] = bucket[hb]; bucket[hb] = count.
         * The masked-bucket VALUE equals RegisterStaticVars' own LOOKUP/dup-check math at 0x1a06a5d/0x1a06a76
         * (`lookupMask & hashMask & (h&hashMask)`). RegisterStaticVars' INSERT site (the chain/bucket writes
         * at 0x1a06ad4-0x1a06aee) uses h&hashMask WITHOUT lookupMask -- but lookupMask is 0xFFFFFFFF on the
         * live populated table (the grow fn always sets it all-ones; only an empty/disabled table differs,
         * impossible for the 6600+-cvar system), so our hb is the SAME bucket the engine writes AND reads. */
        unsigned h    = (unsigned)hashfn(name);
        unsigned mask = *(volatile unsigned *)(cvarSys + CVARSYS_HASHMASK_OFF);
        unsigned look = *(volatile unsigned *)(cvarSys + CVARSYS_LOOKUPMASK_OFF);
        unsigned hb   = (h & mask) & look;
        int *bucket = *(int **)(cvarSys + CVARSYS_HASH_OFF);
        int *chain  = *(int **)(cvarSys + CVARSYS_CHAIN_OFF);
        chain[count] = bucket[hb];
        bucket[hb]   = count;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                                               /* a bad offset -> logged skip, never crash */
    }
}

/* ------------------------------------------------------------------ cvarSys-global decode ---------
 * Resolve the idCVarSystemLocal* global build-portably -- the SAME pattern as its three siblings
 * (sh_resolve_cmdsys / sh_resolve_gamemgr_slot / dr_resolve_renderworld): decode the slot from a sig'd
 * accessor, deref once, keep the base+RVA only as a logged fallback. cvarSys is NOT directly sig'd, but
 * its .data slot is exactly cmdSystem_slot + 0x10 (adjacent globals), and cmdSystem IS sig-decodable via
 * CmdSystemLea -> so we decode the cmdSystem slot, add 0x10, deref once. Returns the live cvarSys object
 * pointer (the address whose +0x08/+0x38/... findable tables we insert into), or NULL on any failure.
 * Re-resolves CmdSystemLea from BACKEND_ENGINE_SIGNATURES by name (same single-source-of-truth pattern as
 * the NameHash resolve below -- no pattern duplication). Every access is SEH-guarded via sh_safe_read. */
static uint8_t *sh_resolve_cvarsys(const uint8_t *module_base)
{
    char line[160];

    /* PRIMARY (portable): decode the cmdSystem slot from the CmdSystemLea sig, +0x10 -> cvarSys slot, deref. */
    if (module_base) {
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
            if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "CmdSystemLea") != 0) continue;
            sig_result one;
            sig_status st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &one);
            if (st != SIG_OK && st != SIG_OK_HOOKED) break;   /* sig miss -> fall through to known-RVA */
            const uint8_t *cmdsys_slot = sh_decode_rip_slot((const uint8_t *)one.addr);
            if (!cmdsys_slot) break;
            const uint8_t *cvarsys_slot = cmdsys_slot + CVARSYS_OFF_FROM_CMDSYS;   /* adjacent .data global */
            uint8_t *obj = NULL;
            if (sh_safe_read(cvarsys_slot, (uint8_t *)&obj, sizeof obj) && obj) {
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cvarSys decoded slot=%p (cmdSystem+0x10) -> obj=%p (portable)",
                    (void *)cvarsys_slot, (void *)obj);
                backend_log(line);
                return obj;
            }
            break;
        }
        backend_log("B2: cvarSys portable decode failed -- trying known-offset fallback");
    }

    /* FALLBACK (hook-tolerance philosophy: portable primary + known_rva fallback / re-derive recipe). */
    if (module_base) {
        uint8_t *obj = NULL;
        __try {
            obj = *(uint8_t * volatile *)(module_base + CVARSYS_SLOT_RVA);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            obj = NULL;
        }
        if (obj) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cvarSys fallback *(base+0x55b7290)=%p", (void *)obj);
            backend_log(line);
            return obj;
        }
    }
    backend_log("B2: cvarSys UNRESOLVED -- cvar findable-insert will be skipped");
    return NULL;
}

int sh_cvars_install(void *cvar_register, const void *module_base)
{
    char line[160];

    if (cvar_register == NULL) {
        backend_log("B2: cvars SKIPPED -- CvarRegister not resolved");
        return 0;
    }
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        backend_log("B2: cvars already registered (one-shot latch) -- skipping double-register");
        return 0;
    }

    cvar_register_fn reg = (cvar_register_fn)cvar_register;
    int ok = 0;
    for (int i = 0; i < CVAR_COUNT; i++)
        ok += register_one(reg, i);

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: cvars registered %d/%d (register=%p, non-EXPOSE / gate-1-invisible; 9 OG rows + snaphak_user_overrides)",
        ok, CVAR_COUNT, cvar_register);
    backend_log(line);

    /* DIRECT engine-populated readback (proves registration despite the production console hiding
     * non-EXPOSE cvars as "Unknown command"). */
    int matched = 0, mutated = 0;
    for (int i = 0; i < CVAR_COUNT; i++) {
        int r = verify_one(i);
        if (r & 2) matched++;
        if (r & 1) mutated++;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: cvar readback -- %d/%d name@+0x40 matched, %d/%d object-mutated (DIRECT engine-populated proof)",
        matched, CVAR_COUNT, mutated, CVAR_COUNT);
    backend_log(line);

    /* THE FIX: link our 9 cvars into the FULL findable table so FindCvar (and the S0-aliased gate-1 ~
     * console) recognizes them. Resolve cvarSys build-portably (CmdSystemLea decode +0x10, base+RVA
     * fallback -- sh_resolve_cvarsys), resolve the engine name-hash, then insert each. */
    uint8_t *cvarSys = (uint8_t *)sh_resolve_cvarsys((const uint8_t *)module_base);
    name_hash_fn hashfn = NULL;
    if (module_base) {
        /* Resolve the engine cvar name-hash from the shipped DB by name (no pattern duplication --
         * single source of truth in BACKEND_ENGINE_SIGNATURES). Both a clean scan and the hook-tolerant
         * known_rva fallback count (the fn is present + callable either way). */
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
            if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "NameHash") != 0) continue;
            sig_result one;
            sig_status st = sig_resolve_one((const uint8_t *)module_base,
                                            &BACKEND_ENGINE_SIGNATURES[i], &one);
            if (st == SIG_OK || st == SIG_OK_HOOKED)
                hashfn = (name_hash_fn)one.addr;
            break;
        }
    }

    int inserted = 0, full_skips = 0, faults = 0;
    if (cvarSys == NULL || hashfn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: cvar findable-insert SKIPPED -- cvarSys=%p hashfn=%p (module_base=%p)",
            (void *)cvarSys, (void *)hashfn, (void *)module_base);
        backend_log(line);
    } else {
        for (int i = 0; i < CVAR_COUNT; i++) {
            int r = cvar_findable_insert_one(cvarSys, hashfn, i);
            if (r == 1) inserted++;
            else if (r == -1) full_skips++;
            else faults++;
        }
        int count_after = -1, cap_after = -1;
        __try {
            count_after = *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
            cap_after   = *(volatile int *)(cvarSys + CVARSYS_LIST_CAP_OFF);
        } __except (EXCEPTION_EXECUTE_HANDLER) { count_after = -1; cap_after = -1; }
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: cvar findable-insert %d/%d (cvarSys=%p count=%d cap=%d full-skip=%d fault=%d)",
            inserted, CVAR_COUNT, (void *)cvarSys, count_after, cap_after, full_skips, faults);
        backend_log(line);
    }
    return ok;
}
