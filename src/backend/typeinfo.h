/* typeinfo.h -- the type-introspection console commands (cs_fieldinfo, sh_type),
 * native C. Ports of OG XINPUT1_3 FUN_180021db0 (cs_fieldinfo) and FUN_180021090 (sh_type).
 *
 * These two dev tools dump the engine's RUNTIME reflection metadata: cs_fieldinfo <type> <field> prints a
 * single field's "Size N, offset M"; sh_type <name> dumps a whole class (its fields) or enum (its members)
 * as C-struct/enum text and copies it to the clipboard.
 *
 * Engine deps (resolved/cached by sh_typeinfo_install):
 *   - the decl-type/reflection manager, reached the SAME way the bss apply already does (the
 *     DECL_MGR_ACCESSOR_RVA / the reference implementation _declMgrAccessor): call the accessor at the HARDCODED RVA
 *     0x17F7030 off g_doom_base -> declMgr, then reflect = (*(*declMgr + 0x80))(declMgr) (vtable slot
 *     +0x80). The accessor is NOT signature-able -- it is a real lazy-init singleton accessor whose fixed
 *     prologue (`53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF ...`) is shared by ~47 .text functions and only
 *     becomes unique via the build-volatile RIP displacement -- so it is resolved by the known data RVA,
 *     the established precedent; everything else is sig-resolved.
 *   - FindTypeInfoByName (sig "FindTypeInfoByName" 0x1A1D590) -- reflect, name, scope=0 -> the type record.
 *   - FindEnumByName     (sig "FindEnumByName"     0x1A1DA20) -- reflect, name        -> the enum record.
 *
 * Build portability (the reference-entity-layout trap):
 *   - the declMgr-accessor RVA + the vtable+0x80 slot are build-specific reflection-mgr layout -- the
 *     accessor call + the vtable deref + call are SEH-guarded; a wrong slot degrades to "type manager
 *     unavailable", never a crash.
 *   - record/field/enum sub-offsets: field array @ rec+0x20, field stride 0x48, field varName @ +0x10 are
 *     CONFIRMED LIVE (FUN_1409c79d0); enum members @ rec+0x10, member stride 0x10, name @ +0, value @ +8
 *     are CONFIRMED LIVE (FUN_140440230). field offset @ +0x18 / size @ +0x1c / varType @ +0x00 /
 *     varOps @ +0x08 / comment @ +0x28 and enum NAME @ +0 are OG-handler-only witnesses -> BUILD-SPECIFIC,
 *     live-confirm at FIRE. (sh_type's CLASS field render reads THREE strings -- varType(+0x00, primary),
 *     varOps(+0x08, the strstr("*") qualifier), varName(+0x10) -- per OG FUN_180021090 L240-253.) Every
 *     record/field/enum deref is SEH-guarded; both walk loops carry an iteration CAP so a never-terminating
 *     (garbage) record cannot spin forever.
 *
 * Clean-room: ported from our own RE (the foundation: the cs_fieldinfo + sh_type decompiles +
 * the OG command decompiles @0x21db0 / @0x21090). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B2_TYPEINFO_H
#define BACKEND_B2_TYPEINFO_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Install the type-introspection handler dependencies. Idempotent (one-shot latch). Caches
 * g_doom_base (for the hardcoded declMgr-accessor RVA 0x17F7030), FindTypeInfoByName + FindEnumByName
 * (sig-resolved). Each handler then runs on console invocation using the cached deps; a missing dep makes
 * that handler print a graceful "unavailable/unresolved" line rather than crash. Emits a
 * "B2: typeinfo install ..." marker. Call AFTER sh_entity_install (the cs_fieldinfo/sh_type handlers are
 * already registered by the sh_commands CMD_TABLE; this only wires their engine deps -- same contract as
 * sh_entity_install).
 *   results / n   = the resolved signature DB (from sig_resolve_all in dllmain).
 *   module_base   = the DOOM module base (for the declMgr-accessor at base + 0x17F7030).
 * Returns 1 on a successful install pass (even with some deps NULL -- the handlers degrade), 0 if it was
 * already installed (latch) or the module base is NULL.
 */
int sh_typeinfo_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* Return the raw declMgr singleton object (the lazy-init accessor at base + 0x17F7030, SEH-guarded;
 * NULL on a NULL base or any fault). Shared so the sh_superscriptop [12] handler (commands.c) reaches
 * the engine event-manager (declMgr vtable slot +0x90 -> evMgr) through sh_typeinfo's ONE declMgr accessor
 * rather than re-resolving the hardcoded RVA. Requires sh_typeinfo_install to have cached g_doom_base. */
void *sh_typeinfo_get_declmgr(void);

/* Enumerate the decl-instance NAMES of a decl-type (the Timeline-Editor decl-combo feed) into out_buf as
 * consecutive NUL-terminated strings (double-NUL end -- the +0x110 packed-string ABI). Uses the NON-LOGGING
 * declManager lookup (reflect = declMgr->[+0x80]; FindByName), NOT engine GetDeclsOfType (the ASSET registry,
 * which LOGS "Unknown resource class" on a decl-type miss). `declType` = the decl-type short-name (e.g.
 * "sound", "projectile"). Returns 1 + *out_count on >=1 name, else 0 (a miss is SILENT -- no log/crash). */
int sh_typeinfo_enum_decls_of_type(const char *declType, char *out_buf, int cap, int *out_count);

/* LAYER C (crash prevention): does `className` derive from (or == ) `baseName`, walking the engine type
 * hierarchy by name (FindTypeInfoByName -> superclass name @ rec+0x08, the decl-validator's own walk).
 * Returns 1 = derives, 0 = does NOT derive (incl. an unresolvable className), -1 = type system unavailable
 * (the caller must NOT reject on -1, only on a definite 0). Used by the bscls/bsin class-compatibility guard to reject
 * an incompatible class change BEFORE the engine's fatal "Class X does not derive from Y" decl-reparse
 * Error(6) (which is inner-caught -> unrecoverable by the fault-shield). */
int sh_typeinfo_class_derives(const char *className, const char *baseName);

/* LAYER C: resolve the inherit decl's base class name Y (= the class an entity's className must derive from
 * for the engine decl validator to accept the pair). Pure engine decl find (no side effects) + the inherit
 * decl's className @ +0x60; SEH-guarded. Copies Y into buf (cap bytes); returns buf, or NULL if the inherit
 * isn't resolvable (caller fail-opens). The engine-EXACT anchor for the class/inherit compatibility guard --
 * lets users morph an entity to ANY class+inherit pair the engine would accept. */
const char *sh_typeinfo_inherit_base(const char *inheritName, char *buf, size_t cap);

/* Enumerate the LIVE idlib reflection type registry -- every registered class NAME, not a frozen list.
 * RE'd from the engine (FindTypeInfoByName 0x1A1D590 + the registry builder/registrar all iterate one array).
 * Chain (reuses the already-wired reflect): reflect = declMgr->[+0x80]; container P = *(reflect+0); type-record
 * array B = *(P+0x20); records stride 0x38, className char* @ rec+0x00 (NULL name terminates the array),
 * superclass name @ rec+0x08. Collects up to `cap` className pointers (which point into engine-owned static
 * string data -- stable for the process life) into out_names[]. Returns the count, or -1 if the registry is
 * unreachable (reflect/container NULL -- e.g. pre-boot), so callers can fall back to a static list. The raw
 * array walk is SEH-guarded (a shifted / garbage array degrades to whatever was collected, never a crash);
 * filter/print in the caller. This is the portable, complete replacement for the frozen sh_entlist /
 * sh_validclasses candidate lists -- it auto-tracks DOOM patches and surfaces valid classes that have no
 * editor decl. */
int sh_typeinfo_collect_classnames(const char **out_names, int cap);

#endif /* BACKEND_B2_TYPEINFO_H */
