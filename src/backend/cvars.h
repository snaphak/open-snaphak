/* cvars.h -- register the cvar table with the engine cvar system: SnapHak's 9 cvars (clean-room
 * reimplementation of OG XINPUT1_3's static-init cvar push_back + spine flush) + our own
 * snaphak_user_overrides row (the user-override-layer kill switch; no OG counterpart).
 *
 * OG SnapHak declares 9 cvars as static descriptors (from our cvar-descriptor RE) and, in the install
 * spine FUN_1800229b1, flushes them through the engine cvar register fn:
 *   ( *(engineBase+0x1a04f00) )( desc+0x28, name, default, typecode, desc, argComp )
 * where typecode = 1=BOOL, 2=INT, 4=FLOAT, passed VERBATIM as the engine `flags` arg (the engine
 * massages the bits internally). None of the 9 carry EXPOSE/NOCHEAT -> they register non-EXPOSE
 * (gate-1-invisible), which is the faithful OG behavior.
 *
 * Our reimpl carries the 9 rows as a static table and calls the engine register fn (resolved by the
 * signature scanner as "CvarRegister", NO hardcoded RVA). The embedded idCVar object the engine writes
 * through lives in persistent, never-freed, 16-byte-aligned backing storage -- the engine links each
 * cvar into its process-lifetime cvar list, exactly as OG's static descriptors persist for the process.
 *
 * Clean-room: ported from our own RE (the cvar descriptor dump + register-ABI extract). Zero OG bytes.
 */
#ifndef BACKEND_B2_CVARS_H
#define BACKEND_B2_CVARS_H

/* Register all table cvars with the engine via the resolved CvarRegister fn (0 => not resolved; logs
 * SKIPPED and returns 0), THEN link them into the engine's FULL findable cvar table so FindCvar (and the
 * S0-aliased gate-1 `~` console) recognizes them. ONE-SHOT-LATCHED: CvarRegister has NO dedup (it
 * unconditionally links into the pending list), and the findable-insert must run exactly once, so the
 * latch prevents OUR double-register AND any double-link.
 *
 * THE FINDABLE-INSERT (the fix): our 9 cvars register into the pending list only; the SOLE table-hasher
 * RegisterStaticVars (0x1a06a00) already ran at static init, so our LATE cvars are in NEITHER findable
 * table -> FindCvar misses -> "Unknown command". After the CvarRegister loop we replay that fn's FULL-
 * table insert for our 9: append each embedded idCVar object into the FULL idList (cvarSys+0x08) and link
 * it into the FULL idHashIndex (cvarSys+0x38) via the engine's own name-hash (sig "NameHash"). We do NOT
 * set CVAR_EXPOSE and do NOT re-call RegisterStaticVars (its static-dup guard ExitProcess(2)es). If the
 * FULL table has no spare room we BAIL per-cvar (logged skip, no realloc). Every engine call + every
 * table access is SEH-guarded (a wrong offset degrades to a logged skip, never a crash/corruption).
 *
 *   cvar_register = resolved engine CvarRegister (sig "CvarRegister"; 0 => SKIPPED, returns 0).
 *   module_base   = the DOOM module base. cvarSys is resolved build-portably from it (the CmdSystemLea
 *                   sig decode +0x10, with *(module_base+0x55b7290) as the logged fallback -- see
 *                   sh_resolve_cvarsys), and it anchors the NameHash sig resolve too.
 *                   NULL => the register loop still runs but the findable-insert is SKIPPED (logged).
 * Does NOT depend on the cmdSystem global. Emits "B2: cvars registered N/9 ..." +
 * "B2: cvar findable-insert N/9 (cvarSys=%p count=%d cap=%d ...)". Returns the count registered (0..9). */
int sh_cvars_install(void *cvar_register, const void *module_base);

/* CVARS index constants (mirror the cvars.c CVARS[] table order) -- for consumers that read a
 * cvar's live value via sh_cvar_value_int / sh_cvar_value_int_reg. Rows 0..8 are the 9 OG cvars;
 * row 9 (snaphak_user_overrides) is our own addition (the user-override-layer kill switch). */
#define B2_CVAR_SNAPHAK_PRETTY_ON                 6
#define B2_CVAR_SNAPHAK_SHOW_RMCOUNT              7
#define B2_CVAR_SNAPHAK_COPY_RESLIST_TO_CLIPBOARD 8
#define B2_CVAR_SNAPHAK_USER_OVERRIDES            9

/* Read the live INT/BOOL value of cvar `index` (one of the B2_CVAR_* constants) from OUR engine-
 * populated backing object. The engine stores valueInteger at idCVar+0x30 (DIRECT from the source-of-
 * record (the engine idlib schema) idCVar + the OG DAT_18003d2b8==idCVar+0x30 cross-check).
 * SEH-guarded; returns `def` on a bad index, before install, or any fault.
 * NOTE (the build-specific-offset trap): +0x30 is DIRECT-from-source but the live read should be
 * spot-checked at FIRE (set the cvar to 1, confirm this returns 1). */
int sh_cvar_value_int(int index, int def);

/* Registration-aware variant: returns `def` until the engine has populated the backing object
 * (name@+0x40 matches), then the live value. Use for a cvar consulted on the boot path BEFORE the
 * cvar flush runs -- the plain read would report the zero-initialized slot (0) there, which is wrong
 * for a default-1 cvar (the overrides loader's snaphak_user_overrides gate is the canonical case). */
int sh_cvar_value_int_reg(int index, int def);

/* Read-only access to the cvar TABLE rows (name/default/description) -- for sh_help's listing.
 * Returns the row count; sh_cvar_table_row returns 1 and fills the pointers for a valid index. */
int sh_cvar_table_count(void);
int sh_cvar_table_row(int index, const char **name, const char **def, const char **desc);

#endif /* BACKEND_B2_CVARS_H */
