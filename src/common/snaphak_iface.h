/* snaphak_iface.h -- THE SHARED UI-INTERFACE ABI (the matched-pair bridge between the two clone DLLs).
 *
 * This header is the DURABLE shared-ABI artifact. It pins the EXACT binary layout of
 * the "UI-interface" object that the BACKEND (XINPUT1_3.dll) creates and the FRONTEND (snaphakui.dll)
 * consumes. Both clone DLLs include this header so the object is a MATCHED PAIR -- the backend writes the
 * vtable + fields, the frontend reads them at the same offsets.
 *
 *   OG provenance (DIRECT, RE-confirmed against the OG binaries this session):
 *     - object: XINPUT1_3 FUN_1800229b1 builds `operator_new(0x60)`, sets `*obj = &PTR_FUN_180035d30`
 *       (the 77-slot vtable), `_Mtx_init_in_situ(obj+8)` (the mutex), `obj[0xb] (=+0x58) = a 0x78-byte
 *       sub-object`, then `DAT_18003e608 = obj` and hands it to snaphakui as CreateThread arg[4].
 *     - sub-object (+0x58): the subcommand `std::map<string,handler>` RB-tree (nil-node `operator_new(0x48)`
 *       with the 0x101 color/leaf magic at +0x18) at sub+0x00, plus the work-queue `std::vector`
 *       (sub +0x60 begin / +0x68 end / +0x70 cap) + the entity stacks/groups.
 *     - vtable: 77 slots (+0x00..+0x260), DIRECT cell-dump from the OG binary.
 *       The live slots are +0x188 REGISTER / +0x190 UNREGISTER / +0x1a0 DRAIN (the work-queue).
 *
 * Build-portability: this is the clone's OWN ABI (clone XINPUT1_3 <-> clone snaphakui), self-
 * consistent and NOT DOOM-build-dependent. The ONLY hardcoded offsets that cross the DLL line are the
 * vtable-slot offsets pinned here. The build-specific ENGINE offsets (editor+0x209a8, entity layout) live
 * BEHIND this vtable in the backend -- re-derived there, never here.
 *
 * SCOPE: the object + the vtable layout + the REGISTER/UNREGISTER/DRAIN trio (the only slots the
 * bring-up exercises) are fully specified; the apply/serialize/select/toast/decl slots (+0xd0/+0xc8/...)
 * are PINNED at their offsets but STUBBED (a pin-and-stub placeholder) so the layout is the matched pair
 * now and the bodies are filled in later WITHOUT moving any offset.
 *
 * Clean-room: ported from our own RE (the OG vtable cell-dump + the
 * FUN_1800229b1 / FUN_180015c04 decompiles). Zero OG SnapHak bytes.
 */
#ifndef SNAPHAK_IFACE_H
#define SNAPHAK_IFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ command handler signature -------
 * A registered SnapStack subcommand handler. OG enqueues {handler, parsed-argv-vector} onto the work
 * queue (XINPUT 0x7620 -> obj+0x58 sub-object), and the think-loop's +0x1a0 DRAIN runs them on the UI
 * (main) thread. The args are an argv-style string vector; argc/argv are passed through verbatim.
 * `ctx` is the user pointer registered alongside the handler (later routed to the SnapStack op
 * dispatch table). */
typedef void (*sh_cmd_handler)(void *ctx, int argc, const char **argv);

/* The engine-touch interface-slot signatures (the LIGHT engine touches the SnapStack STORE-ops
 * need -- selection read/write + toast + class/inherit read + id validity/count/resolve). These are
 * BACKEND-OWNED bodies (the backend resolves the editor singleton + the AddToSelection/ClearSelection/
 * Toast engine fns by SIGNATURE, SEH-guards every deref); the FRONTEND calls them through the vtable at
 * the pinned offsets. Each is __cdecl(self, ...) so the offset is the ABI; `self` lets a body reach the
 * backend's cached engine state (it ignores `self` in practice -- the engine state is module-static). The
 * heavy serialize/apply slots (+0xc8/+0xd0) stay `void*` placeholders (bound later). */
struct sh_iface;
typedef int          (*sh_get_selection_fn)(struct sh_iface *self, int *out_ids, int max);   /* +0x150 */
typedef void         (*sh_clear_selection_fn)(struct sh_iface *self);                          /* +0x148 */
typedef void         (*sh_add_to_selection_fn)(struct sh_iface *self, int id);                 /* +0x138 */
typedef int          (*sh_hovered_id_fn)(struct sh_iface *self);                               /* +0x198 */
typedef int          (*sh_is_entity_mode_fn)(struct sh_iface *self);                            /* +0x1c0 */
/* +0x28 IS-VALID id. NOTE: the CLONE's +0x28 INVERTS the OG boolean sense -- the OG slot returns
 * TRUE-when-INVALID (FUN_180010274 proceeds only when +0x28=='\0'; FUN_1800147e8 skips when +0x28!=0),
 * whereas the clone's slot_is_valid_id returns TRUE-when-VALID (entity[id]+8 != 0). This is the clone's
 * OWN matched-pair ABI used self-consistently (the backend says true=valid, the frontend gates skip-if-
 * NOT-valid) -- net editor-visible behavior (skip invalid, proceed on valid) is identical to OG. Live-
 * ratified later. Keep backend + frontend in lock-step on the clone's true=valid convention. */
typedef int          (*sh_is_valid_id_fn)(struct sh_iface *self, int id);                      /* +0x28  */
typedef int          (*sh_entity_count_fn)(struct sh_iface *self);                             /* +0x10  */
typedef const char  *(*sh_id_to_string_fn)(struct sh_iface *self, int id, char *buf, int cap); /* +0x18  */
typedef const char  *(*sh_classname_fn)(struct sh_iface *self, int id, char *buf, int cap);    /* +0x48  */
typedef const char  *(*sh_inherit_fn)(struct sh_iface *self, int id, char *buf, int cap);      /* +0x50  */
typedef void         (*sh_toast_fn)(struct sh_iface *self, const char *title, const char *text);/* +0x1b8 */

/* ------------------------------------------------------------------ DATA-TAB slots --------
 * The engine-touch slots the 4 DATA tabs (Entities / Entity-State / Prefabs / Timelines) need, beyond
 * the store/apply set. ALL backend-OWNED (the backend resolves the engine fns by SIGNATURE -- or a
 * re-derive-tagged fallback RVA -- and SEH-guards every body); the FRONTEND calls them through the vtable
 * at the pinned offsets. They mirror the OG XINPUT1_3 slot bodies (FUN_180006a20/6ab0/6850/72a0/7230/
 * 6ba0/6bc0/73c0) faithfully. */

/* +0x58 GET displayname: reads entity[id]+0x178 (len) / +0x180 (data) into buf; returns buf (Entity-State
 * read-sync). OG FUN_180007230. */
typedef const char  *(*sh_get_displayname_fn)(struct sh_iface *self, int id, char *buf, int cap);  /* +0x58 */

/* +0x30 GET decl-source: copies the live entity's canonical decl-source text (*(ent+0x158)+0x140 ptr /
 * +0x138 len) into buf; returns buf (Entity-State read-sync, the QPlainTextEdit). OG FUN_1800065b0. */
typedef const char  *(*sh_get_declsource_fn)(struct sh_iface *self, int id, char *buf, int cap);   /* +0x30 */

/* +0x78 SET classname (id, cstr): IdStrAssign(defsub+0x60, cstr). OG FUN_180006a20 -> FUN_180004140. */
typedef void         (*sh_set_classname_fn)(struct sh_iface *self, int id, const char *cstr);      /* +0x78 */
/* +0x80 SET inherit (id, cstr): IdStrAssign(defsub+0x58, cstr). OG FUN_180006ab0 -> FUN_180004070. */
typedef void         (*sh_set_inherit_fn)(struct sh_iface *self, int id, const char *cstr);        /* +0x80 */
/* +0x128 SET displayname (id, cstr): IdStrAssign(entity[id]+0x170, cstr). OG FUN_1800072a0. */
typedef void         (*sh_set_displayname_fn)(struct sh_iface *self, int id, const char *cstr);    /* +0x128 */
/* +0x40 REBUILD+SET decl-source (id, cstr): DeclSourceRebuild(defsub, cstr, 1) -- the Save-to-Decl route
 * (DOOM 0x17ae560). OG FUN_180006850 -> FUN_180003fa0. */
typedef void         (*sh_rebuild_declsource_fn)(struct sh_iface *self, int id, const char *cstr); /* +0x40 */

/* +0xb0 serialize SELECTION -> idSnapEntityPrefab JSON text (Prefabs create). Writes up to cap-1 bytes;
 * returns the byte length (0 on failure). OG FUN_180006ba0 -> FUN_180004210. */
typedef int          (*sh_serialize_selection_fn)(struct sh_iface *self, char *out_json, int cap);  /* +0xb0 */

/* +0xc0 RESOLVE prefab path: %USERPROFILE%\snaphak\prefabs\<name>.json into out_path. Returns 1 on
 * success (and out_ok != 0). OG FUN_180006bc0 -> FUN_18000ce50 (SHGetFolderPathA + "/snaphak/" + prefix +
 * name). `prefix` = "prefabs/" (the OG passes the prefabs\ literal as the path prefix). */
typedef int          (*sh_resolve_prefab_path_fn)(struct sh_iface *self, const char *prefix,
                                                  const char *name, char *out_path, int cap);       /* +0xc0 */

/* +0x130 REMOVE id from selection (Entities ctx-menu Delete). Gated on editor+0x204d0 != 0 && id != -1.
 * OG FUN_1800073c0 -> engine 0x59fda0. */
typedef void         (*sh_remove_from_selection_fn)(struct sh_iface *self, int id);                 /* +0x130 */

/* +0x110 ENUMERATE the engine decls of a resource class (the Timeline-Editor constrained decl-comboboxes).
 * OG FUN_18000994c calls `(**(iface+0x110))(iface, resClassName, out_names_vec, out_current)` where the
 * idDecl* arg-type-name (e.g. "idDeclSoundShader*") was reduced to its lowercased resource-class string
 * (strip the leading "idDecl", lowercase the rest, drop the trailing '*' -> e.g. "soundshader"). The body
 * (backend-owned) calls the engine GetDeclsOfType(resClassName), walks the typed decl-manager node, and
 * fills `out_names` with up to `max` valid decl-name C-strings; returns the count written (0 = unknown
 * type / no decls / editor down). `out_names[i]` points into a caller-supplied char buffer block the
 * frontend owns -- here the slot writes each name into out_names[i] (a caller array of char* into one
 * scratch arena) is impractical across the POD ABI, so the slot writes the names PACKED into out_buf as
 * consecutive NUL-terminated strings and returns the count; the frontend splits them. SEH-guarded; a
 * shifted-build offset degrades to a clean 0 (the combobox then falls back to a plain string box, faithful
 * to the OG cVar8=='\0' branch). */
typedef int          (*sh_enum_decls_of_resclass_fn)(struct sh_iface *self, const char *res_class,
                                                    char *out_buf, int cap, int *out_count);       /* +0x110 */

/* +0x268 (clone-extension slot 0) ATOMIC class+inherit set. Validates the FINAL (cls,inh) pair ONCE with
 * sh_iface_class_inherit_ok, then -- if accepted -- writes BOTH defsub+0x60 (class) and defsub+0x58 (inherit)
 * via the engine IdStrAssign DIRECTLY, bypassing the per-slot +0x78/+0x80 guards (whose intermediate single-
 * field check would reject a cross-family morph at the half-applied state). Both-non-NULL also SIDESTEPS the
 * build-specific defsub+0x60/+0x58 live-read the single-field guards depend on. Returns 1 = applied, 0 =
 * rejected (the final pair would fatally fault the decl reparse) / no map / unbound. NULL/empty cls or inh =>
 * leave that field (degrades to the single-field guard semantics). The CALLER still issues the ONE +0x40
 * decl-rebuild after this returns 1 -- and MUST skip the rebuild when this returns 0 (a rejected fatal pair). */
typedef int          (*sh_apply_class_inherit_fn)(struct sh_iface *self, int id,
                                                  const char *cls, const char *inh);              /* +0x268 (ext 0) */

/* +0x270 (clone-extension slot 1) ENUMERATE the engine-valid classes for an inherit (the linked class
 * dropdown). Resolves Y = sh_typeinfo_inherit_base(inherit), then packs every SH_CLASS_UNIVERSE className that
 * == Y or derives from Y (sh_typeinfo_class_derives) into out_buf as consecutive NUL-terminated strings
 * (double-NUL end marker -- SAME packed-string ABI as +0x110 enum_decls_of_resclass). Returns 1 + *out_count
 * on success, 0 on an unresolvable inherit / empty (the frontend then leaves the combo editable-empty = the
 * free-text hatch). Same sh_typeinfo_class_derives the apply-guard uses -> the dropdown offers EXACTLY what a
 * Save will accept. */
typedef int          (*sh_enum_valid_classes_fn)(struct sh_iface *self, const char *inherit,
                                                 char *out_buf, int cap, int *out_count);          /* +0x270 (ext 1) */

/* +0x278 (clone-extension slot 2) ENUMERATE the complete valid-INHERIT set (the inherit dropdown). Walks the
 * LIVE entityDef decl manager (every loaded entityDef is a valid inherit -- NOT gated by placeable/path) and
 * packs the decl paths into out_buf as consecutive NUL-terminated strings (double-NUL end -- SAME packed ABI
 * as +0x270). Returns 1 + *out_count on success, 0 if the manager is unreachable (the frontend then falls back
 * to its static list). A raw decl-array read -> thread-safe on the Qt UI thread. Replaces the frozen 272-entry
 * static inherit list with the engine's full ~2,500. */
typedef int          (*sh_enum_inherits_fn)(struct sh_iface *self,
                                            char *out_buf, int cap, int *out_count);               /* +0x278 (ext 2) */

/* +0x280 (clone-extension slot 3) DEV-LAYER visibility query for an editor entity id. The SnapMap editor
 * hides "dev layer" entities unless the `snapEdit_enableDevLayer` cvar is 1: an entity is visible iff
 * (entity->layerBits & activeMask) != 0, with activeMask = enableDevLayer ? (devLayerMask|1) : 1 (the
 * engine's own pick/visibility gate). This slot returns 1 iff the entity is currently HIDDEN by that gate
 * (cvar off AND the entity is not in the base layer) -- the Entities + Timelines lists skip those, so they
 * match what the editor shows. Returns 0 when the cvar is on, a read faults, or the editor is down
 * (fail-safe: never hide on uncertainty). A raw layer-bit read -> thread-safe on the Qt UI thread. */
typedef int          (*sh_id_dev_layer_hidden_fn)(struct sh_iface *self, int id);                /* +0x280 (ext 3) */
typedef int          (*sh_wire_edit_generation_fn)(struct sh_iface *self);                       /* +0x288 (ext 4) */

/* ------------------------------------------------------------------ heavy apply slots --------
 * The heavy serialize/deserialize/apply slots the SnapStack APPLY-ops (bss/bsi/bsf/bsb/bse/accl/
 * acctargets/mkcmd) need. These are the native port of the reference implementation's +0xc8 serialize / +0xd0 deserialize-
 * apply / +0xb8 mkcmd-submit. ALL are BACKEND-OWNED (the backend resolves the engine fns by signature +
 * SEH-guards every body); the FRONTEND (Qt) calls them through the vtable at the pinned offsets, doing
 * ONLY the JSON patch in between (QJson for structure + a raw-token splice for the float leaf -- the reference implementation
 * patchFullJsonEdit). The HEAVY structured-deserialize is AV-prone mid-frame (a stale reflection-handler),
 * so the apply does NOT run inline: the frontend SCHEDULEs it (sh_schedule_apply) and the backend drains
 * it at the engine command-buffer exec point (clone_bss_apply -- the reference implementation FIX B). */

/* +0xc8 serialize entity id -> the FULL ~type/|pointer idSnapEntity JSON (the reference implementation serializeEntityToJson).
 * Writes up to cap-1 bytes into out_json (NUL-terminated); returns the byte length written (0 on failure /
 * no map / unbound). The frontend QJson-patches this string, then schedules the apply on the patched text. */
typedef int          (*sh_serialize_entity_fn)(struct sh_iface *self, int id, char *out_json, int cap); /* +0xc8 */

/* A scheduled apply work-item the backend stashes + drains at the clone_bss_apply command-exec point.
 *   kind     : 0=bulkset/bse (deserialize patched_text -> temp def -> commit class/inherit/source on the
 *              live entity `id`); 1=mkcmd (deserialize prefab_text as idSnapEntityPrefab -> editor+0x209a8
 *              -- also used by the Prefabs tab's Load/Place, which just stages and prompts the user to
 *              paste manually with a real Ctrl+V, matching the original's own actual workflow).
 *   id       : the live entity id the patched_text applies to (kind 0). Ignored for mkcmd (kind 1).
 *   text     : the FULL patched entity JSON (kind 0) or the full prefab JSON (kind 1). Backend-copied.
 * The frontend builds these (one per id for the scalar/list ops; one for mkcmd) and hands a batch to
 * sh_schedule_apply, which copies them, registers clone_bss_apply once, and BufferCommandTexts it. */
typedef struct sh_apply_item {
    int         kind;       /* 0 = deserialize-and-commit on `id`; 1 = mkcmd prefab paste (also Load/Place) */
    int         id;         /* the target live entity id (kind 0) */
    const char *text;       /* the patched entity JSON (kind 0) / prefab JSON (kind 1) */
} sh_apply_item;

/* +0xd0 SCHEDULE a batch of apply-items at the engine command-exec point (the reference implementation doBulkSet/doMkcmd ->
 * BufferCommandText). Copies `items` (deep, incl. the text strings) into the backend's pending store,
 * registers the clone_bss_apply engine command once, then enqueues it on the command buffer so the engine
 * drains it on the DOOM main thread (the decl-safe exec point). `op_label` is the op name for the result
 * toast. Returns 1 if scheduled, 0 on a binding gap / editor down. */
typedef int          (*sh_schedule_apply_fn)(struct sh_iface *self, const sh_apply_item *items, int count,
                                             const char *op_label);                              /* +0xd0 */

/* +0xb8 serialize the editor's pending prefab (editor+0x209a8) -> idSnapEntityPrefab JSON (the reference implementation
 * readPrefabStagingJson / shReadPrefabStaging). The mkcmd READ-BACK + the +0x209a8 BUILD-MISMATCH check:
 * a round-trip that returns the staged prefab proves the paste-slot offset on this build. Writes up to
 * cap-1 bytes; returns the length (0 on failure). */
typedef int          (*sh_read_prefab_fn)(struct sh_iface *self, char *out_json, int cap);      /* +0xb8 */

/* One queued work record (a {handler, args} pair) drained on the UI thread by vtable +0x1a0. The bring-up ships
 * the record shape; the producer (the `sh` dispatcher enqueue) + the consumer (drain) are filled in later. */
typedef struct sh_work_item {
    sh_cmd_handler  handler;
    void           *ctx;
    int             argc;
    char          **argv;       /* heap-owned copy of the parsed argv; freed after the handler runs */
} sh_work_item;

/* ------------------------------------------------------------------ the 77-slot vtable -------------
 * Layout PINNED to the OG cell-dump. Every slot is a
 * function pointer; the 77 OG slots span 0x00..0x260 (sizeof 0x268), and clone-extension slots follow after
 * at +0x268+ (append-only, no OG offset moves) - but only the offset of each
 * named slot matters (the frontend calls by offset). The live trio (register/unregister/drain) carry
 * real prototypes; the rest are `void *` pin-and-stub placeholders typed to be filled in later without
 * an offset shift. The _padNN names keep the slot index == byte_offset/8 == the OG vtable index.
 *
 * Slot-index reference (byte offset = index*8):
 *   idx  0 = +0x00 ... idx 0x31 = +0x188 REGISTER ... idx 0x32 = +0x190 UNREGISTER ...
 *   idx 0x34 = +0x1a0 DRAIN ... idx 0x4c = +0x260 (last). 77 slots total (0..76).
 */
typedef struct sh_iface sh_iface;   /* fwd */

/* +0x00/+0x08 the editor CAMERA-ORIGIN vec3 (3 floats at editor+0x170) -- the Camera Origin X/Y/Z + Lock Position. */
typedef void (*sh_set_editor_vec3_fn)(struct sh_iface *self, const float *xyz);   /* +0x00 (0x64a0) */
typedef void (*sh_get_editor_vec3_fn)(struct sh_iface *self, float *out_xyz);      /* +0x08 (0x6500) */
typedef int  (*sh_editor_ready_fn)(struct sh_iface *self);                         /* +0x88 (0x6b40) editor-ready */

typedef struct sh_iface_vtbl {
    /* +0x00..+0x180 : engine-touching slots (set vec3 / counts / decl read+write / serialize / apply /
     * select / toast / clipboard / ...). PINNED here as raw void* placeholders -- the backend fills the
     * real bodies later; the frontend calls them through these offsets. Names mirror the truth table. */
    sh_set_editor_vec3_fn set_editor_vec3;  /* +0x00  (0x64a0) SET the camera-origin vec3 (editor+0x170) */
    sh_get_editor_vec3_fn get_editor_vec3;  /* +0x08  (0x6500) GET the camera-origin vec3 */
    sh_entity_count_fn entity_count;/* +0x10  (0x6550) ENTITY COUNT (C2-LIVE) */
    sh_id_to_string_fn id_to_string;/* +0x18  (0x6580) resolve id->string (mkcmd) (C2-LIVE) */
    void *module_index_of;          /* +0x20  (0x6e50) */
    sh_is_valid_id_fn is_valid_id;  /* +0x28  (0x6e60) IS-VALID id (C2-LIVE) */
    sh_get_declsource_fn get_declsource_copy; /* +0x30 (0x65b0) decl-source COPY (C3-LIVE, Entity-State read) */
    void *get_declsource_ptr;       /* +0x38  (0x6640) */
    sh_rebuild_declsource_fn rebuild_set_declsource; /* +0x40 (0x6850) REBUILD+SET decl source = Save-to-Decl route (C3-LIVE) */
    sh_classname_fn get_classname_copy; /* +0x48 (0x68e0) classname (C2-LIVE, filtcls) */
    sh_inherit_fn get_inherit_copy; /* +0x50  (0x6980) inherit (C2-LIVE, filtinh) */
    sh_get_displayname_fn get_displayname; /* +0x58 (0x7230) displayname (C3-LIVE, Entity-State read) */
    void *get_classname_ptr;        /* +0x60  (0x8150) */
    void *get_inherit_ptr;          /* +0x68  (0x81b0) */
    void *get_displayname_ptr;      /* +0x70  (0x8210) */
    sh_set_classname_fn set_classname; /* +0x78 (0x6a20) SET classname (C3-LIVE, Save-to-Decl) */
    sh_set_inherit_fn set_inherit;  /* +0x80  (0x6ab0) SET inherit (C3-LIVE, Save-to-Decl) */
    sh_editor_ready_fn editor_ready_poll; /* +0x88 (0x6b40) per-frame editor-ready poll (window gate) */
    void *enqueue_cmd_record;       /* +0x90  (0x66a0) ENQUEUE {string} command record */
    void *enqueue_cmd_fmt;          /* +0x98  (0x67b0) vsnprintf then self+0x90 */
    void *engine_call_a;            /* +0xa0  (0x6b60) */
    void *engine_call_b;            /* +0xa8  (0x6b80) */
    sh_serialize_selection_fn serialize_selection; /* +0xb0 (0x6ba0) serialize SELECTION -> idSnapEntityPrefab text (C3-LIVE) */
    sh_read_prefab_fn read_prefab;  /* +0xb8  (0x6bf0) mkcmd-submit / READ-BACK editor+0x209a8 */
    sh_resolve_prefab_path_fn resolve_prefab_path; /* +0xc0 (0x6bc0) resolve PREFAB file path (C3-LIVE) */
    sh_serialize_entity_fn serialize_entity; /* +0xc8 (0x6d50) serialize entity id -> idSnapEntity JSON */
    sh_schedule_apply_fn apply_edit;/* +0xd0  (0x6d70) SCHEDULE+APPLY edit batch (SnapStack bs-ops + mkcmd) */
    void *catalog_count;            /* +0xd8  (0x6d80) */
    void *catalog_class_u32;        /* +0xe0  (0x6db0) */
    void *catalog_class_name;       /* +0xe8  (0x6dd0) */
    void *catalog_event_name;       /* +0xf0  (0x6df0) */
    void *catalog_event_desc;       /* +0xf8  (0x6e20) */
    void *enum_decl_list;           /* +0x100 (0x6eb0) */
    void *enum_decls_of_restype;    /* +0x108 (0x6ff0) */
    sh_enum_decls_of_resclass_fn enum_decls_of_resclass; /* +0x110 (0x70b0) ENUM decls of a resource class
                                     * (Timeline-Editor constrained decl-comboboxes; C3b-LIVE) */
    void *parse_json_file;          /* +0x118 (0x7190) */
    void *spawn_idsnapentity;       /* +0x120 (0x71a0) */
    sh_set_displayname_fn set_entity_0x170; /* +0x128 (0x72a0) SET displayname entity+0x170 (C3-LIVE) */
    sh_remove_from_selection_fn selection_guard; /* +0x130 (0x73c0) REMOVE id from selection (C3-LIVE, Delete) */
    sh_add_to_selection_fn add_to_selection; /* +0x138 (0x73f0) ADD to selection (C2-LIVE, popsel) */
    void *remove_from_selection;    /* +0x140 (0x7420) */
    sh_clear_selection_fn clear_selection;   /* +0x148 (0x7450) CLEAR selection (C2-LIVE, psel) */
    sh_get_selection_fn get_selection;       /* +0x150 (0x7480) GET selection (C2-LIVE, psel) */
    void *id_guarded_0x51f890;      /* +0x158 (0x74b0) */
    void *const_0x37c;              /* +0x160 (0x7510) */
    void *classname_by_index;       /* +0x168 (0x7520) */
    void *or_render_flags;          /* +0x170 (0x7530) OR render/debug flags */
    void *clipboard_write;          /* +0x178 (0x75d0) DEAD: 0 callers this build */
    void *clipboard_read;           /* +0x180 (0x75e0) DEAD: 0 callers this build */

    /* ---- the live trio: REGISTER / UNREGISTER / DRAIN (real prototypes; backend fills bodies) ---- */
    /* +0x188 (0x7a00) REGISTER subcommand(name, handler, ctx) -> the cmd-map at obj+0x58 */
    void (*register_cmd)(sh_iface *self, const char *name, sh_cmd_handler handler, void *ctx);
    /* +0x190 (0x7ba0) UNREGISTER subcommand by name */
    void (*unregister_cmd)(sh_iface *self, const char *name);
    sh_hovered_id_fn hovered_id;    /* +0x198 (0x7d30) get hovered id (C2-LIVE, phov) */
    /* +0x1a0 (0x7d50) DRAIN+run the work-queue under the mutex (called per-frame on the UI thread) */
    void (*drain_work_queue)(sh_iface *self);
    void *input_state_b;            /* +0x1a8 (0x7e30) */
    void *input_state_a;            /* +0x1b0 (0x7e50) */
    sh_toast_fn toast;              /* +0x1b8 (0x7e70) TOAST/notification(label,text) (C2-LIVE) */
    sh_is_entity_mode_fn is_entity_mode;  /* +0x1c0 (0x7f30) 1 when tabbed IN a module (editor+0x23618==2);
                                           * the Create-New-Timeline gate + Qt button gray-out. */
    void *is_module_mode;           /* +0x1c8 (0x7f50) */
    void *is_entering_entity_mode;  /* +0x1d0 (0x7f70) */
    void *declmgr_lookup_void;      /* +0x1d8 (0x7f90) */
    void *declmgr_lookup;           /* +0x1e0 (0x7fe0) */
    /* +0x1e8..+0x260 : generic struct-field accessors + declMgr lookups + double->idStr (the exhaustive
     * tail in the truth artifact). The 77 OG slots end at +0x260 (0x268 for the OG block); the clone-
     * extension slots (apply_class_inherit @ +0x268, ...) follow after -- sizeof grows, no OG offset moves;
     * bodies stubbed. */
    void *acc_0x1e8;                /* +0x1e8 (0x8030) */
    void *acc_0x1f0;                /* +0x1f0 */
    void *acc_0x1f8;                /* +0x1f8 */
    void *acc_0x200;                /* +0x200 */
    void *acc_0x208;                /* +0x208 */
    void *acc_0x210;                /* +0x210 declMgr lookup (guarded) */
    void *acc_0x218;                /* +0x218 */
    void *acc_0x220;                /* +0x220 */
    void *acc_0x228;                /* +0x228 */
    void *acc_0x230;                /* +0x230 */
    void *acc_0x238;                /* +0x238 */
    void *acc_0x240;                /* +0x240 */
    void *acc_0x248;                /* +0x248 */
    void *acc_0x250;                /* +0x250 declMgr lookup (guarded) */
    void *acc_0x258;                /* +0x258 is-entity-array-readable (IsBadReadPtr) */
    void *acc_0x260;                /* +0x260 double -> idStr (LAST OG slot, idx 76) */

    /* ---- clone-extension slots (AFTER the 77 OG slots; clone-own ABI, never an OG offset; the object holds
     * a vtbl POINTER so the 0x60 object size is unchanged; both DLLs rebuild from this header as a pair). ---- */
    sh_apply_class_inherit_fn apply_class_inherit;   /* +0x268 (ext 0) ATOMIC class+inherit set */
    sh_enum_valid_classes_fn  enum_valid_classes;    /* +0x270 (ext 1) class-dropdown enumerator */
    sh_enum_inherits_fn       enum_inherits;         /* +0x278 (ext 2) inherit-dropdown enumerator */
    sh_id_dev_layer_hidden_fn id_dev_layer_hidden;   /* +0x280 (ext 3) dev-layer entity-hidden query */
    sh_wire_edit_generation_fn wire_edit_generation; /* +0x288 (ext 4) wire-any connect-edit generation counter
                                                      * (entity-list re-read signal; see wiring_cleandirect.c) */
} sh_iface_vtbl;

/* ------------------------------------------------------------------ the interface object -----------
 * Object layout PINNED to FUN_1800229b1: +0x00 vtable, +0x08 mutex, +0x58 sub-object. The mutex is an
 * opaque blob sized to the MSVCRT `_Mtx_t` the OG initializes with `_Mtx_init_in_situ`; we hold it as a
 * raw byte blob (the backend owns the mutex's real lifecycle via the OS primitive it wraps) so the
 * struct's binary layout is exact regardless of which CRT each DLL links. The frontend NEVER touches
 * +0x08..+0x57 directly -- only +0x00 (vtable) and +0x58 (via the vtable slots).
 *
 * SH_IFACE_MTX_BLOB: the OG _Mtx is a pointer to a heap _Mtx_internal_imp_t; `_Mtx_init_in_situ` writes
 * an 8-byte pointer at obj+8 (+ the structure it points to). We reserve 0x50 bytes (+0x08..+0x57) so the
 * sub-object lands EXACTLY at +0x58 -- the backend's mutex impl stores its handle within this window. */
#define SH_IFACE_VTBL_OFF   0x00
#define SH_IFACE_MTX_OFF    0x08
#define SH_IFACE_SUB_OFF    0x58    /* obj[0xb] -> the 0x78-byte sub-object (cmd-map + work-queue) */

/* The +0x58 sub-object. Layout pinned to the OG: the RB-tree map head at sub+0x00 + the work-queue
 * vector at sub+0x60/+0x68/+0x70. We expose only the fields the bring-up and apply paths need; the entity stacks/groups (the
 * SnapStack stores) live in the tail and are added later. */
typedef struct sh_iface_sub {
    void          *map_nil;         /* sub+0x00  RB-tree nil/head node (operator_new(0x48), 0x101 magic) */
    void          *map_root;        /* sub+0x08 */
    uint64_t       map_size;        /* sub+0x10 */
    uint8_t        _mtx2[0x48];     /* sub+0x18  the map's own _Mtx (OG _Mtx_init_in_situ(puVar3+2)) */
    /* the work-queue std::vector (begin/end/cap) -- DRAIN (+0x1a0) runs [begin,end) then resets */
    sh_work_item  *wq_begin;        /* sub+0x60 */
    sh_work_item  *wq_end;          /* sub+0x68 */
    sh_work_item  *wq_cap;          /* sub+0x70 */
    /* sub+0x78.. : entity stacks/groups (the SnapStack stores) -- added in C2 */
} sh_iface_sub;

struct sh_iface {
    const sh_iface_vtbl *vtbl;      /* +0x00 */
    uint8_t              mtx[0x50]; /* +0x08..+0x57  the object's _Mtx blob (backend-owned) */
    sh_iface_sub        *sub;       /* +0x58  -> the cmd-map + work-queue sub-object */
};

/* ------------------------------------------------------------------ the CreateThread arg block -----
 * snaphak_ui_init receives `param_1` = a pointer to this block (OG &DAT_18003e5e0). The OG accesses:
 *   param_1[0] = out-slot   (snaphak_ui_init writes the loop-state obj here: `*param_1[0] = DAT_180031858`)
 *   param_1[1] = argc       (passed to QApplication as `int*`)
 *   param_1[2] = argv       (passed to QApplication as `char**`)
 *   param_1[3] = interface  (= DAT_18003e608 = the sh_iface* the frontend caches as WIN[4])
 * (DIRECT: snaphak_ui_init @0x129d0 -- QApplication(.., param_1+1, param_1[2], ..); FUN_180012bac(.., param_1[3]).)
 * The OG block is wider (e.g. _DAT_18003e5f0/5e8 carry an export table + a flag the frontend ignores in
 * the init path); the bring-up ships the 4 init-relevant slots the frontend reads. */
typedef struct sh_ui_argblock {
    void     *out_slot;             /* [0] frontend writes the loop-state obj address here */
    int       argc;                 /* [1] QApplication argc (read as int*) */
    char    **argv;                 /* [2] QApplication argv */
    sh_iface *iface;                /* [3] the shared interface object (backend-owned) */
} sh_ui_argblock;

/* ------------------------------------------------------------------ backend factory ----------------
 * Build a minimal interface object: allocate it, install the vtbl, init the mutex, allocate the
 * sub-object (empty cmd-map + empty work-queue), wire the REGISTER/UNREGISTER/DRAIN bodies. Hosted in
 * the backend (snaphak_iface.c). The `sh` dispatcher gates on the returned pointer; NULL -> "Ui
 * interface doesnt exist yet!". Returns NULL on allocation failure. */
sh_iface *sh_iface_create(void);

/* ------------------------------------------------------------------ cmd-map lookup -------------
 * Look the subcommand `name` up in the interface's runtime cmd-map (the obj+0x58 RB-tree the OG's
 * register path populates; our backing store is the sub_impl's linear map). On a hit, fills *handler +
 * *ctx with the registered pair and returns 1; on a miss returns 0. Taken under the cmd-map's lock.
 * The `sh` dispatcher (XINPUT 0x7620 port) calls this to decide enqueue-or-"not registered". */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx);

/* ------------------------------------------------------------------ work-queue enqueue ---------
 * Append a {handler, ctx, argc, argv-copy} record onto the interface's work-queue (the sub+0x60/+0x68/
 * +0x70 vector) under the mutex, for MAIN-THREAD execution by the think-loop's +0x1a0 drain. The argv
 * strings are DEEP-COPIED here (heap-owned by the record), so the caller's argv may be freed/reused
 * after the call -- the drain frees the copy once the handler has run. Returns 1 on success, 0 on OOM /
 * a null interface. This is the producer the OG `sh` dispatcher (0x7620) is. */
int sh_iface_enqueue_work(sh_iface *self, sh_cmd_handler handler, void *ctx,
                          int argc, const char **argv);

/* ------------------------------------------------------------------ engine-slot bind ----
 * The backend-provided bodies for the engine-touch vtable slots the SnapStack STORE-ops need. The backend
 * resolves the editor singleton + the selection/toast engine fns by signature, then calls
 * sh_iface_bind_engine_slots once at install to patch the shared vtable. A NULL body leaves the slot NULL
 * (the frontend null-checks). The heavy serialize/apply slots (+0xc8/+0xd0) are NOT here (bound later). */
typedef struct sh_iface_engine_slots {
    sh_set_editor_vec3_fn   set_editor_vec3;     /* +0x00 */
    sh_get_editor_vec3_fn   get_editor_vec3;     /* +0x08 */
    sh_entity_count_fn      entity_count;        /* +0x10 */
    sh_id_to_string_fn      id_to_string;        /* +0x18 */
    sh_is_valid_id_fn       is_valid_id;         /* +0x28 */
    sh_editor_ready_fn      editor_ready_poll;   /* +0x88 (window gate) */
    sh_classname_fn         get_classname_copy;  /* +0x48 */
    sh_inherit_fn           get_inherit_copy;    /* +0x50 */
    sh_add_to_selection_fn  add_to_selection;    /* +0x138 */
    sh_clear_selection_fn   clear_selection;     /* +0x148 */
    sh_get_selection_fn     get_selection;       /* +0x150 */
    sh_hovered_id_fn        hovered_id;          /* +0x198 */
    sh_is_entity_mode_fn    is_entity_mode;      /* +0x1c0 (Create-New-Timeline gate / Qt gray-out) */
    sh_toast_fn             toast;               /* +0x1b8 */
    /* the heavy serialize / schedule-apply / prefab-readback slots (iface_engine.c fills the
     * bodies once the full apply-chain engine fns are signature-resolved). */
    sh_serialize_entity_fn  serialize_entity;    /* +0xc8 */
    sh_schedule_apply_fn    apply_edit;          /* +0xd0 */
    sh_read_prefab_fn       read_prefab;         /* +0xb8 */
    /* the DATA-tab slots (Entity-State read/write + Prefabs serialize/path + Delete). */
    sh_get_declsource_fn       get_declsource_copy;   /* +0x30  */
    sh_rebuild_declsource_fn   rebuild_set_declsource;/* +0x40  */
    sh_get_displayname_fn      get_displayname;       /* +0x58  */
    sh_set_classname_fn        set_classname;         /* +0x78  */
    sh_set_inherit_fn          set_inherit;           /* +0x80  */
    sh_set_displayname_fn      set_displayname;       /* +0x128 */
    sh_serialize_selection_fn  serialize_selection;   /* +0xb0  */
    sh_resolve_prefab_path_fn  resolve_prefab_path;   /* +0xc0  */
    sh_remove_from_selection_fn remove_from_selection;/* +0x130 */
    /* the Timeline-Editor constrained decl-combobox enumerator (+0x110). */
    sh_enum_decls_of_resclass_fn enum_decls_of_resclass;/* +0x110 */
    /* clone-extension: the atomic class+inherit morph. */
    sh_apply_class_inherit_fn    apply_class_inherit;   /* +0x268 (ext 0) */
    /* clone-extension: the class-dropdown enumerator. */
    sh_enum_valid_classes_fn     enum_valid_classes;    /* +0x270 (ext 1) */
    /* clone-extension: the inherit-dropdown enumerator. */
    sh_enum_inherits_fn          enum_inherits;         /* +0x278 (ext 2) */
    /* clone-extension: the dev-layer entity-hidden query. */
    sh_id_dev_layer_hidden_fn    id_dev_layer_hidden;   /* +0x280 (ext 3) */
    /* clone-extension: the wire-any connect-edit generation counter (entity-list re-read signal). */
    sh_wire_edit_generation_fn   wire_edit_generation;  /* +0x288 (ext 4) */
} sh_iface_engine_slots;

void sh_iface_bind_engine_slots(const sh_iface_engine_slots *slots);

#ifdef __cplusplus
}
#endif

#endif /* SNAPHAK_IFACE_H */
