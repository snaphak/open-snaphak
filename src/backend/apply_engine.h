/* apply_engine.h -- the BACKEND-hosted 8-pass full-entity APPLY CHAIN.
 *
 * The heavy half of the SnapStack APPLY-ops (bss/bsi/bsf/bsb/bse/accl/acctargets/mkcmd): the
 * serialize-entity-to-JSON / deserialize-patched-text-onto-temp-def / commit-class-inherit-source /
 * mkcmd-prefab-paste engine sequences, plus the clone_bss_apply command-buffer routing (the reference implementation FIX B).
 *
 * This is the native port of the reference implementation's proven mechanism (serializeEntityToJson +
 * deserializeTextToObject + doApplyBssOne's commit tail + doMkcmdApplyNow + ensureBssCommand). Every
 * engine function is resolved by SIGNATURE off the shared sig DB (version-portable); the declMgr accessor
 * is the hardcoded RVA reused from sh_typeinfo (the one non-sig-able engine accessor). Every struct is
 * allocated at its REAL size + constructed with the engine ctor (an allocation-size freeze lesson); the teardown
 * order is faithful to FUN_180004950 (FIX A) and deliberately does NOT call the OG raw lexer-buffer free.
 *
 * The frontend (Qt) does the JSON patch between serialize + apply; this backend owns the engine touch.
 * The three vtable slots (serialize_entity +0xc8 / apply_edit +0xd0 / read_prefab +0xb8) are exported via
 * sh_apply_engine_slots() so sh_iface_engine can fold them into the single sh_iface_bind_engine_slots call.
 *
 * Clean-room: ported from our own RE + the reference implementation (the live-proven apply chain). Zero
 * OG SnapHak bytes.
 */
#ifndef B2_APPLY_ENGINE_H
#define B2_APPLY_ENGINE_H

#include <stdint.h>
#include "signatures.h"
#include "snaphak_iface.h"

/* Resolve the full apply-chain engine fns (entity-clone / struct-serialize / tree-render-json /
 * struct-deserialize / lexer ctor+parse / parse-node ctor+dtor / entity-def ctor+dtor / decl-source-rebuild
 * / idstr-assign -- all by SIGNATURE) + cache the cmdSystem + BufferCommandText/AddCommand for the
 * command-buffer routing. Call once at install, AFTER the sig DB resolves + sh_typeinfo_install (the shared
 * declMgr accessor). `cmdsys` is the idCmdSystemLocal* already decoded by sh_resolve_cmdsys (may be NULL ->
 * the command-buffer apply degrades to a clean toast, no crash). Returns 1 on a complete bind, 0 otherwise
 * (the slots still bind -- each body null-checks its own deps and degrades). Idempotent (one-shot latch). */
int sh_apply_engine_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys);

/* Fix B (sh_target_any timeline trigger): enqueue a targets-write -- append the TARGET entity's module-qualified
 * id to the SOURCE entity's state.edit.targets (the engine's native `activate` trigger), committed on the DOOM
 * main thread. Called by the sh_target_any wire hook for a bare timeline target INSTEAD of laying an invalid
 * (dangling) CSR connection edge. Both ids are live editor entity ids. */
void ae_schedule_target_write(int source_id, int target_id);

/* Fill `out` with the three heavy vtable-slot bodies (serialize_entity +0xc8 / apply_edit +0xd0 /
 * read_prefab +0xb8). sh_iface_engine merges these into its sh_iface_engine_slots before the single
 * sh_iface_bind_engine_slots call -- so all the engine-touch slots bind together. */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab);

/* expose the +0xb0 serialize-SELECTION->idSnapEntityPrefab slot body (the Prefabs create body)
 * so sh_iface_engine folds it into the single sh_iface_bind_engine_slots call. */
void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection);

#endif /* B2_APPLY_ENGINE_H */
