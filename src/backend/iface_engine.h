/* iface_engine.h -- the BACKEND-hosted engine-touch bodies for the UI-interface vtable.
 *
 * The shared interface object (snaphak_iface) is backend-OWNED: the backend resolves the editor singleton
 * + the selection/toast engine fns by SIGNATURE and fills the vtable's engine-touch slots (the LIGHT
 * touches the SnapStack STORE-ops need -- selection read/write, hovered id, toast, class/inherit read, id
 * validity/count/resolve). The frontend snaphakui.dll calls these through the vtable at the pinned offsets.
 *
 * R1 / portability: the editor singleton is a HARDCODED data RVA (0x3056748, like cmdSystem/cvarSystem --
 * a non-unique data global, NOT sig-able); EVERY engine FUNCTION (AddToSelection/ClearSelection/Toast/
 * IdStr ctor/dtor) is resolved by SIGNATURE off the same sig DB the rest of the backend uses -- never a
 * hardcoded base+RVA. The editor-struct field offsets (selection obj, ids array, hovered id, defsub,
 * classname/inherit source-text blobs) are this-live-build offsets ported from the reference implementation (the proven
 * mechanism) -- SEH-guarded so a shifted-build offset degrades to a clean no-op, never a crash.
 *
 * Clean-room: ported from our own RE + the reference implementation (the live-proven editor bridge). Zero
 * OG SnapHak bytes.
 */
#ifndef B2_IFACE_ENGINE_H
#define B2_IFACE_ENGINE_H

#include <stdint.h>
#include "signatures.h"

/* Resolve the engine deps (editor singleton via known data RVA off module_base; the selection/toast/idStr
 * engine fns by name from the already-resolved sig results) and BIND the interface vtable's engine-touch
 * slots (sh_iface_bind_engine_slots). Call once at install, AFTER the interface object exists + the sig DB
 * has resolved. Degrades gracefully: an unresolved fn leaves its slot a clean no-op (the op reports the
 * gap, never crashes). Idempotent (one-shot latch). Returns the number of slots bound. */
int sh_iface_engine_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* LAYER C (crash prevention): would entity `id`'s class+inherit be ACCEPTED by the engine decl validator
 * after this change? `newClass`/`newInherit` = the values being set (NULL = that field unchanged; the live
 * defsub value is read). The validator's rule: the className must derive from the inherit decl's base type.
 * Returns 1 = OK/uncertain (apply), 0 = definite "does not derive" (REJECT, logged) -- the combo that fatally
 * faults the decl reparse with an inner-caught Error(6) the fault-shield cannot recover. Called BEFORE the
 * set/deserialize by slot_set_classname (+0x78), slot_set_inherit (+0x80), and ae_apply_one (bss-apply).
 * Fail-open on uncertainty. Allows ANY valid class+inherit morph; rejects only crash-inducing combos. */
int sh_iface_class_inherit_ok(int id, const char *newClass, const char *newInherit);

/* Resolve an entity id -> its full module-qualified id-string (the targets-field ref form). A global entity
 * yields the "... (no module)" form (no resolvable ref -- the caller rejects it). SEH-guarded. */
const char *ie_resolve_id_string(int id, char *buf, int cap);

#endif /* B2_IFACE_ENGINE_H */
