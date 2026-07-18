/* overrides.h -- the OVERRIDES FILE-SHADOW resource loader, native C
 * (port of OG's resource-open vtable swap FUN_18000b370 / FUN_18000b110 / FUN_18000ce50).
 *
 * SnapHak "overrides" are a transparent file-shadow soft-mod: any resource the engine opens by name
 * is FIRST looked up as a same-named file under %USERPROFILE%\snaphak\overrides\ (or
 * overrides\shader_includes\ for shader includes); if present, the engine is served that file's bytes
 * from disk instead of the packaged resource. This is how SnapHak ships its 29 snapeditorentitydef /
 * editor-settings / property-inspector overrides that expand the editor palette.
 *
 * MECHANISM (DIRECT, RE of OG XINPUT1_3.dll 2021-03-27 + live DOOM):
 *   The engine's resource-provider class has a C++ vtable at `engineBase + 0x27984a0` (DOOM
 *   PTR_FUN_1427984a0, set as member[0] by the ctor FUN_141a51070). Its open-by-name VIRTUAL METHOD is
 *   vtable slot +0xf8 (= engineBase + 0x2798598), originally engine fn 0x141a57a60. SnapHak overwrites
 *   THAT ONE 8-byte slot with its own open hook FUN_18000b370, saving the original (via a verbatim
 *   vtable copy whose +0xf8 it keeps as DAT_18003e708). On every engine open, the hook signature is
 *       int open(void* this, const char* name, uint8 b1, uint8 b2, uint mode)   // __fastcall, 5 args
 *   It calls FUN_18000b110 to test for overrides/<name> under the profile dir; if the file exists it
 *   returns a SnapHak idFile-subclass stream (vtable PTR_FUN_18003d050: Read=fread, Length, Name,
 *   Close=fclose+free, ...) that the engine reads through; else it CHAINS to the saved original
 *   (*DAT_18003e708)(this,name,b1,b2,mode). A mode>=2 recursion guard goes straight to the original.
 *   The path builder FUN_18000ce50 = SHGetFolderPathA(CSIDL_PROFILE) + "/snaphak/" + relative.
 *
 * NATIVE PORT (the difference from OG):
 *   - This is a VTABLE-SLOT swap, NOT an inline code detour: the target is an 8-byte .data function
 *     pointer (vtable+0xf8), so we do NOT use install_inline_hook (which writes a 14-byte code jmp). We
 *     read+save the original slot pointer, then write our open hook's address into the slot (VirtualProtect
 *     RW, store, restore, FlushInstructionCache) and record it so we can restore the slot on unload.
 *   - The vtable is .data (not masked-byte sig-scannable). We locate it BUILD-PORTABLY: sig-resolve the
 *     ctor (DB name "ResProviderCtor"), decode the `LEA RAX,[rip+vtable]` right after its prologue to
 *     recover the vtable VA, then open-slot = vtable + 0xf8 -- the same LEA-decode the strids op uses.
 *   - Our returned stream is our OWN clean-room idFile subclass (its dtor frees with our allocator, so we
 *     need no engine allocator/free): the engine only ever touches it through the vtable methods (all
 *     ours) + the public Length/Name fields. Semantically equivalent to OG's stream.
 *   - THREE-LAYER resolution (OG has two): user disk file -> our BUILT-IN default decls served FROM
 *     MEMORY (overrides_baked.h; the "*Custom" tab set) -> the engine's packaged resource. Built-ins
 *     are never written to the user's folder (they update with each release; deleting a user file =
 *     reset to default). The user layer is toggleable via the snaphak_user_overrides cvar; install
 *     runs a reclaim (deletes OUR untouched previously-written default copies) + an audit log pass.
 *
 * Clean-room: ported from our own RE (above). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B1_OVERRIDES_H
#define BACKEND_B1_OVERRIDES_H

#include <stdint.h>
#include <stddef.h>

/* Install the overrides file-shadow by swapping the engine resource-provider's open vtable slot.
 *   ctor_fn        = resolved engine ResProviderCtor address (from the signature resolver, DB name
 *                    "ResProviderCtor"). 0 => not resolved; logs SKIPPED and returns 0.
 *   ctor_status_ok = 1 iff a CLEAN scan hit (SIG_OK), not the hook-tolerant known_rva fallback
 *                    (SIG_OK_HOOKED). The ctor is only used to DECODE the vtable LEA (we don't patch the
 *                    ctor's code), so a hooked prologue would corrupt the LEA decode -- refuse on a
 *                    hook-tolerant resolve, same conservative policy as the other installs.
 * Returns 1 if the slot was swapped, 0 otherwise (logs the reason). Emits a "B1: overrides file-shadow
 * installed ..." marker on success. */
int sh_overrides_install(void *ctor_fn, int ctor_status_ok);

/* Set the overrides ROOT directory (the dir that holds overrides\ and overrides\shader_includes\). The
 * effective lookup is <root>\overrides\<name>. Default = %USERPROFILE%\snaphak (OG's path -> overrides
 * under it). Pass NULL to reset to the default. Returns 1 if a path is set. */
int sh_overrides_set_root(const char *path);

/* How many times the shadow has FIRED (served an override file instead of the packaged resource).
 * Observability for the test harness. */
unsigned long sh_overrides_shadow_count(void);

/* Restore the engine open vtable slot to the saved original (LIFO-safe; idempotent). Returns 1 if a
 * slot was restored, 0 if none was installed. Call on unload to leave the engine vtable clean. */
int sh_overrides_uninstall(void);

#endif /* BACKEND_B1_OVERRIDES_H */
