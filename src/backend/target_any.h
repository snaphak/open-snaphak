/* target_any.h -- the console command sh_target_any: the editor-decl visibility toggle.
 *
 * This reproduces OG SnapHak's own sh_target_any (FUN_180021EE0) EXACTLY: a global toggle that walks every
 * idDeclSnapEditorEntity decl and flips bits 7-6 (0xC0) of the editor-flags byte at decl+0x3CD -- SET on
 * reveal, CLEAR on re-hide (with idInfoPath kept visible). This is what reveals the hundreds of
 * campaign-only / normally-hidden placeable entity decls in the SnapMap editor palette (and, as a
 * side-effect of the +0x3CD visibility pair, colours their logic wires green).
 *
 * PORTABILITY: the decl registry is reached through the engine's GetDeclsOfType (resolved by signature and
 * handed in at install); the decl-manager node offsets (+0x20 array / +0x28 count), the flags byte
 * (+0x3CD), and the idInfoPath className hop (entityDef@+0x1C8 -> className@+0x60) are build-specific --
 * re-derive per DOOM build (see target_any.c). Every engine memory touch is SEH-guarded.
 *
 * Clean-room: built from our own reverse-engineering of the OG behavior. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_TARGET_ANY_H
#define BACKEND_TARGET_ANY_H

struct idCmdArgs;

/* Cache GetDeclsOfType (resolved in dllmain) so the toggle can enumerate the decl registry. Call once. */
void sh_target_any_install(void *get_decls_of_type);

/* sh_target_any: toggle the editor-decl visibility (1st call reveals, 2nd call re-hides). Registered by
 * the console-command table. */
void h_target_any(struct idCmdArgs *a);

/* 1 while sh_target_any is in the REVEAL state (visible), 0 while hidden. Read by the wire-any connect-tool
 * detours (wiring_cleandirect.c) so they take the clean-direct branch only while sh_target_any is on. */
int sh_target_any_is_shown(void);

#endif /* BACKEND_TARGET_ANY_H */
