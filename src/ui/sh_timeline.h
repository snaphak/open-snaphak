/* sh_timeline.h -- the Timeline-Editor (tab 5): the per-timeline QTabWidget TL + the bidirectional
 * componentTimeLine binding + the per-arg-type widget dispatch + the +0xd0 commit.
 *
 * A clean-room, FAITHFUL port of the OG snaphakui.dll Timeline-Editor (the RE'd OG chain:
 * FUN_1800102a0 / FUN_180012458 / FUN_1800122a4 / FUN_180011a88 / FUN_180010ee0 /
 * FUN_1800120a4 / FUN_1800116c4 / FUN_180010b68 / FUN_18000a730 / FUN_18000994c). The OG chain was decompiled
 * from the original snaphakui.dll. The collect chain is FUN_180012458/FUN_1800122a4/FUN_1800102a0/FUN_1800120a4;
 * the eventcall is FUN_180010ee0/FUN_180010b68; the arg dispatch is FUN_18000a730/FUN_18000994c; the |0x80 commit
 * guard lives inside FUN_180014e7c. The supporting fns are FUN_180011e9c (event-row), FUN_180011288
 * (EventTime-row), FUN_180010274 (is-valid guard), FUN_180012018 (teardown). Zero OG bytes; our own C++ on Qt 5.9.
 *
 * THE OG SHAPE (DIRECT):
 *   - the live editor QTabWidget `TL` (built by FUN_1800102a0) is stored WIN[0x3b]; the interface obj is
 *     cached TL+0x100; the timeline entity id being edited is TL+0xf8 (-1 sentinel); the built value-tree is
 *     TL+0x90; the per-tab row-widgets / scrollareas vectors are TL+0x30..0x58.
 *   - one inner TAB per `entityEvents.item[i]` (a per-entity event list): an entity QComboBox (the entity the
 *     events run ON) + a scrollarea of event-rows.
 *   - each event-row: an EventTime QLineEdit + an eventDef QComboBox + per-arg widgets.
 *   - per-arg widget = the arg-type dispatch FUN_18000a730: float/color/angles/string/entity dedicated;
 *     path-family shared; idDecl* -> a QComboBox CONSTRAINED to valid engine decl values via interface +0x110;
 *     non-idDecl declMgr-resolvable -> +0x100; else a string box.
 *   - COLLECT (UI->tree, save_entity_timeline.clicked -> WIN[0]|0x80): FUN_180012458 chain builds the
 *     componentTimeLine tree at TL+0x90 + sets the canonical className/inherit; the commit
 *     `if(TL+0xf8 != -1) (*(*(TL+0x100)+0xd0))(iface, TL+0xf8, TL+0x90)` applies it onto the live entity.
 *   - decl args emit the NESTED `{<declType>:<declName>}` reader/LOAD form (the GOOD form, per
 *     startSoundShader arg =
 *     {"decl":{"sound":<name>}}); the outer arg key is "decl" (R2, strong-inferred -> emit "decl").
 *
 * THE CLONE'S APPLY PATH (reuses the working apply chain -- do NOT perturb it):
 *   The OG's TL+0x90 is its own idDict value-tree; the clone instead reuses the +0xc8 serialize / +0xd0
 *   schedule path (the same path bss/Save-to-Decl ride). COLLECT-on-open serializes the timeline entity
 *   (iface +0xc8), parses the existing componentTimeLine out of entityDef.state.edit, and builds the UI from
 *   it. COMMIT rebuilds componentTimeLine from the UI model, re-serializes the entity fresh, patches
 *   entityDef.state.edit.componentTimeLine, and SCHEDULEs it via iface +0xd0 (the clone_bss_apply main-
 *   thread path). So the timeline commit IS a bss-style apply on the timeline entity id -- decl-safe + tested.
 *
 * TIMELINE CREATION is NOT done here: there is no clone-side "create timeline" path (both the from-scratch
 * SPAWN and the reclass-a-selected-entity MORPH corrupted the map -- the clone cannot fabricate a timeline
 * entity outside the engine's own creation path). A timeline is created by PLACING one from the in-game SnapMap
 * entity palette (a built-in decl override makes it selectable there); this Timeline-Editor then AUTHORS events
 * on the already-placed, engine-validated timeline.
 *
 * FAITHFUL QUIRK (reproduced deliberately):
 *   - the `componentTimeline`->`componentTimeLine` canonicalization (FUN_180012458) is reproduced.
 */
#ifndef SH_TIMELINE_H
#define SH_TIMELINE_H

struct ShWinController;
struct sh_iface;

/* Rewrite a palette-placed Timeline's `inherit` from our `snapmaps/editor_only/placeholder_target` override to
 * the universal `snapmaps/unknown` so the saved map is portable (reloads without our override). className stays
 * idTarget_Timeline (no reclass -> no crash surface); a raw-JSON splice + the main-thread bss apply. Idempotent
 * one-shot; returns true iff a normalize was scheduled. Called from the sh_tabs poll on a world change. */
bool sh_timeline_normalize_inherit(sh_iface *iface, int id);

/* OPEN the Timeline-Editor on the given timeline entity id (the Timelines-tab double-click target, or a
 * Timeline-Editor selection). Builds the per-timeline QTabWidget TL, stores it on win->timeline_tl (OG
 * WIN[0x3b]), caches the interface + the entity id, COLLECTs the live componentTimeLine decl into the UI
 * rows. `id` < 0 (or an unresolved/invalid id) -> the editor opens with entity_id = -1 (the broken
 * Create-New-Timeline shape: the commit guard will skip). Idempotent-ish: re-opening rebuilds the TL. */
void sh_timeline_open(ShWinController *win, int id);

/* The Timelines-tab double-click consumer rewire (C3b): the OG FUN_180017444 stashes the chosen id onto the
 * Timeline-Editor's deferred-load slot (TL+0x1d0) + switches to tab 5; here we open the editor directly on
 * the clicked id. Called from sh_timeline_item_double_clicked (sh_tabs.cpp) via win->pending_select_id. */
void sh_timeline_open_pending(ShWinController *win);

/* insert_entity_event.clicked (port FUN_180011e9c / FUN_180011288): append an empty event-row to the
 * currently-shown inner tab (a new EventTime line-edit + eventDef combobox + an empty arg area). */
void sh_timeline_insert_event(ShWinController *win);

/* The |0x80 APPLY_TIMELINE consumer (save_entity_timeline.clicked -> WIN[0]|0x80 -> here): COLLECT the UI
 * rows back into the nested componentTimeLine decl object (the GOOD {<declType>:<declName>} form), build the
 * full patched entity JSON, and APPLY via interface +0xd0 onto the timeline entity (entity_id) reusing the
 * C2 clone_bss_apply main-thread path. Guarded `if(entity_id != -1)` (the OG TL+0xf8 != -1 commit guard) --
 * a broken/Create-New timeline (entity_id == -1) is a no-op (faithful R6). No-op if no editor is open. */
void sh_timeline_commit(ShWinController *win);

#endif /* SH_TIMELINE_H */
