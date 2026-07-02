/* signatures.c -- see signatures.h. C port of the reference implementation's signature table for the SnapHak backend.
 *
 * The scanner mirrors the reference resolver byte-for-byte in behaviour: compile an IDA-style hex
 * pattern to (pattern,mask), find the longest fixed (mask==0xFF) run as a memmem anchor, then verify
 * the full masked pattern at each anchor hit. The only structural difference is that the reference
 * version reads .text out of a file image (RVA<->file-offset) while this reads the LIVE mapped DOOM
 * module (each section's bytes sit at module_base + section.VirtualAddress), because the resolver runs
 * in-process against the DOOM module our backend DLL is loaded into.
 *
 * Scope of the scan: every executable section (Characteristics & IMAGE_SCN_MEM_EXECUTE), not just a
 * section literally named ".text" -- DOOM is SteamStub-unpacked and section names are not guaranteed,
 * and the engine code we sign lives in the executable image regardless of name. Uniqueness is still
 * enforced across the whole executable range, so a false second match anywhere reports AMBIGUOUS.
 */
#include "signatures.h"
#include <string.h>

/* ------------------------------------------------------------------ pattern compile + match ------ */

#define SIG_MAX_PATTERN 256   /* longest signature byte length we support (DB max is ~40 bytes) */

static int parse_token(const char *tok, size_t len, uint8_t *b, uint8_t *m)
{
    if (len == 1 && tok[0] == '?') { *b = 0; *m = 0; return 1; }
    if (len == 2 && tok[0] == '?' && tok[1] == '?') { *b = 0; *m = 0; return 1; }
    if (len != 2) return 0;
    int hi = -1, lo = -1;
    for (int i = 0; i < 2; i++) {
        char c = tok[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        if (i == 0) hi = v; else lo = v;
    }
    *b = (uint8_t)((hi << 4) | lo);
    *m = 0xFF;
    return 1;
}

static size_t compile_pattern(const char *pattern, uint8_t *pat, uint8_t *mask, size_t cap)
{
    size_t n = 0;
    const char *p = pattern;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - tok);
        if (n >= cap) return 0;
        if (!parse_token(tok, len, &pat[n], &mask[n])) return 0;
        n++;
    }
    return n;
}

static void longest_fixed_run(const uint8_t *mask, size_t n, size_t *off, size_t *len)
{
    size_t best_off = 0, best_len = 0, cur_off = 0, run = 0;
    for (size_t i = 0; i < n; i++) {
        if (mask[i]) {
            if (run == 0) cur_off = i;
            run++;
            if (run > best_len) { best_len = run; best_off = cur_off; }
        } else {
            run = 0;
        }
    }
    *off = best_off; *len = best_len;
}

static int matches_at(const uint8_t *blob, const uint8_t *pat, const uint8_t *mask, size_t n, size_t i)
{
    for (size_t k = 0; k < n; k++)
        if (mask[k] && blob[i + k] != pat[k]) return 0;
    return 1;
}

static long find_bytes(const uint8_t *blob, size_t end, const uint8_t *needle, size_t nn, size_t start)
{
    if (nn == 0 || nn > end) return -1;
    size_t last = end - nn;
    for (size_t i = start; i <= last; i++) {
        if (blob[i] == needle[0] && memcmp(blob + i, needle, nn) == 0)
            return (long)i;
    }
    return -1;
}

/* ----------------------------------------------------------------------- PE executable sections -- */

typedef struct exec_section {
    const uint8_t *base;   /* module_base + VirtualAddress (mapped) */
    uint32_t       vaddr;  /* RVA of the section */
    uint32_t       vsize;  /* mapped size to scan */
} exec_section;

#define MAX_SECTIONS 64

static int collect_exec_sections(const uint8_t *module_base, exec_section *out, int cap)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(module_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return -1;

    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = nt->FileHeader.NumberOfSections;
    int count = 0;
    for (int i = 0; i < n && count < cap; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t vsize = sec[i].Misc.VirtualSize;
        if (vsize == 0) vsize = sec[i].SizeOfRawData;
        if (vsize == 0) continue;
        out[count].base  = module_base + sec[i].VirtualAddress;
        out[count].vaddr = sec[i].VirtualAddress;
        out[count].vsize = vsize;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------------ hook-tolerant fallback --
 * When the masked-byte scan misses a function because its PROLOGUE was inline-hooked at runtime (the
 * stolen first bytes were overwritten with a detour jump -- an external instrumentation tool does exactly this
 * to DeserializeFromJson / AddCommand / MenuPump during testing), the function is still present and
 * callable (the detour trampolines through to the original). So if the scan finds 0 matches, we fall
 * back to the DB's known_rva: if the bytes there start with a detour jump AND the sig's fixed-byte TAIL
 * (past the stolen prologue window) still matches the live bytes, the function is present-but-hooked and
 * we resolve at known_rva. This is BUILD-SPECIFIC (known_rva is the extraction build's RVA) -- it is a
 * deliberate fallback that fires ONLY on a detected hook; the portable scan stays primary. */

#define HOOK_MAX_STEAL   24   /* widest plausible whole-instruction steal window a detour overwrites */
#define HOOK_MIN_TAIL     6   /* require at least this many FIXED tail bytes to match (anti-coincidence) */

/* Is the byte at p[0..] the start of an x64 unconditional jmp a detour installer would write?
 *   E9 rel32 | EB rel8 | FF 25 [rip+disp32] (abs indirect) | FF /4 reg/mem jmp. Returns the opcode
 *   length consumed for the "stolen window starts here" purpose (best-effort; the tail slide is the
 *   real validator). 0 if not a recognized jmp opcode. */
static int detour_jmp_len(const uint8_t *p)
{
    if (p[0] == 0xE9) return 5;                    /* jmp rel32 */
    if (p[0] == 0xEB) return 2;                    /* jmp rel8  */
    if (p[0] == 0xFF) {
        uint8_t modrm = p[1];
        uint8_t reg   = (uint8_t)((modrm >> 3) & 7);
        if (reg == 4) {                            /* FF /4 = jmp r/m64 (incl. FF 25 rip-relative) */
            if ((modrm & 0xC7) == 0x25) return 6;  /* FF 25 disp32 (the >2GB abs-jmp our hook.c uses) */
            return 2;                              /* other jmp r/m forms (reg/[reg]); slide finds tail */
        }
    }
    /* REX-prefixed FF /4 (rare for a detour, but be lenient) */
    if ((p[0] & 0xF0) == 0x40 && p[1] == 0xFF && ((p[2] >> 3) & 7) == 4) {
        if ((p[2] & 0xC7) == 0x25) return 7;
        return 3;
    }
    return 0;
}

/* SEH-guarded copy of up to n bytes from src into dst; returns 1 if all read, 0 on access violation. */
static int sig_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Try the known_rva hook-tolerant fallback. Returns 1 (and fills out as SIG_OK_HOOKED) if the live
 * bytes at module_base+known_rva are a detour whose post-prologue tail matches the sig; else 0. */
static int try_hooked_known_rva(const uint8_t *module_base, const sig_entry *sig,
                                const uint8_t *pat, const uint8_t *mask, size_t n, sig_result *out)
{
    if (sig->known_rva == 0) return 0;
    const uint8_t *site = module_base + sig->known_rva;

    uint8_t live[SIG_MAX_PATTERN];
    size_t  want = n < SIG_MAX_PATTERN ? n : SIG_MAX_PATTERN;
    if (!sig_safe_read(site, live, want)) return 0;       /* unreadable -> not a confident hook hit */

    int jlen = detour_jmp_len(live);
    if (jlen == 0) return 0;                               /* not detoured -> the fn genuinely moved */

    /* The detour stole a whole-instruction window >= jlen. Slide the steal offset k and require the
     * sig's FIXED tail (mask==0xFF positions from k to n) to match the live bytes, with enough fixed
     * tail bytes to rule out coincidence. */
    for (size_t k = (size_t)jlen; k <= HOOK_MAX_STEAL && k < n; k++) {
        int fixed_tail = 0, ok = 1;
        for (size_t j = k; j < n; j++) {
            if (!mask[j]) continue;
            fixed_tail++;
            if (live[j] != pat[j]) { ok = 0; break; }
        }
        if (ok && fixed_tail >= HOOK_MIN_TAIL) {
            out->status = SIG_OK_HOOKED;
            out->addr   = (uintptr_t)site;
            out->rva    = sig->known_rva;
            return 1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------------------- resolve ----- */

sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{
    out->name = sig->name;
    out->status = SIG_NOT_FOUND;
    out->addr = 0;
    out->rva = 0;

    uint8_t pat[SIG_MAX_PATTERN], mask[SIG_MAX_PATTERN];
    size_t n = compile_pattern(sig->pattern, pat, mask, SIG_MAX_PATTERN);
    if (n == 0) { out->status = SIG_BAD_PATTERN; return out->status; }

    exec_section secs[MAX_SECTIONS];
    int nsec = collect_exec_sections(module_base, secs, MAX_SECTIONS);
    if (nsec < 0) { out->status = SIG_BAD_MODULE; return out->status; }

    size_t a_off, a_len;
    longest_fixed_run(mask, n, &a_off, &a_len);
    if (a_len == 0) { out->status = SIG_BAD_PATTERN; return out->status; }  /* all-wildcard: not a real sig */
    const uint8_t *anchor = pat + a_off;

    uintptr_t found_addr = 0;
    uint32_t  found_rva = 0;
    int       hits = 0;

    for (int s = 0; s < nsec && hits < 2; s++) {
        const uint8_t *blob = secs[s].base;
        uint32_t       size = secs[s].vsize;
        if (size < n) continue;
        size_t scan_end = size;
        size_t pos = a_off;
        __try {
            for (;;) {
                long apos = find_bytes(blob, scan_end, anchor, a_len, pos);
                if (apos < 0) break;
                long start = apos - (long)a_off;
                if (start < 0) { pos = (size_t)apos + 1; continue; }
                if ((size_t)start + n > scan_end) break;
                if (matches_at(blob, pat, mask, n, (size_t)start)) {
                    hits++;
                    if (hits == 1) {
                        found_rva  = secs[s].vaddr + (uint32_t)start;
                        found_addr = (uintptr_t)(blob + start);
                    } else {
                        break;
                    }
                }
                pos = (size_t)apos + 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* uncommitted section tail -- treat what we found so far as authoritative */
        }
    }

    if (hits == 0) {
        /* Scan missed. Before declaring NOT_FOUND, try the hook-tolerant known_rva fallback: the
         * prologue may be inline-hooked (an external instrumentation tool may do this to a few engine fns
         * during testing), so the fixed bytes the scan needs are overwritten -- but the fn is present + callable. */
        if (try_hooked_known_rva(module_base, sig, pat, mask, n, out))
            return out->status;   /* SIG_OK_HOOKED */
        out->status = SIG_NOT_FOUND;
        return out->status;
    }
    if (hits > 1)  { out->status = SIG_AMBIGUOUS; return out->status; }
    out->status = SIG_OK;
    out->addr = found_addr;
    out->rva = found_rva;
    return SIG_OK;
}

size_t sig_db_count(void)
{
    size_t n = 0;
    while (BACKEND_ENGINE_SIGNATURES[n].name != NULL) n++;
    return n;
}

size_t sig_resolve_all(const uint8_t *module_base, sig_result *results, size_t cap)
{
    size_t ok = 0;
    size_t i = 0;
    for (; BACKEND_ENGINE_SIGNATURES[i].name != NULL && i < cap; i++) {
        sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &results[i]);
        /* Both a clean scan hit and a hook-tolerant known_rva hit count as resolved (present+callable). */
        if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED) ok++;
    }
    return ok;
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if ((results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED) &&
            results[i].name && strcmp(results[i].name, name) == 0)
            return results[i].addr;
    return 0;
}

/* ------------------------------------------------------------ the shipped engine signature DB ----
 * Verbatim port of the reference implementation's ENGINE_SIGNATURES table (the authoring source). Each `pattern`
 * is the same masked-byte string; `known_rva` is the extraction-build RVA (validation/documentation
 * only -- the scanner re-finds the function with NO hardcoded RVA). Regenerated by the project's
 * signature extractor from an unpacked DOOM image when the DOOM build changes; do not hand-edit. */
const sig_entry BACKEND_ENGINE_SIGNATURES[] = {
    { "DeserializeFromJson",
      "40 55 56 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 C7 44 24 68 FE FF FF FF",
      0x5EA490u },
    { "SerializeToJson",
      "40 53 56 57 48 81 EC E0 00 00 00 48 C7 44 24 70 FE FF FF FF",
      0x5F2390u },
    { "AddCommand",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
      "41 54 41 56 41 57 48 83 EC 20 48 63 69 10",
      0x1AA3630u },
    { "SpawnByEntityDef",
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 49 8B C0",
      0x315AF0u },
    { "NameHash",          /* cvar NAME hash (0x1a00480): case-insensitive h = h*0x1f + tolower(c), the
                            * accumulator RegisterStaticVars (0x1a06a00) buckets each cvar into the FULL
                            * idHashIndex (cvarSys+0x38) with. The cvar findable-insert calls THIS to place
                            * our 9 late-registered cvars into the gate-0 findable table (the S0 alias then
                            * makes it the gate-1 table). Fingerprint = imul r8d,r8d,0x1f (45 6B C0 1F) +
                            * the lea/cmp dl,0x19 tolower branch; the two rel8 disps (74 ??, 77 ??) wild.
                            * Unique at 43 bytes; pure leaf, no calls. */
      "0F B6 01 45 33 C0 4C 8B C9 84 C0 74 ?? 0F 1F 00 8D 50 BF 80 FA 19 77 ?? "
      "04 20 45 6B C0 1F 49 FF C1 0F BE C8 41 0F B6 01 44 03 C1",
      0x1A00480u },
    { "GetDeclsOfType",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB 74 ?? "
      "0F 1F 80 00 00 00 00 48 8B 4B 10",
      0x1800D20u },
    { "GameMgrLea",        /* thin RET-leaf bool getter (0xb10870) whose prologue loads the
                            * gameMgr global via MOV RAX,[rip+gameMgr] (48 8B 05, the FIRST decode-target
                            * opcode, byte offset 0), then CMP [RAX+0xA54A0],0 / SETZ. Decode = rip_next +
                            * disp32 -> gameMgr-global SLOT (RVA 0x56ffb90, == OG *(engineBase+0x56ffb90));
                            * then DEREFERENCE ONCE: g_gamemgr = *(void**)slot. sh_resolve_gamemgr reuses
                            * sh_decode_rip_slot (the 4-opcode scanner shared from sh_commands -- catches
                            * 48 8B 05 at offset 0). Fallback: *(g_doom_base+0x56ffb90). 18-byte sig. */
      "48 8B 05 ?? ?? ?? ?? 48 83 B8 A0 54 0A 00 00 0F 94 C0",
      0xB10870u },
    { "MenuThink",
      "48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 48 C7 44 24 38 FE FF FF FF",
      0x1718600u },
    { "MenuPump",
      "40 56 41 57 48 81 EC 18 01 00 00",
      0x1702BA0u },
    { "AddToSelection",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B A9 88 00 00 00",
      0x59F210u },
    { "ClearSelection",
      "45 33 D2 44 39 91 88 00 00 00",
      0x59FA00u },
    { "PasteInstantiate", /* FUN_14054f950 -- the engine paste-INSTANTIATE consumer: void(prefab=editor+0x209a8,
                           * editor). Instantiates each staged-prefab entity into the live map (register +
                           * AddToSelection 0x59f210) at a camera-relative grab transform; does NOT consume the
                           * slot. The clone's create-from-scratch timeline (sh_timeline.cpp SPAWN -> kind=2) calls
                           * it AFTER ae_mkcmd_one stages the prefab into editor+0x209a8. 28-byte zero-wildcard
                           * prologue (mov rax,rsp; push rbp/rsi/rdi/r12-r15; lea rbp,[rax-0x858]; sub rsp,0x920),
                           * capstone scan HITS:1. RE'd DIRECT from our own decompile. */
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 A8 F7 FF FF 48 81 EC 20 09 00 00",
      0x54F950u },
    { "ConnectOutputCreator", /* FUN_140cdbb40 -- the editor wire tool's connect creator (a vtable-dispatched
                               * FSM leaf reached on the target pick). wiring_mode.c inline-detours it (Hook 2
                               * of the interactive wire-any mode): in wire-mode it records the target into the
                               * tool's chain slot 1 + sets the direct-edge flags, so the tool's trailing
                               * finalize lays a direct source->target edge for ANY target -- including a
                               * node-less target the stock creator would refuse (the detour is flag-gated +
                               * off by default, so OFF is a transparent passthrough). ABI: void(tool, world,
                               * idx[int]); world+0x204c8 = the editor entity table. Unique @ 34-byte prologue
                               * (3 reg-saves + MOV R9,[RDX+0x204c8] + MOVSXD R10,[RCX+0x10] -- distinct from
                               * the 3 sibling creators cdb610/cdb860/cdb990). Re-derive (per DOOM build):
                               * decompile FUN_140cdbb40. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
      "4C 8B 8A C8 04 02 00 48 8B EA 4C 63 51 10",
      0xCDBB40u },
    { "WireConnectCreator1", /* FUN_140cdb990 -- the editor wire tool's connect creator for an OUTPUT-NODE
                              * source (the pick processor's creator-selector 1, vs cdbb40's selector 0 for a
                              * base-entity source). wiring_mode.c inline-detours it (Hook 3 of the interactive
                              * wire-any mode): when the source is an output node, the target pick reaches THIS
                              * creator; in wire-mode it records the target into the tool's chain slot 0x18
                              * (the source is already in slot 0x14) + sets the direct-edge flags, so the
                              * tool's trailing finalize lays a direct source->target edge for ANY target --
                              * including a node-less target (e.g. a timeline) the stock creator would
                              * node-mediate (the "which input" radial). The detour is flag-gated + off by
                              * default, so OFF is a transparent passthrough. ABI: void(tool, world, idx[int]);
                              * world+0x204c8 = the editor entity table. Unique @ a 52-byte body: the generic
                              * save prologue + MOVSXD RDI,R8D (49 63 F8 -- distinct from the 3 sibling
                              * creators cdb610\cdb860\cdbb40) + the source/world deref chain (world+0x204c8 ->
                              * +0x6a0 -> entity, TEST +0x164,0x20); the 4 wildcard bytes are the rel32 jz disp
                              * (build-volatile). Re-derive (per DOOM build): decompile FUN_140cdb990. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 63 F8 48 8B F2 48 8B D9 83 FF FF "
      "0F 84 ?? ?? ?? ?? 48 8B 8A C8 04 02 00 48 8B 81 A0 06 00 00 48 8B 04 F8 F6 80 64 01 00 00 20",
      0xCDB990u },
    { "WireOutputState",   /* FUN_140cdaa30 -- the editor wire tool's output-select FSM leaf (reached after
                            * the first/source pick; the stock leaf raises a modal output-node picker).
                            * wiring_mode.c inline-detours it (Hook 1 of the interactive wire-any mode): in
                            * wire-mode it records the source, selects the direct-edge creator, advances the
                            * tool to target-select (think-state 2), and returns WITHOUT raising the modal --
                            * so the tool's input/camera/escape handling stays alive (a 0 think-state with the
                            * tool active would swallow input). The detour is flag-gated + off by default, so
                            * OFF is a transparent passthrough. ABI: void(tool, a, world). Unique @ 49 bytes:
                            * the generic save prologue + MOVZX EAX,[RCX+0xd] + 3 reg-moves + TEST AL,0x40,
                            * then (past the build-volatile rel8 branch, masked) MOV RAX,[R8+0x21088] + LEA
                            * R9,[RCX+0x48] + MOV R8D,3 -- the category-3 output-picker fingerprint that
                            * distinguishes it from its sibling leaves cda910/cdab30 (which share the
                            * prologue). Scan HITS:1 @ 0xcdaa30 vs an unpacked DOOM image; the 1 wildcard is
                            * the rel8 jnz disp. Re-derive (per DOOM build): re-extract the prologue. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 41 0D 49 8B F8 48 8B F2 48 8B D9 A8 40 "
      "75 ?? 49 8B 80 88 10 02 00 4C 8D 49 48 41 B8 03 00 00 00",
      0xCDAA30u },
    { "ToolReset",         /* FUN_140cdb3e0 -- the wire tool reset: sets the four pick slots
                            * (tool+0x10..+0x1c) to -1 and clears the flag/picker-result fields. wiring_mode
                            * calls it after each handled pick to keep the tool clean (and on OFF to clear a
                            * half-done pick). ABI: void(tool). Unique @ 27-byte prologue (xor r10d,r10d;
                            * lea rax,[rcx+0x10]; lea rdx,[rax+0x10]; mov r9d,4; cmp rax,rdx; mov r8d,r10d;
                            * cmova r9d,r10d -- stops before the build-volatile rel8 branch); zero wildcards,
                            * file-wide unique (HITS:1 @ 0xcdb3e0). Re-derive (per DOOM build): re-extract. */
      "45 33 D2 48 8D 41 10 48 8D 50 10 41 B9 04 00 00 00 48 3B C2 45 8B C2 45 0F 47 CA",
      0xCDB3E0u },
    /* --- wire-tool pick-RECLAIM primitives (called, not detoured) used by wiring_mode.c to convert an
     * in-progress wire pick into a clean wire-any cycle when the toggle flips ON mid-pick. Each is
     * void(tool, editor), reads only the editor (editor+0x204c8 = the entity table / +0x204d0 = the
     * selection object), and self-guards on its own tool flag so calling it out of state is a no-op.
     * Re-derive (per DOOM build): decompile each named RVA. --- */
    { "WireUndoPrev",      /* FUN_140cd9a90 -- removes the ONE current preview edge, switching on the undo
                            * class tool+0x24 (7 cases, each removing a slot-pair edge via 5a72a0) then
                            * clearing tool+0x24. wiring_mode.c calls it FIRST in the sanitize to reclaim the
                            * dragged preview edge. Unique @ 24-byte window: the generic save prologue
                            * (48 89 5C 24 08 / 57 / 48 83 EC 20) + MOV EAX,[RCX+0x24] (the undo-class load,
                            * 8B 41 24) + MOV RDI,RDX + DEC EAX + MOV RBX,RCX + CMP EAX,6 (the 7-case switch
                            * bound, 83 F8 06) -- distinct from the sibling reclaim leaves. 0 wildcards. */
      "48 89 5C 24 08 57 48 83 EC 20 8B 41 24 48 8B FA FF C8 48 8B D9 83 F8 06",
      0xCD9A90u },
    { "WireDelListenerNode", /* FUN_140cda210 -- if tool+0xc & 0x40 (an auto-created listener node exists),
                            * deletes that node entity at slot1 (tool+0x14): cdb350(entityTable, idx) removes
                            * it from the entity-index list, 59fda0(selObj, idx) rebuilds the selection, then
                            * slot1=-1 and clears tool+0xc&0x40. wiring_mode.c calls it in the sanitize so the
                            * converted pick leaves no orphan node in the map. Unique @ 25-byte window: the
                            * save prologue + TEST byte[RCX+0xc],0x40 (F6 41 0C 40 -- the listener flag) +
                            * MOV RDI,RDX + MOV RBX,RCX + JE (rel8, wildcarded) + MOV EDX,[RCX+0x14] (the
                            * slot1 load, 8B 51 14). The test-immediate (0x40 at +0x0c) + slot offset (0x14)
                            * disambiguate it from the near-twin action-node deleter (0x02 at +0x0d, 0x1c). */
      "48 89 5C 24 08 57 48 83 EC 20 F6 41 0C 40 48 8B FA 48 8B D9 74 ?? 8B 51 14",
      0xCDA210u },
    { "WireDelActionNode", /* FUN_140cda2b0 -- the mirror of WireDelListenerNode: if tool+0xd & 2 (an
                            * auto-created action node exists), deletes that node entity at slot3 (tool+0x1c)
                            * via the same cdb350 + 59fda0 pair, then slot3=-1 and clears tool+0xd&2. Unique @
                            * 25-byte window differing from the listener deleter only in the distinguishing
                            * bytes: TEST byte[RCX+0xd],2 (F6 41 0D 02) + MOV EDX,[RCX+0x1c] (the slot3 load,
                            * 8B 51 1C); the JE rel8 is wildcarded. */
      "48 89 5C 24 08 57 48 83 EC 20 F6 41 0D 02 48 8B FA 48 8B D9 74 ?? 8B 51 1C",
      0xCDA2B0u },
    { "WireClrMark2",      /* FUN_140cdbe00 -- if tool+0xd & 8, clears the slot2 highlight bit in the editor
                            * entity-table "marked" bitmask (editor+0x204c8 -> +0x6e0), then clears tool+0xd&8.
                            * NO entity is deleted (a cosmetic highlight clear only); wiring_mode.c calls it
                            * best-effort in the sanitize for exactness. The function starts DIRECTLY with the
                            * test (no frame prologue), so uniqueness comes from the body: TEST byte[RCX+0xd],8
                            * (F6 41 0D 08) + JE rel8 (wildcarded) + MOV R9D,[RCX+0x18] (the slot2 load,
                            * 44 8B 49 18) + MOV RAX,[RDX+0x204c8] (the entity-table deref). The test-immediate
                            * (8) + slot offset (0x18) disambiguate it from the near-twin WireClrMark1 (4, 0x14). */
      "F6 41 0D 08 74 ?? 44 8B 49 18 48 8B 82 C8 04 02 00",
      0xCDBE00u },
    { "WireClrMark1",      /* FUN_140cdbe40 -- the mirror of WireClrMark2: if tool+0xd & 4, clears the slot1
                            * highlight bit in the same editor+0x6e0 bitmask, then clears tool+0xd&4. NO entity
                            * delete. Unique @ 17-byte window differing only in the distinguishing bytes: TEST
                            * byte[RCX+0xd],4 (F6 41 0D 04) + MOV R9D,[RCX+0x14] (the slot1 load, 44 8B 49 14);
                            * the JE rel8 is wildcarded. Best-effort (optional) in wiring_mode.c. */
      "F6 41 0D 04 74 ?? 44 8B 49 14 48 8B 82 C8 04 02 00",
      0xCDBE40u },
    { "Toast",
      "40 57 48 83 EC 20 48 8B F9 48 8B 89 F0 08 00 00",
      0xCFA0B0u },
    { "IdStrCtor",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B DA 48 89 01 48 8B F9",
      0x19FCEF0u },
    { "IdStrDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 51 10",
      0x19FD120u },
    { "EntityDefCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 E8 ?? ?? ?? ?? 90 48 8D 8B 68 01 00 00",
      0x5E9400u },
    { "EntityDefDtor",
      "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 48 81 C1 30 01 00 00",
      0x17ACE70u },
    { "DeclSourceRebuild",
      "48 85 D2 0F 84 ?? ?? ?? ?? 55 56 57 41 56 41 57 48 81 EC C0 01 00 00",
      0x17AE560u },
    { "IdStrAssign",
      "40 53 48 83 EC 20 48 8B D9 45 33 C9 48 8D 0D ?? ?? ?? ??",
      0x1A03E10u },
    { "Lexer",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 41 0F B6 D9",
      0x1A5CD90u },
    { "StructDeserialize",
      "4C 8B DC 57 48 83 EC 70 49 C7 43 C8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x1A1D450u },
    { "LexCtxCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 33 C0 "
      "C7 41 10 00 00 05 00 48 89 01 48 89 41 08 C7 41 28 00 00 05 00 48 89 41 18 "
      "48 89 41 20 48 83 C1 30",
      0x1A5BB70u },
    { "ParseNodeCtor",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 89 51 08",
      0x1A41400u },
    { "ParseNodeDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 8B 41 08",
      0x1A41640u },
    { "BufferCommandText",
      "83 B9 A8 00 02 00 00 41 B8 40 00 00 00",
      0x1AA3780u },
    { "EntityClone",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 50 01 00 00",
      0x5A6460u },
    { "StructSerialize",
      "4C 8B DC 57 48 81 EC 80 00 00 00 49 C7 43 B8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x1A21B40u },
    { "TreeRenderJson",
      "40 57 48 81 EC E0 00 00 00 48 C7 44 24 28 FE FF FF FF",
      0x1A43730u },
    /* --- strids #str_ injector engine fns (port of OG FUN_18000FF10) --- */
    { "StridsSortBody",    /* idLangDict radix-sort body (0x1a2b480 wrapper -> JMP here); detour target */
      "48 89 5C 24 18 55 56 57 48 8D AC 24 10 F8 FF FF",
      0x1A2B490u },
    { "StridsTableLea",    /* idLangDict::GetIndexForId -- carries LEA RCX,[the strids idList global]; the
                            * build-portable anchor we decode to find the string-table descriptor global */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 ?? 80 3A 00",
      0x1A2ACD0u },
    { "StridsInsert",      /* idList<StridEntry>::Append (32-byte element). Long sig: a near-twin idList::
                            * Append shares the prologue; uniqueness only at the element-copy body (+74B). */
      "48 89 5C 24 08 57 48 83 EC 20 8B 41 0C 48 8B FA 48 8B D9 39 41 08 75 ?? "
      "E8 ?? ?? ?? ?? 84 C0 75 ?? 83 C8 FF 48 8B 5C 24 30 48 83 C4 20 5F C3 "
      "48 63 43 08 3B 43 0C 7D ?? 48 8B C8 8B 07 48 C1 E1 05 48 03 0B 89 01 48 8B 47 08",
      0x1A29980u },
    { "StridsHash",        /* idStr::Hash (FNV-1a 0x811c9dc5/0x1000193, lowercasing) -- the engine hash the
                            * record's key field uses; SnapHak inlines the same FNV-1a, we call the engine's */
      "48 89 5C 24 08 57 48 83 EC 20 0F B6 01",
      0x1A29B90u },
    /* --- overrides FILE-SHADOW engine anchor (port of OG FUN_18000b370 vtable swap) --- */
    { "ResProviderCtor",   /* engine resource-provider ctor (0x1a51070): member[0]=vtable@engineBase+0x27984a0;
                            * carries `LEA RAX,[rip+vtable]` right after the prologue -> decode to recover the
                            * vtable build-portably. The open-by-name method = vtable slot +0xf8 (orig fn
                            * 0x141a57a60). The overrides op swaps THAT slot with our override-open. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 "
      "48 8D 05 ?? ?? ?? ?? 48 89 01 33 FF C7 41 18 00 00 33 00",
      0x1A51070u },
    /* --- console-command + cvar registration infra (clone of XINPUT1_3 FUN_1800229b1) --- */
    { "Printf",            /* idCommon message-dispatch (0x1a08e80); every handler's console output via
                            * the Printf wrapper (clone of OG FUN_180006380 -> (1, fmt, &va)) routes here */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 BA FF FF",
      0x1A08E80u },
    { "CvarRegister",      /* OUTER cvar register (0x1a04f00); self-defaults the two .data globals + forwards
                            * to idCVarSystem::Register 0x1a05e70. We sig THIS (not 0x1a05e70). The cvar install
                            * calls it per cvar with flags=typecode (1=BOOL 2=INT 4=FLOAT) */
      "48 8B C4 48 89 48 08 57 48 83 EC 60 48 C7 40 E8 FE FF FF FF 48 89 58 10 48 89 68 18 "
      "48 89 70 20 41 8B F9",
      0x1A04F00u },
    { "CmdSystemLea",      /* KEYSTONE: thin engine accessor (0x717a50, the bot_add/bot_remove registrar) whose
                            * prologue loads the idCmdSystemLocal* global via MOV RCX,[rip+cmdSystem] (48 8B 0D,
                            * the FIRST decode-target opcode in the fn, byte offset 6). Decode = rip_next+disp32
                            * -> cmdSystem-global SLOT (RVA 0x55b7280, == OG *(engineBase+0x55b7280)); then
                            * DEREFERENCE ONCE: g_cmdsys = *(void**)slot. Generic prologue forces a 65-byte sig.
                            * Form is MOV (48 8B 0D), NOT LEA -> sh_resolve_cmdsys must scan all four decode
                            * opcodes (48 8D 0D / 48 8B 0D / 48 8D 05 / 48 8B 05), unlike sh_strids which scans
                            * 48 8D 0D only. Fallback: g_cmdsys = *(g_doom_base + 0x55b7280) (known offset). */
      "40 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 DB 4C 8D 05 ?? ?? ?? ?? "
      "89 5C 24 28 48 8D 15 ?? ?? ?? ?? 48 89 5C 24 20 48 8B 01 FF 50 20 48 8B 0D ?? ?? ?? ?? "
      "4C 8D 0D ?? ?? ?? ??",
      0x717A50u },
    /* --- type-introspection (cs_fieldinfo / sh_type; clone of XINPUT1_3 FUN_180021db0 /
     * FUN_180021090). Both reach the reflection/type-info mgr via the hardcoded declMgr accessor RVA
     * 0x17f7030 (NOT sig-able -- a real lazy-init singleton accessor, fixed prologue shared by ~47 fns,
     * unique only via the build-volatile RIP disp; resolved off g_doom_base in typeinfo.c, vtable+0x80).
     * These two engine lookups ARE sig-able + unique (sig-resolve == known_rva). */
    { "FindTypeInfoByName", /* FindTypeInfoByName(reflect, typeName, scope=0) -> the type record. Ghidra's
                             * void-return is a decompiler miss (recursive %s::%s scope lookup); the live
                             * caller FUN_1409c79d0 proves a non-void record return (*(rec+0x20)). cs_fieldinfo
                             * + sh_type CLASS branch both call it. record+0x20 field array / stride 0x48 /
                             * varName@+0x10 CONFIRMED LIVE; offset@+0x18 size@+0x1c varType@+0x00 varOps@+0x08
                             * comment@+0x28 OG-only BUILD-SPECIFIC (CLASS render reads 3 strings:
                             * varType/varOps/varName). 12-byte large-frame prologue, zero wildcards. */
      "40 55 56 57 48 8D AC 24 50 BE FF FF",
      0x1A1D590u },
    { "FindEnumByName",     /* sh_type ENUM branch: FindEnumByName(reflect, name) -> the enum record
                             * (FUN_141a1da20 cleanly returns *(*hashIdx+0x10)+idx*0x18). sh_type falls
                             * through to it when FindTypeInfoByName returns NULL. enumRec+0x10 members /
                             * stride 0x10 / member name@+0 / value(uint)@+8 CONFIRMED LIVE; enum NAME@+0
                             * OG-only BUILD-SPECIFIC. 37 bytes, 4 wildcards = the single rel32 call disp. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B EA 48 8B CA "
      "E8 ?? ?? ?? ?? 23 43 24",
      0x1A1DA20u },
    { "MapGetter",          /* sh_dumpmap: MapGetter(gameMgr) -> the live SnapMap object
                             * (FUN_14031ad60; reads gameMgr+0x29a0b0, builds DynamicSnapMap if absent).
                             * BLACK BOX with our GameMgrLea gameMgr; no struct offset in the clone's reach.
                             * 30-byte prologue, 0 wildcards. */
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8B D9 48 8B B9 B0 A0 29 00",
      0x31AD60u },
    { "MapWriter",          /* sh_dumpmap: MapWriter(map, pathCStr) writes the SnapMap to a file
                             * (FUN_14182b740; no-ops if map+0x38 != 5; Printf's "writing %s..." itself).
                             * BLACK BOX: pass argv[1]. 71 bytes, 4 wildcards (security-cookie rip-disp); tail
                             * 83 79 38 05 = cmp [rcx+0x38],5 = the v5 gate. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 "
      "48 C7 44 24 30 FE FF FF FF 48 89 9C 24 00 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 85 A0 01 00 00 48 8B F2 48 8B F9 83 79 38 05",
      0x182B740u },
    { "SessionDevModeGetter", /* devmode: the 8-byte idSessionLocal bool getter at 0x18a31d0
                               * (movzx eax,[rcx+0x34c89]; ret), CC-padded after. snaphak_disable_devmode
                               * patches the head 0F B6 81 -> 31 C0 C3 (xor eax,eax; ret) so the getter
                               * returns 0; snaphak_reenable_devmode code_unpatches it back. The pattern IS
                               * the expect bytes -- the disp 0x34c89 (89 4C 03 00) is build-specific and
                               * EMBEDDED so a shifted build REFUSES rather than mis-patches. code_patch
                               * overwrites only the first 3 bytes (bytes 3-7 untouched), so unpatch restores
                               * the full getter + the sig re-resolves -> repeatable. File-wide UNIQUE
                               * (1 match @ 0x18a31d0). */
      "0F B6 81 89 4C 03 00 C3",
      0x18A31D0u },
    { "RenderLogStub",        /* cs_start_render_logging: the render-debug TRACE SINK at .text
                               * 0xd99dc0 (FUN_140d99dc0), a no-op when logging is off: `mov [rsp+0x20],r9;
                               * ret` (4C 89 4C 24 20 C3) + 10 CC pad -> 16 bytes of safe overwrite room
                               * (next fn @0xd99dd0). cs_start_render_logging detours it with
                               * our_renderlog_hook so the engine's printf trace lines land in renderlog.txt;
                               * stolen=14 (hook.c writes a 14-byte FF25 abs-jmp + requires stolen>=14; 14<=16).
                               * The 6-byte stub alone matches twice (0xd99dc0, 0x17cb345) -- NOT unique; the
                               * 12-byte window (stub + 6 CC) is file-wide UNIQUE (1 match @0xd99dc0). The hook
                               * reads ZERO renderer internals (the engine passes a fully-formatted fmt+varargs)
                               * and does NOT trampoline (the original sink was a no-op). Verified vs
                               * an unpacked DOOM image. */
      "4C 89 4C 24 20 C3 CC CC CC CC CC CC",
      0xD99DC0u },
    /* --- snaphak_algo override targets (cs_dontuse; clone of the 4 OG XINPUT1_3 detours over the
     * engine math fns). cs_dontuse FULL-replaces each engine fn with our f64 reimpl (matmul/inverse/
     * curveEval) or bit-exact color-pack; OG hooks do NOT chain, so neither do ours. Each sig is
     * file-wide UNIQUE @ the named RVA, verified vs an unpacked DOOM image (resolver
     * hits==1, rva==known_rva). See algo.c. */
    { "AlgoMatMul",        /* engine 0x1a82f10 void f(const float*A rcx[16], const float*B rdx[16],
                            * float*out r8[16]) -- 4x4*4x4 row-major out=A*B (out[r*4+c]=sum_k A[r*4+k]*
                            * B[k*4+c]); DIRECT-confirmed from the 0x141a82f10 decompile. f32, no clamp.
                            * cs_dontuse FULL-replaces it with sh_algo_matmul (f64 accumulate, store f32).
                            * Sig steals 14 (clean prologue, no RIP-rel in window). */
      "48 8B C4 48 81 EC E8 00 00 00 0F 10 01 4C 8D 18",
      0x1A82F10u },
    { "AlgoInverse",       /* engine 0x1a828f0 bool f(const float*M rcx[16], float*out rdx[16]) -- 4x4
                            * inverse; AL=invertible. DIRECT (0x141a828f0 decompile + disasm): det via 4
                            * cofactors dotted with row 0, singular test ABS(det) < epsilon
                            * (DAT_14279b060=1.0000000168623835e-16f) -> if singular AL=0 + out UNTOUCHED;
                            * else AL=1 + write the 16 adjugate*(1/det). cs_dontuse FULL-replaces it with
                            * sh_algo_inverse (f64 adjugate/det, SAME contract). Sig steals 14. */
      "48 8B C4 48 81 EC 98 01 00 00 0F 10 19 0F 10 41 10",
      0x1A828F0u },
    { "AlgoPackRGBA",      /* engine 0x1a19470 uint32 f(const float*rgba rcx[4]) -> R|G<<8|B<<16|A<<24.
                            * ENGINE: per-ch i=(int)(f*255.0f) cvttss2si TRUNCATE, clamp [0,255]. The OG
                            * cs_dontuse hook (0x1cdc0, DIRECT capstone) is round-HALF-UP in double:
                            * i=(int)floor((double)f*255.0 + 0.5) (mulsd 255.0 DAT_180038740, addsd 0.5
                            * DAT_180038728, call floor, cvttsd2si), clamp [0,255]. cs_dontuse FULL-replaces
                            * with sh_algo_packrgba reproducing the HOOK (BIT-EXACT to OG). 3rd instr is
                            * RIP-rel (F3 0F 10 0D disp32) at off 10 -> masked; full-replace never executes
                            * the original/trampoline so a 14-byte clobber is safe (hook.c blindly copies the
                            * window to the unused trampoline). Sig steals 14. */
      "F3 0F 10 01 41 BA FF 00 00 00 F3 0F 10 0D ?? ?? ?? ??",
      0x1A19470u },
    { "AlgoCurveEval",     /* engine 0x1a5eb40 float f(const void*c rcx, float t xmm1, uint8_t mode r8b)
                            * -> xmm0. Keyframed curve: count@c+0x20c, times[]@c+0xc, values[]@c+0x10c,
                            * mode bytes c+0x00/01/02. DIRECT (0x141a5eb40 decompile): count<1->0; ==1->v0;
                            * c+0x02 set -> ALT MODE (cubic-spline tail FUN_141a5e6e0 -- FLAGGED limitation,
                            * sh_algo falls back to the main path); else bracket-find + (c+1 set: hold v_prev
                            * | else linear lerp (1-frac)*v_prev + frac*v_curr) with TWO flush-to-zero guards
                            * (abs-mask 0x7fffffff, thresh DAT_141fd5940=1e-18f). cs_dontuse FULL-replaces
                            * with sh_algo_curveeval (f64 main path). Sig steals 14. */
      "48 89 5C 24 10 56 48 83 EC 40 8B 81 0C 02 00 00",
      0x1A5EB40u },
    /* --- WS-C deferred dev/asset commands ([20] sh_genmd6model / [19] sh_genbmodel / [17] sh_debugrender).
     * These engine fns are the call-targets the OG XINPUT1_3 handlers reach via *(engineBase+RVA)
     * (FUN_18000b560 / FUN_18000b4a0 / FUN_18001ffe0). The clone resolves each by SIGNATURE off the live
     * DOOM module (b2 handlers' resolve_sig_by_name over g_module_base) -- NO hardcoded base+RVA. Author
     * recipe (file-image, capstone): dump <rva> <len> + scan the minimal-unique <pattern> against
     * an unpacked DOOM image (image base 0x140000000). Each pattern below is file-wide UNIQUE
     * (scan HITS==1 @ the named RVA); masked bytes are the RIP-rel disp32s + rel32 call operands (build-
     * volatile). re-derive on a build bump: re-run the minimal-unique scan, confirm 1 hit. --- */
    { "Md6Ctor",           /* [20] md6 model-compiler context ctor (engine 0x149b8d0). idMd6Builder::ctor:
                            * RCX=&md6, stores vtable@0x14254d3a8 to [md6], default-idStr-ctor's md6+0x60,
                            * inits the build options (md6+0x38=-1, md6+0x58=1, the size fields 0x150000/
                            * 0x50000, the float defaults). Returns RAX=&md6. The OG sh_genmd6model calls it
                            * FIRST on the md6 ctx. UNIQUE via the distinctive `mov dword[rbx+0x38],-1` tail
                            * (the generic ctor prologue alone matches 109x). 51 bytes; 8 wildcards = the
                            * rel32 ctor-call + the LEA vtable disp32. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 "
      "E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 03 48 C7 43 38 FF FF FF FF",
      0x149B8D0u },
    { "Md6SetOutput",      /* [20] idMd6Builder::SetOutput (engine 0x149c450). RCX=&md6, RDX=output idStr;
                            * 0x1d0-byte frame, security-cookie-guarded, copies the output decl name into the
                            * md6 ctx (the bmd6model target path). The OG sh_genmd6model calls it AFTER
                            * IdStrAssign(input)+IdStrCtor(output): SetOutput(&md6, output). UNIQUE 55-byte
                            * prologue; 4 wildcards = the security-cookie rip-disp32 (48 8B 05 ????). Note:
                            * Ghidra did NOT auto-define a function at 0x149c450 (no static xref -- the OG
                            * reaches it only via the computed *(base+RVA) call); the sig anchors on the raw
                            * file-image prologue bytes, confirmed by capstone. */
      "40 57 48 81 EC D0 01 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 F0 01 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 01 00 00 48 8B DA 48 8B F9 48 8D 4C 24 60",
      0x149C450u },
    { "Md6Build",          /* [20] the FINAL md6 call (engine 0x149bee0) the OG sh_genmd6model makes on the
                            * md6 ctx before the idStr dtors. RE-FINDING: this fn is DESTRUCTOR-SHAPED, not a
                            * pure "build" -- it reloads vtable@0x14254d3a8 to [md6], frees the model array
                            * (calls the array-clear 0x149c5d0 + the 0x10-arg resource free [vtbl+0x10]),
                            * allocates+writes the compiled buffer to md6+0xf8 (R9D=6 size-class), nulls every
                            * field, and TAIL-JMPs to the base teardown 0x1417feb30. So the OG's last md6 call
                            * is a compile-then-release (build folded into the dtor), NOT a separate Build().
                            * The clone calls it faithfully (last, single-arg &md6). UNIQUE 47-byte prologue;
                            * 4 wildcards = the LEA vtable disp32. */
      "40 57 48 83 EC 50 48 C7 44 24 40 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 "
      "48 89 74 24 70 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 33 ED 8B F5",
      0x149BEE0u },
    { "DefaultIdStrCtor",  /* [19]+[20] SHARED idStr default ctor (engine 0x19fd040). RCX=&str; stores
                            * vtable@0x14278b270, calls 0x19fc880, sets the inline-buffer string empty
                            * (str+0x18=0x80000014 capacity, str+0x10=&str+0x1c data ptr, str+0x8=0 len,
                            * byte[+0x1c]=0). Returns RAX=&str. BOTH sh_genmd6model (the options idStr) and
                            * sh_genbmodel (the options idStr) ctor a default idStr via THIS fn (OG calls
                            * *(base+0x19fd040) -- distinct from IdStrCtor 0x19fcef0 which copies a C-string).
                            * Author once, reuse from both handlers. UNIQUE via the capacity/data/len init
                            * run (the prologue alone is generic); 8 wildcards = the LEA vtable disp32 +
                            * the rel32 sub-ctor call. 49 bytes. */
      "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 E8 ?? ?? ?? ?? "
      "48 8D 43 1C C7 43 18 14 00 00 80 48 89 43 10 C7 43 08 00 00 00 00 C6 00 00",
      0x19FD040u },
    { "BModelBuilder",     /* [19] idBModelBuilder entry (engine 0x14cf550). 4-arg: RCX=out (a 208-byte
                            * result struct the OG stack-allocs), RDX=input file, R8=output file, R9=&opts (the
                            * 0xD0-byte options struct the OG memsets to all-0x01). Security-cookie-guarded;
                            * ctor's an internal builder @[rsp+0x40] (0x14c61b0), runs the build (0x14cf600),
                            * on success runs 0x14d0be0, dtor's (0x14c62b0). The OG sh_genbmodel calls it as
                            * BModelBuilder(out208, argv[1], argv[2], &opts). UNIQUE 51-byte prologue; 4
                            * wildcards = the security-cookie rip-disp32 (48 8B 05 ????).
                            * RECIPE-TAG (build-specific, NOT in this sig): the opts struct is 0xD0 bytes
                            * memset to 0x01 (OG cmd_0xb4a0 `memset(auStack_e8,1,0xd0)`, DIRECT). Re-confirm
                            * the 0xD0 size on a build bump by tracing how 0x14cf600 reads R9/opts; the clone
                            * memsets a 0xD0 byte buffer to 0x01 and passes &buf as R9, faithful to the OG. */
      "40 53 55 56 57 48 81 EC 28 01 00 00 48 C7 44 24 30 FE FF FF FF 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 10 01 00 00 49 8B F1 49 8B E8 48 8B FA 48 8B D9",
      0x14CF550u },
    { "RenderWorldGetter", /* [17] sh_debugrender renderWorld handle. The OG reads renderWorld =
                            * *(engineBase+0x57216f0) -- a .data SLOT (RVA 0x57216f0, holds the live
                            * idRenderWorld* at runtime), NOT a thin accessor fn (unlike cmdSystem/gameMgr,
                            * renderWorld has no dedicated getter). PORTABLE HANDLE: this sig anchors a UNIQUE
                            * 32-byte engine window (in a renderWorld-registrar fn @0x5e4c28) that carries
                            * `LEA RCX,[rip+slot]` at byte offset +6; decode rip_next(+0x5e4c35)+disp32 ->
                            * the slot RVA 0x57216f0 (CONFIRMED: the live disp decodes exactly to 0x57216f0),
                            * then DEREFERENCE ONCE for the live renderWorld (== OG *(base+0x57216f0)). Reuse
                            * sh_decode_rip_slot (the shared 4-opcode RIP scanner: it catches 48 8D 0D at the
                            * window's first decode-target). FALLBACK (CmdSystemLea/GameMgrLea philosophy):
                            * renderWorld = *(g_doom_base + 0x57216f0). 6 LEA xrefs to the slot exist; this
                            * site (89 B3 A8 02 00 00 | 48 8D 0D ???? | E8 ???? | 48 8B 0D ???? | 48 85 C9 74
                            * 12 66 C7) is the unique anchor. 32 bytes; 12 wildcards = 2 disp32s + 1 rel32.
                            * RECIPE-TAG the SLOT RVA: scan "89 B3 A8 02 00 00 48 8D 0D" -> the LEA, decode
                            * its disp -> slot RVA (re-derive on a build bump).
                            * RECIPE-TAG the renderWorld VTABLE indices (build-specific, NOT sig-able -- a
                            * runtime vtable): OG uses vtbl+0x188 = GetActiveRenderModelCount() -> uint, and
                            * vtbl+0x190 = GetRenderModel(idx) -> model (model+0x10 = name char*); vtbl+0x88 =
                            * the test_sum world getter. Re-derive per build from the OG cmd_0x1ffe0 dispatch
                            * (dumprenderinfo/test_rm_commit branches) or by tracing the live renderWorld vtbl.
                            * RECIPE-TAG the editor offsets (build-specific): showcursor writes
                            * byte[editor+0x23624]=0; the cursor-copy reads editor+0x170 (qword) + editor+0x178
                            * (dword) -- from the OG cmd_0x1ffe0 (DAT_18003e5c0 = the editor global the clone
                            * already resolves). */
      "89 B3 A8 02 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 85 C9 74 12 66 C7",
      0x5E4C28u },
    { NULL, NULL, 0 }   /* terminator */
};
