/* sh_timeline.cpp -- the SnapMap Timeline-Editor (tab 5 of the SnapHak Studio window). See sh_timeline.h.
 *
 * ===================================================================================================
 * WHAT A SNAPMAP TIMELINE IS  (read this first if you have never touched this system)
 * ===================================================================================================
 * A "timeline" is a SnapMap entity (an idTarget_Timeline, or an idEncounterManager for encounters) that acts
 * as a SCHEDULER: it fires EVENTS on OTHER entities at scheduled times. You place a timeline in your map,
 * point it at some entities, and say "at t=0ms fire `bind` on the demon; at t=500ms fire `startSoundShader`
 * on the speaker; ...". When the timeline is triggered in-game it walks its schedule and dispatches each event.
 *
 * The schedule is a tree -- the `componentTimeLine` (the exact on-disk shape is at the bottom of this header):
 *   timeline entity
 *     |__ entityEvents[]          one entry PER TARGET ENTITY  (the editor shows each as an inner tab "item[N]")
 *          |__ entity             WHICH entity these events run ON (a map module-id string)
 *          |__ events[]           the events to fire on that entity (the editor: one row per event in the tab)
 *               |__ event
 *                    |__ eventCall
 *                    |    |__ eventDef    WHICH event   (e.g. "bind", "showModel", "startSoundShader")
 *                    |    |__ args[]      the event's parameters, each {<argType>: <value>}
 *                    |__ eventTime        WHEN to fire it (milliseconds; optional)
 *
 * ===================================================================================================
 * HOW TIMELINES INTERACT WITH ENTITIES  (the engine runtime model)
 * ===================================================================================================
 * Every SnapMap entity is an instance of an engine CLASS (an idEntity subclass -- idAI, idTarget, idSpeaker,
 * a turret class, ...). Each class registers the set of EVENTS it knows how to handle (the idTech idClass /
 * idEventDef EVENT() registry -- a per-class {eventDef -> handler fn} map). At runtime, when the timeline
 * fires an eventCall on a target entity, the engine looks the eventDef up in the TARGET ENTITY'S CLASS map:
 *   - the class handles it -> the handler runs with the args (idAI handles `bind`; a turret handles
 *                             `ae_turretFire`; a render entity handles `showModel`);
 *   - the class does NOT   -> the event is a silent no-op.
 * So an event is only MEANINGFUL on an entity whose class handles it, and an event's ASSET args (an animation,
 * a model index, a skeleton tag) only have valid values that come FROM that target entity (its md6 skeleton,
 * its render models). `ae_turretFire`'s animHandle must be one of the TURRET'S md6 anims; it is meaningless on
 * a keycard. This is the key fact that drives the arg-editor dispatch + the (still-to-build) coupling framework.
 *
 * ===================================================================================================
 * THE EDITOR'S JOB + THE PER-ARG-TYPE EDITOR DISPATCH  (tl_classify_arg / tl_build_arg_widget)
 * ===================================================================================================
 * This file builds the UI to author that schedule and serialize it back. The central decision is, per event
 * arg, WHICH editor widget to show. Two families:
 *   FREE VALUES (no enumerable set -> a typed editor; a dropdown would be meaningless):
 *     int/float -> a number box;  string -> a text box;  vec3/angles -> x/y/z fields;  color -> r/g/b/a fields.
 *   SELECTABLE VALUES (a known valid set -> a dropdown):
 *     entity      -> the map's entities (the shared entity model);
 *     idDecl*     -> the engine's valid decl names of that resource class (idSoundShader* -> the sound decls);
 *     <X>_t enum  -> the enum's members (soundChannel_t -> {SND_CHANNEL_*});
 *     modelIndex / animWebPath / md6Anim / animAlias -> values SCOPED TO THE TARGET ENTITY'S CLASS, looked up
 *       in the GENERATED sh_entity_asset_lists.h by the entity's `inherit` slug (= entityDef.inherit, obtained
 *       by serializing the entity and reading one JSON field; see tl_entity_inherit_slug). The OG TYPE-INS all
 *       of these -- the per-entity dropdowns are the clone EXCEEDING the OG.
 *
 * ===================================================================================================
 * KNOWN GAP / THE FRAMEWORK STILL TO BUILD  (entity <-> valid-events coupling)
 * ===================================================================================================
 * The event PICKER currently offers ALL 1611 engine event-defs for ANY entity -- faithful to the OG, which is
 * itself unconstrained. THAT is why you can pick an event the target entity's class cannot service, and then
 * its asset args show blank/typed (the entity has no such asset). The proper fix is a FRAMEWORK that couples
 * each entity CLASS to (a) its set of valid events, and (b) each event's per-entity arg value-sources -- so the
 * picker only offers events the entity can handle and every asset arg resolves. The per-entity asset lists in
 * sh_entity_asset_lists.h are the FIRST HALF of that framework; the class->valid-events data source (the
 * entitydef inputs grammar vs the engine per-class event-map) is under RE (timeline-entity-event-coupling-re).
 *
 * ===================================================================================================
 * IMPLEMENTATION  (the clean-room port + the on-disk shape + the apply path)
 * ===================================================================================================
 * Clean-room FAITHFUL port of the OG snaphakui.dll Timeline-Editor. The RE'd OG call-map (the
 * create-timeline-re RE, DIRECT decompile): OPEN = FUN_1800127a0 (sets TL+0xf8 = the selected entity index) ->
 * FUN_1800120a4 (populate the tabs from the parsed componentTimeLine). COMMIT-BUILDER = FUN_180012458 (rebuild
 * the entity JSON, FORCING className="idTarget_Timeline" + inherit="snapmaps/unknown", write componentTimeLine),
 * fired by the save handler FUN_1800173bc setting WIN flag |0x80, applied by the per-frame dispatch FUN_180014e7c
 * (`if(TL+0xf8 != -1)` -> iface +0xd0 onto TL+0xf8). (NB: the earlier note "collect FUN_1800102a0" was WRONG --
 * FUN_1800102a0 is the event-NAME catalog + ds_descriptions loader, not the open/collect.) Zero OG bytes.
 *
 * THE ON-DISK SHAPE (DIRECT):
 *   componentTimeLine = { entityEvents:{ num, item[i]:{   (NB: NO top-level num -- count lives in entityEvents)
 *     entity:"<full module id>",
 *     events:{ "\nnum":K, item[j]:{
 *       "\neventHandle_t eventDef":"<EVENT NAME>",          // the engine's literal key (note the leading \n)
 *       args:{ "\nnum":K, item[k]:{ "<argTypeName>": <value> } },
 *       eventTime:<ms int>                                  // emitted ONLY if the EventTime box parses as uint
 *   }}}}}
 *   A decl-pointer arg is the NESTED form, outer key "decl": startSoundShader arg0 = {"decl":{"sound":"<shader>"}};
 *   an enum arg is {<enumType>:<MEMBER>}; modelIndex is a plain {int:<index>}.
 *
 * THE APPLY PATH (reuses the working bss-apply chain -- do NOT perturb ae_serialize_to_json / the memsets):
 *   OPEN  (tl_collect_from_decl): iface +0xc8 serialize the timeline entity -> parse
 *          entityDef.state.edit.componentTimeLine -> build the tabs/rows.
 *   COMMIT (sh_timeline_commit):  rebuild componentTimeLine from the UI -> serialize the entity fresh -> patch
 *          the componentTimeLine back in -> SCHEDULE via iface +0xd0 (the main-thread clone_bss_apply) onto
 *          entity_id. So a timeline commit IS a decl-safe bss-style apply on the timeline entity.
 *
 * CREATE-NEW-TIMELINE (clone EXCEEDS OG): in OG the "Create New Timeline" button is UNWIRED -- it has no slot
 * (no connectImpl, no on_*_clicked auto-slot; create-timeline-re DIRECT), so clicking it does nothing and a new
 * timeline could only be reached by selecting an already-placed idTarget_Timeline. The clone IMPLEMENTS it as a
 * real feature (sh_timeline_create_new below): gated on EntityMode (tabbed inside a module), it either MORPHS the
 * selected entity into a timeline host or SPAWNS a fresh one grabbed at the camera. The engine canonicalizes
 * componentTimeline -> componentTimeLine (we always emit the L-form).
 */
#include "sh_timeline.h"
#include "sh_controller.h"
#include "snaphak_iface.h"
#include "sh_event_catalog.h"   /* GENERATED data table: the engine event-def catalog that
                                 * drives the event-picker dropdown -- the OG builds it live from the engine
                                 * eventMgr; we embed the engine's source-of-record event dump. */
#include "sh_entity_asset_lists.h"  /* GENERATED data table: per-entity-class asset value
                                 * lists (render-model names, anim-web paths), keyed by the entity's inherit slug.
                                 * Feeds the EXCEED-the-OG modelIndex/animWebPath dropdowns (the OG type-ins these). */
#include "sh_event_docs.h"      /* GENERATED data table: OUR
                                 * author-facing descriptions for the 1611 events (the engine ships none). Feeds the
                                 * EXPLAIN description box -- the rich "what this event does" + per-arg prose. */
#include "mkcmd_template.inc"   /* SH_MKCMD_PREFAB_TEMPLATE: the BYTE-EXACT, engine-accepted idSnapEntityPrefab
                                 * (also used by snapstack.cpp::h_mkcmd). The Create-New-Timeline SPAWN builds from
                                 * THIS (substituting className/displayName/edit-body) so its float leaves + customIcon
                                 * are engine-correct by construction -- a Qt-rebuilt prefab dropped the float ".0"
                                 * ("...grabAxis.mat[0].z is not a floating point") + omitted customIcon. */

#include <string>
#include <vector>
#include <unordered_map>   /* O(1) name->index maps -- replaces the O(model-size) findText in the load path */
#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QMainWindow>
#include <QTabWidget>
#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QString>
#include <QByteArray>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QStandardItemModel>   /* the SHARED event/entity combo models (built once, not per row/tab) */
#include <QFormLayout>          /* legible per-event arg layout (label: editor) */
#include <QElapsedTimer>        /* [DIAGNOSTIC] open-phase timing to root-cause the busy-timeline freeze */
#include <QIdentityProxyModel>  /* SCOPE: per-tab event-picker proxy that dims events unfillable by the entity */
#include <QColor>
#include <QCompleter>           /* FLOW: type-to-filter the 1611-event picker (substring match) */

/* SCOPE (the entity<->event coupling, advisory form): a 1:1 proxy over the shared event model that DIMS the
 * events whose args cannot resolve for the tab's entity (RE timeline-entity-event-coupling-re, Path A -- an event
 * is "fillable" iff every asset arg has a list on this entity). Greyed = "this won't do anything useful here",
 * but still SELECTABLE (out-of-class events are engine no-ops, so we never block -- no escape hatch needed). The
 * index stays 1:1 so combo row == catalog index. moc-free (only data() is overridden). */
class ShEventScopeProxy : public QIdentityProxyModel {
public:
    ShEventScopeProxy(QObject *parent) : QIdentityProxyModel(parent) {}
    void setValid(std::vector<char> v) {
        valid.swap(v);
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, 0));   /* repaint the combos */
    }
    QVariant data(const QModelIndex &idx, int role) const override {
        if (role == Qt::ForegroundRole && idx.isValid() &&
            idx.row() < (int)valid.size() && !valid[(size_t)idx.row()])
            return QColor(0x99, 0x99, 0x99);   /* dim: args don't resolve for this entity */
        return QIdentityProxyModel::data(idx, role);
    }
private:
    std::vector<char> valid;
};

/* ================================================================ iface wrappers ==================== */
/* Thin null-checked wrappers over the interface vtable slots the Timeline-Editor needs (the backend SEH-
 * guards every body). Mirror the sh_tabs.cpp / snapstack.cpp helper style -- a partial backend build
 * degrades cleanly (empty result / no-op), never crashes. */

#define SH_TL_JSON_CAP   (256 * 1024)   /* the full-entity JSON can be large */

static bool tl_iface_is_valid(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->is_valid_id || id < 0) return false;
    return iface->vtbl->is_valid_id(iface, id) != 0;
}
static std::string tl_iface_serialize_entity(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->serialize_entity || id < 0) return std::string();
    std::string out;
    out.resize(SH_TL_JSON_CAP);
    int n = iface->vtbl->serialize_entity(iface, id, &out[0], SH_TL_JSON_CAP);
    if (n <= 0) return std::string();
    out.resize((size_t)n);
    return out;
}
static bool tl_iface_schedule_apply(sh_iface *iface, int id, const std::string &patched, const char *op)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_edit || patched.empty() || id < 0) return false;
    sh_apply_item it;
    it.kind = 0;                 /* bss-style deserialize+commit on the timeline entity id */
    it.id   = id;
    it.text = patched.c_str();
    return iface->vtbl->apply_edit(iface, &it, 1, op) != 0;
}
static void tl_iface_toast(sh_iface *iface, const char *title, const char *text)
{
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, title, text);
}
/* +0x1c0 is the player TABBED INSIDE a module (EntityMode, editor+0x23618==2)? The Create-New-Timeline gate +
 * the Qt button gray-out. A missing binding (partial backend) -> false (button stays disabled), never a crash. */
static bool tl_iface_is_entity_mode(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->is_entity_mode) return false;
    return iface->vtbl->is_entity_mode(iface) != 0;
}
/* +0x268 ATOMIC class+inherit morph (one FINAL-pair check then both fields, guard-bypassed at the invalid
 * intermediate). Returns 1 applied / 0 rejected (the FINAL pair is incompatible) / -1 slot absent. The Create-
 * New-Timeline MORPH needs THIS (not the plain bss-apply) -- the bss alone changes className but NOT the engine
 * inherit field, so the snapmaps/unknown-forced model-strip never lands + later timeline commits get REJECTED. */
static int tl_iface_apply_class_inherit(sh_iface *iface, int id, const char *cls, const char *inh)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_class_inherit) return -1;
    /* Defense-in-depth: NEVER morph a stale/deleted id -- the engine's class-derive compat check dereferences
     * the entity's className idStr, and a freed slot's dangling ptr faults (the delete-then-create crash). */
    if (!tl_iface_is_valid(iface, id)) return -1;
    return iface->vtbl->apply_class_inherit(iface, id, cls, inh);
}

/* +0x110 ENUMERATE the valid decl names of a resource class (the constrained decl-combobox). The backend
 * packs the names into out_buf as consecutive NUL-terminated strings; we split them. Returns the names (or
 * an empty list -> the caller falls back to a plain editable box, faithful to the OG cVar8=='\0' branch). */
static std::vector<std::string> tl_iface_enum_decls(sh_iface *iface, const std::string &res_class)
{
    std::vector<std::string> out;
    if (!iface || !iface->vtbl || !iface->vtbl->enum_decls_of_resclass || res_class.empty()) return out;
    static char buf[512 * 1024];
    buf[0] = '\0'; buf[1] = '\0';
    int count = 0;
    int ok = iface->vtbl->enum_decls_of_resclass(iface, res_class.c_str(), buf, (int)sizeof(buf), &count);
    if (!ok || count <= 0) return out;
    /* split the packed double-NUL-terminated block. */
    const char *p = buf;
    for (int i = 0; i < count; i++) {
        if (*p == '\0') break;
        std::string s(p);
        out.push_back(s);
        p += s.size() + 1;
    }
    return out;
}

/* ================================================================ the in-memory model ==============
 * The Timeline-Editor edits a parsed model of componentTimeLine (parsed from the live decl on open, re-
 * serialized on commit). Each arg keeps its TYPE NAME (the engine eventDef arg-type, e.g. "idSoundShader*",
 * "float", "soundChannel_t") so commit re-emits the correct keyed value -- the type-name is the arg's
 * widget key (OG FUN_180010ee0 keys item[k] by the arg widget's type-name idStr at widget+0x50). */

enum ShArgKind {
    SH_ARG_STRING = 0,   /* QLineEdit -- string/path-family/unknown */
    SH_ARG_FLOAT,        /* QLineEdit (float text) */
    SH_ARG_INT,          /* QLineEdit (int text) */
    SH_ARG_BOOL,         /* QCheckBox */
    SH_ARG_ENUM,         /* QComboBox -- value = the member NAME; emit {<enumType>:<MEMBER>} */
    SH_ARG_DECL,         /* QComboBox CONSTRAINED via +0x110 -- emit {"decl":{<declType>:<declName>}} */
    SH_ARG_ENTITY,       /* QComboBox of the map's entities (the shared entity model) -- emit {entity:<id-string>} */
    SH_ARG_MODEL_INDEX,  /* QComboBox of the entity's render-models (display=name, data=index) -- emit {int:<index>} */
    SH_ARG_ANIMWEB,      /* QComboBox of the entity's anim-web paths (display=data=path) -- emit {animWebPath:<value>} */
    SH_ARG_VECN,         /* a ROW of component fields: vec3/angles (x y z) + color (r g b a) -- emit {<type>:"a b c"} */
    SH_ARG_MD6ANIM,      /* QComboBox of the entity's md6 animations (display=basename, data=path) -- {md6Anim:<path>} */
    SH_ARG_ANIMALIAS,    /* QComboBox of the entity's md6 anim aliases (display=data=name) -- {animAlias:<name>} */
    SH_ARG_TAGNAME,      /* QComboBox of the entity's md6 attach-point tags (display=data=tag) -- {string:<tag>} */
};

struct ShArgWidget {
    std::string typeName;    /* the engine arg-type name (the item[k] key driver) */
    ShArgKind   kind;
    /* for a decl arg, declType = the inner key of {<declType>:<declName>} (e.g. "sound") captured on load. */
    std::string declType;
    QWidget    *editor;      /* the QLineEdit / QCheckBox / QComboBox built by the arg-type dispatch */
    QWidget    *label;       /* a per-arg name/type QLabel (built on the picker path; NULL on the load path) */
    std::vector<QLineEdit *> subEditors;   /* SH_ARG_VECN: the per-component fields (x/y/z or r/g/b/a) */
};

struct ShEventRow {
    QLineEdit              *eventTimeEdit;   /* the EventTime line-edit (OG eventRow+0x38) */
    QComboBox              *eventDefCombo;   /* the eventDef combobox (OG eventCall+0x58) */
    QLabel                 *descLabel;       /* author-facing "what is this event" box (signature + arg roles) */
    std::vector<ShArgWidget> args;           /* the per-arg widgets (OG the args vector at +0x30/+0x38) */
    QWidget                *rowWidget;       /* the row container (a titled QGroupBox) */
    QFormLayout            *argForm;         /* the per-arg "name: editor" rows (legible layout) */
    std::string             inheritSlug;     /* the owning tab's entity class slug (per-entity asset dropdowns) */
};

struct ShEntityTab {
    QComboBox               *entityCombo;    /* the entity the events run ON (full module id) */
    std::string              entityId;       /* the loaded entity id string (combobox currentData/text) */
    std::string              inheritSlug;    /* the entity's class slug (entityDef.inherit) -> per-entity asset lists */
    ShEventScopeProxy       *eventProxy;     /* SCOPE: the per-tab event-picker proxy (dims unfillable events) */
    QWidget                 *page;           /* the inner tab page */
    QVBoxLayout             *eventsLayout;   /* where event-rows are added */
    std::vector<ShEventRow*> events;
};

struct ShTimelineEditor {
    QTabWidget               *tl_tabs;       /* OG WIN[0x3b] -- the per-timeline QTabWidget */
    sh_iface                 *iface;         /* OG TL+0x100 -- the cached interface */
    int                       entity_id;     /* OG TL+0xf8 -- the timeline entity id (-1 = broken/new) */
    bool                      is_encounter;  /* idEncounterManager (encounterComponent) vs idTarget_Timeline */
    std::vector<ShEntityTab*> tabs;
    /* SHARED combo models (built ONCE per open, parented to tl_tabs -> auto-freed with it). The event model
     * (every event-def) is shared by every eventDef combo -- avoids the O(events x 1611) per-row fill that
     * froze the UI; the entity model (the map's entities) is shared by every entity combo + entity-typed arg. */
    QStandardItemModel       *event_model;
    QStandardItemModel       *entity_model;
    /* O(1) load-path lookups (the findText linear-scan over these big models was the open-FREEZE: 7218 ents x
     * many combos + 1611 events x many combos = millions of string compares). entity_index: id-string -> model
     * row; the event index is a process-static (the catalog is constant) -- see tl_event_index. */
    std::unordered_map<std::string, int> entity_index;
    /* dirty-tracking for the "Save Timeline" button: false right after open/commit (no unsaved changes -> button
     * grayed); a user edit sets it true (button enabled). `loading` suppresses the programmatic fills during
     * open/collect from falsely marking dirty. `win` is cached so the dirty helpers can reach the save button. */
    ShWinController          *win     = nullptr;
    bool                      dirty   = false;
    bool                      loading = false;
};

/* ---- dirty-tracking helpers for the "Save Timeline" button (gray when no unsaved changes) ---- */
static void tl_set_save_enabled(ShWinController *win, bool en)
{
    if (!win) return;
    QPushButton *btn = qobject_cast<QPushButton *>(static_cast<QWidget *>(win->ui[SH_UI_save_entity_timeline]));
    if (btn) btn->setEnabled(en);
}
static void tl_mark_dirty(ShTimelineEditor *ed)
{
    if (!ed || ed->loading || ed->dirty) return;   /* ignore the programmatic fills during open/collect */
    ed->dirty = true;
    tl_set_save_enabled(ed->win, true);
}
/* Connect every input child of `root` so a USER edit marks the timeline dirty. User-only signals (combo
 * activated / lineedit textEdited / checkbox clicked) -- NOT the programmatic setCurrentIndex/setText the load
 * path uses -- so a load never trips dirty. */
static void tl_connect_dirty(ShTimelineEditor *ed, QWidget *root)
{
    if (!ed || !root) return;
    for (QComboBox *c : root->findChildren<QComboBox *>())
        QObject::connect(c, QOverload<int>::of(&QComboBox::activated), [ed](int){ tl_mark_dirty(ed); });
    for (QLineEdit *le : root->findChildren<QLineEdit *>())
        QObject::connect(le, &QLineEdit::textEdited, [ed](const QString &){ tl_mark_dirty(ed); });
    for (QCheckBox *cb : root->findChildren<QCheckBox *>())
        QObject::connect(cb, &QCheckBox::clicked, [ed](bool){ tl_mark_dirty(ed); });
}

/* the single live Timeline-Editor (the OG holds it at WIN[0x3b]; we mirror that lifetime on win->timeline_tl). */

/* ================================================================ build helpers =====================
 * Per-arg-type widget dispatch -- the clone's FUN_18000a730: build the correct Qt editor for an arg type,
 * and for a decl-pointer type build a QComboBox CONSTRAINED to the engine's valid decl names via +0x110.
 * The arg-type-name reduction (idDecl* -> lowercased resource class) mirrors FUN_18000994c. */

/* Non-"idDecl" pointer arg-types whose decl-type short-name is NOT the mechanical strip-"id" (verified vs the
 * source-of-record entity decls): idSoundShader* is the "sound" decl-type, NOT
 * "soundshader". idActorModifier* and idMaterial* DO reduce mechanically but are listed for explicitness. The rest
 * of the catalog's id<X>* (idMD6Anim*, idEventReceiver*, …) are assets/objects, NOT decls -> no entry -> they
 * stay editable enum boxes. RE: timeline-decl-resclass-re (the OG +0x100 path; FUN_18000994c is
 * idDecl*-only and PREPENDS "idDecl"; the inner-key is the engine's decl-type dir name, not a mechanical reduction). */
static const char *tl_decl_alias(const std::string &typeName)
{
    static const struct { const char *type; const char *declType; } TL[] = {
        { "idSoundShader*",   "sound" },
        { "idActorModifier*", "actormodifier" },
        { "idMaterial*",      "material" },
    };
    for (size_t i = 0; i < sizeof(TL) / sizeof(TL[0]); i++)
        if (typeName == TL[i].type) return TL[i].declType;
    return NULL;
}

/* Reduce an arg-type-name to the decl-type SHORT-NAME = the engine's <declType> decl-type short-name (the
 * engine declManager key AND the nested-commit inner-key). idDecl<X>* -> strip "idDecl" + lowercase + drop '*'
 * ("idDeclProjectile*" -> "projectile"). A non-"idDecl" decl-pointer takes the explicit alias (tl_decl_alias).
 * NB: this is NOT the engine GetDeclsOfType resource-class (that registry is asset-types + LOGS on a decl
 * miss -- the combo feed is the non-logging declManager enumerator; see backend slot_enum_decls_of_resclass). */
static std::string tl_decl_resclass(const std::string &typeName)
{
    const char *alias = tl_decl_alias(typeName);
    if (alias) return std::string(alias);
    std::string s = typeName;
    if (s.size() > 6 && s.compare(0, 6, "idDecl") == 0) s = s.substr(6);
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '*') break;
        out.push_back((char)tolower((unsigned char)c));
    }
    return out;
}

/* classify an arg type-name into a widget kind (the FUN_18000a730 dispatch order). A type-name beginning
 * "idDecl" is a decl-pointer (constrained combobox via +0x110); the enum case is detected at commit (a
 * non-built-in, non-decl type with an enum-member value). */
static ShArgKind tl_classify_arg(const std::string &typeName)
{
    if (typeName == "float")  return SH_ARG_FLOAT;
    if (typeName == "int")    return SH_ARG_INT;
    if (typeName == "bool")   return SH_ARG_BOOL;
    if (typeName == "entity") return SH_ARG_ENTITY;   /* a map-entity dropdown (the bind master, target entities, ...) */
    if (typeName == "vec3" || typeName == "angles" || typeName == "color")
        return SH_ARG_VECN;     /* multi-component: a row of x/y/z (vec3/angles) or r/g/b/a (color) fields */
    if (typeName == "string" || typeName == "animWebPath" || typeName == "jointName" ||
        typeName == "jointTag" || typeName == "animAlias" || typeName == "md6Anim")
        return SH_ARG_STRING;   /* dedicated string-ish editors collapse to a text box (correct on-disk key) */
    if (typeName.size() > 6 && typeName.compare(0, 6, "idDecl") == 0)
        return SH_ARG_DECL;
    if (tl_decl_alias(typeName) != NULL)   /* a non-"idDecl" decl-pointer (idSoundShader* -> "sound", etc.) */
        return SH_ARG_DECL;
    /* a non-built-in, non-decl type-name is an ENUM (e.g. soundChannel_t) -> {<enumType>:<MEMBER>}. */
    return SH_ARG_ENUM;
}

/* The component KEY NAMES for a multi-component (SH_ARG_VECN) arg, by engine type. The engine reads/writes these
 * as a STRUCTURED object -- idVec3={x,y,z} / idAngles={pitch,yaw,roll} / idColor={r,g,b,a} -- NOT a space-separated
 * string. Source-of-record: idVec3/idAngles/idColor field names + the corpus color form {"r":..,"g":..,"b":..,"a":..}
 * + the engine event-arg reader (opens an object, reads named float fields; a string value is rejected). Returns
 * the key array + count via *n; defaults to vec3 (x/y/z). */
static const char *const *tl_vecn_keys(const std::string &typeName, int *n)
{
    static const char *const VEC3[] = { "x", "y", "z" };
    static const char *const ANG[]  = { "pitch", "yaw", "roll" };
    static const char *const COL[]  = { "r", "g", "b", "a" };
    if (typeName == "color")  { *n = 4; return COL; }
    if (typeName == "angles") { *n = 3; return ANG; }
    *n = 3; return VEC3;
}

/* The per-entity-class asset value list (render-models / anim-web paths) for a class slug (entityDef.inherit),
 * or NULL. Small table (only the configurable-model/anim classes) -> linear scan. */
static const ShEntityAssets *tl_lookup_entity_assets(const std::string &slug)
{
    if (slug.empty()) return nullptr;
    for (int i = 0; i < SH_ENTITY_ASSETS_N; i++)
        if (slug == SH_ENTITY_ASSETS[i].slug) return &SH_ENTITY_ASSETS[i];
    return nullptr;
}

/* Resolve an entity's class slug (entityDef.inherit) by serializing it + reading ONE JSON field -- UI-thread-safe,
 * no engine type lookup (RE: timeline-entity-class-binding-re path (a)). The serialize is the SAME +0xc8 call the
 * COLLECT path uses. Empty -> unknown class -> the asset dropdowns fall back to plain boxes (faithful). */
static std::string tl_entity_inherit_slug(ShTimelineEditor *ed, int entityId)
{
    if (!ed || entityId < 0) return std::string();
    std::string j = tl_iface_serialize_entity(ed->iface, entityId);
    if (j.empty()) return std::string();
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(j.data(), (int)j.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::string();
    return std::string(doc.object().value("entityDef").toObject()
                          .value("inherit").toString().toLocal8Bit().constData());
}

/* build the arg editor widget + return its ShArgWidget. `initValue` / `initDeclType` seed it from the
 * loaded decl (empty for a fresh insert). For a decl arg, the combobox is constrained via +0x110; if the
 * enumeration returns nothing the box stays editable (faithful OG fallback to a plain box). `argName` +
 * `inheritSlug` drive the EXCEED-the-OG per-entity asset dropdowns (modelIndex/animWebPath). */
static ShArgWidget tl_build_arg_widget(ShTimelineEditor *ed, QWidget *parent, const std::string &typeName,
                                       const std::string &initValue, const std::string &initDeclType,
                                       const std::string &argName = std::string(),
                                       const std::string &inheritSlug = std::string())
{
    ShArgWidget aw;
    aw.typeName = typeName;
    aw.kind     = tl_classify_arg(typeName);
    aw.declType = initDeclType;
    aw.editor   = nullptr;
    aw.label    = nullptr;

    /* EXCEED-the-OG per-entity asset dropdowns (the OG type-ins these). Scope the value list to the event's entity
     * class (inheritSlug -> SH_ENTITY_ASSETS). modelIndex (name-keyed; on-disk type "int") -> render-model NAMES
     * with the index in userData -> commit {int:<index>}; animWebPath -> the entity's anim-web paths -> commit
     * {animWebPath:<value>}. Editable; an empty/unknown list falls through to the normal box (faithful). */
    const ShEntityAssets *ea = tl_lookup_entity_assets(inheritSlug);
    if (ea && argName == "modelIndex" && ea->nModels > 0) {
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        for (int i = 0; i < ea->nModels; i++)
            combo->addItem(QString::fromUtf8(ea->models[i].display), QVariant(QString::fromUtf8(ea->models[i].value)));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(22);
        if (!initValue.empty()) {
            int sel = combo->findData(QVariant(QString::fromStdString(initValue)));
            if (sel >= 0) combo->setCurrentIndex(sel);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.typeName = "int";   /* the on-disk key for modelIndex */
        aw.kind     = SH_ARG_MODEL_INDEX;
        aw.editor   = combo;
        return aw;
    }
    if (ea && typeName == "animWebPath" && ea->nAnimWeb > 0) {
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        for (int i = 0; i < ea->nAnimWeb; i++)
            combo->addItem(QString::fromUtf8(ea->animWeb[i].display), QVariant(QString::fromUtf8(ea->animWeb[i].value)));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(40);
        if (!initValue.empty()) {
            int sel = combo->findData(QVariant(QString::fromStdString(initValue)));
            if (sel >= 0) combo->setCurrentIndex(sel);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.kind   = SH_ARG_ANIMWEB;
        aw.editor = combo;
        return aw;
    }
    if (ea && typeName == "md6Anim" && ea->nMd6Anim > 0) {
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        for (int i = 0; i < ea->nMd6Anim; i++)
            combo->addItem(QString::fromUtf8(ea->md6Anim[i].display), QVariant(QString::fromUtf8(ea->md6Anim[i].value)));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(36);
        if (!initValue.empty()) {
            int sel = combo->findData(QVariant(QString::fromStdString(initValue)));
            if (sel >= 0) combo->setCurrentIndex(sel);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.kind   = SH_ARG_MD6ANIM;
        aw.editor = combo;
        return aw;
    }
    if (ea && typeName == "animAlias" && ea->nAnimAlias > 0) {   /* animAlias is reduced to this key (name-keyed) */
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        for (int i = 0; i < ea->nAnimAlias; i++)
            combo->addItem(QString::fromUtf8(ea->animAlias[i].display), QVariant(QString::fromUtf8(ea->animAlias[i].value)));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(28);
        if (!initValue.empty()) {
            int sel = combo->findData(QVariant(QString::fromStdString(initValue)));
            if (sel >= 0) combo->setCurrentIndex(sel);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.kind   = SH_ARG_ANIMALIAS;
        aw.editor = combo;
        return aw;
    }
    if (ea && (argName == "tag" || argName == "tagName") && ea->nTagName > 0) {   /* name-keyed; on-disk key stays "string" */
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        for (int i = 0; i < ea->nTagName; i++)
            combo->addItem(QString::fromUtf8(ea->tagName[i].display), QVariant(QString::fromUtf8(ea->tagName[i].value)));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(24);
        if (!initValue.empty()) {
            int sel = combo->findData(QVariant(QString::fromStdString(initValue)));
            if (sel >= 0) combo->setCurrentIndex(sel);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.kind   = SH_ARG_TAGNAME;   /* typeName stays the reduced key ("string") -> commit {string:<tag>} unchanged */
        aw.editor = combo;
        return aw;
    }

    if (aw.kind == SH_ARG_BOOL) {
        QCheckBox *cb = new QCheckBox(parent);   /* the form provides the arg label */
        cb->setChecked(initValue == "true");
        aw.editor = cb;
    } else if (aw.kind == SH_ARG_ENTITY) {
        /* a map-entity dropdown, sharing the prebuilt entity model (no per-arg enumeration). Editable so a
         * not-yet-listed id can still be typed; the value round-trips as the entity id-string. */
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        if (ed && ed->entity_model) combo->setModel(ed->entity_model);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(40);   /* fixed width -> no 7218-item size-hint scan on first show */
        if (!initValue.empty()) {
            int idx = -1;
            if (ed) {
                std::unordered_map<std::string, int>::const_iterator ei = ed->entity_index.find(initValue);
                if (ei != ed->entity_index.end()) idx = ei->second;   /* O(1) (was O(7218) findText) */
            }
            if (idx >= 0) combo->setCurrentIndex(idx);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.editor = combo;
    } else if (aw.kind == SH_ARG_DECL) {
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        std::vector<std::string> names = tl_iface_enum_decls(ed ? ed->iface : nullptr, tl_decl_resclass(typeName));
        for (size_t i = 0; i < names.size(); i++)
            combo->addItem(QString::fromStdString(names[i]));
        if (!initValue.empty()) {
            int idx = combo->findText(QString::fromStdString(initValue));
            if (idx >= 0) combo->setCurrentIndex(idx);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.editor = combo;
    } else if (aw.kind == SH_ARG_ENUM) {
        /* an enum (soundChannel_t, gameTeam_t, ...) -> a combobox PRE-POPULATED with the engine enum members.
         * RE timeline-arg-dispatch-re: the OG's default branch (11) probes the enum/reflection type via the
         * +0x100 NON-LOGGING FindByName -- which the backend exposes as enum_decls_of_resclass (-> the sh_typeinfo
         * declManager enumerator). We pass the enum type-name VERBATIM (no idDecl reduction). Editable so an
         * unlisted member can still be typed; an empty enumeration degrades to a plain editable box (faithful). */
        QComboBox *combo = new QComboBox(parent);
        combo->setEditable(true);
        std::vector<std::string> members = tl_iface_enum_decls(ed ? ed->iface : nullptr, typeName);
        for (size_t i = 0; i < members.size(); i++)
            combo->addItem(QString::fromStdString(members[i]));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(24);
        if (!initValue.empty()) {
            int idx = combo->findText(QString::fromStdString(initValue));   /* small list (enum members) */
            if (idx >= 0) combo->setCurrentIndex(idx);
            else          combo->setEditText(QString::fromStdString(initValue));
        }
        aw.editor = combo;
    } else if (aw.kind == SH_ARG_VECN) {
        /* vec3/angles -> 3 fields; color -> 4 fields. Component editors (one QLineEdit per component), placeholder-
         * labelled with the engine field names (vec3=x/y/z, angles=pitch/yaw/roll, color=r/g/b/a). ON-DISK = a
         * STRUCTURED object {<type>:{<key>:float,...}} (see tl_build_arg_json / the load path below) -- NOT a
         * space-separated string; the engine arg reader opens an object and reads named float fields, and rejects
         * a string value. Internally we keep the components as a space-joined "v0 v1 v2" and convert at the JSON
         * boundary, so the field order here == the tl_vecn_keys order. */
        int n = 0;
        const char *const *ph = tl_vecn_keys(typeName, &n);
        QWidget *box = new QWidget(parent);
        QHBoxLayout *hl = new QHBoxLayout(box);
        hl->setContentsMargins(0, 0, 0, 0);
        QStringList parts = QString::fromStdString(initValue).split(' ', QString::SkipEmptyParts);
        for (int i = 0; i < n; i++) {
            QLineEdit *f = new QLineEdit(box);
            f->setPlaceholderText(QString::fromUtf8(ph[i]));
            f->setMaximumWidth(64);
            if (i < parts.size()) f->setText(parts[i]);
            hl->addWidget(f);
            aw.subEditors.push_back(f);
        }
        aw.editor = box;
    } else {
        QLineEdit *le = new QLineEdit(parent);
        le->setText(QString::fromStdString(initValue));
        aw.editor = le;
    }
    return aw;
}

/* read the current value out of an arg editor (for commit). */
static std::string tl_read_arg_value(const ShArgWidget &aw)
{
    if (!aw.editor) return std::string();
    if (aw.kind == SH_ARG_BOOL) {
        QCheckBox *cb = qobject_cast<QCheckBox *>(aw.editor);
        return (cb && cb->isChecked()) ? std::string("true") : std::string("false");
    }
    if (aw.kind == SH_ARG_VECN) {
        /* join the component fields with single spaces; an empty MIDDLE field -> "0"; trim trailing empties but
         * keep >= 3 components (vec3 / rgb). A color the user fills only r/g/b commits "r g b"; adding alpha
         * makes it "r g b a" -- matching whatever the loaded value's component count was. */
        int last = -1;
        std::vector<std::string> vals;
        for (size_t i = 0; i < aw.subEditors.size(); i++) {
            std::string t = aw.subEditors[i] ? std::string(aw.subEditors[i]->text().toLocal8Bit().constData())
                                             : std::string();
            vals.push_back(t);
            if (!t.empty()) last = (int)i;
        }
        int keep = last + 1;
        if (keep < 3) keep = 3;
        if (keep > (int)vals.size()) keep = (int)vals.size();
        std::string out;
        for (int i = 0; i < keep; i++) {
            if (i) out += " ";
            out += vals[i].empty() ? std::string("0") : vals[i];
        }
        return out;
    }
    if (aw.kind == SH_ARG_MODEL_INDEX || aw.kind == SH_ARG_ANIMWEB ||
        aw.kind == SH_ARG_MD6ANIM || aw.kind == SH_ARG_ANIMALIAS || aw.kind == SH_ARG_TAGNAME) {
        /* a PICKED item commits its userData (model index / anim path / alias / tag); TYPED text commits verbatim. */
        QComboBox *combo = qobject_cast<QComboBox *>(aw.editor);
        if (!combo) return std::string();
        int mi = combo->findText(combo->currentText());
        if (mi >= 0) {
            QVariant d = combo->itemData(mi);
            if (d.isValid()) return std::string(d.toString().toLocal8Bit().constData());
        }
        return std::string(combo->currentText().toLocal8Bit().constData());
    }
    if (aw.kind == SH_ARG_DECL || aw.kind == SH_ARG_ENUM || aw.kind == SH_ARG_ENTITY) {
        QComboBox *combo = qobject_cast<QComboBox *>(aw.editor);
        return combo ? std::string(combo->currentText().toLocal8Bit().constData()) : std::string();
    }
    QLineEdit *le = qobject_cast<QLineEdit *>(aw.editor);
    return le ? std::string(le->text().toLocal8Bit().constData()) : std::string();
}

/* ================================================================ the shared combo models ==========
 * The OG's event picker offers EVERY engine event-def (unconstrained by the entity's class), built live from
 * the engine eventMgr. We serve the SAME list from the embedded SH_EVENT_CATALOG (the source-of-record dump),
 * built ONCE into a shared model that every eventDef combo setModel()s -- model row i == catalog index i, so a
 * pick's activated(i) keys straight into SH_EVENT_CATALOG[i]. (Building it per-row was the O(events x 1611)
 * freeze.) The entity model (the map's entities) is likewise shared by every entity combo + entity-typed arg. */

static QStandardItemModel *tl_build_event_model(QObject *parent)
{
    QStandardItemModel *m = new QStandardItemModel(parent);
    QList<QStandardItem *> col;
    col.reserve(SH_EVENT_CATALOG_N);
    for (int i = 0; i < SH_EVENT_CATALOG_N; i++)
        col.append(new QStandardItem(QString::fromUtf8(SH_EVENT_CATALOG[i].name)));
    m->appendColumn(col);   /* one column, SH_EVENT_CATALOG_N rows; combo index == catalog index */
    return m;
}

static QStandardItemModel *tl_build_entity_model(ShTimelineEditor *ed, QObject *parent)
{
    QStandardItemModel *m = new QStandardItemModel(parent);
    if (ed) ed->entity_index.clear();
    if (ed && ed->iface && ed->iface->vtbl && ed->iface->vtbl->entity_count && ed->iface->vtbl->id_to_string) {
        int n = ed->iface->vtbl->entity_count(ed->iface);
        if (n < 0) n = 0;
        if (n > 100000) n = 100000;
        int row = 0;
        for (int id = 0; id < n; id++) {
            if (!tl_iface_is_valid(ed->iface, id)) continue;
            /* DEV-LAYER filter (same gate as the Entities/Timelines lists, sh_tabs.cpp): while
             * `snapEdit_enableDevLayer` is 0 the SnapMap editor hides dev-layer entities (they show as
             * "(no module)") -- mirror it here so the timeline entity picker AND every entity-typed arg
             * dropdown (both share this one model) omit them. Reveal via `snapEdit_enableDevLayer 1` then
             * reopen the timeline. Fail-safe: an absent/faulting slot -> no filtering (show). */
            if (ed->iface->vtbl->id_dev_layer_hidden &&
                ed->iface->vtbl->id_dev_layer_hidden(ed->iface, id)) continue;
            char buf[256]; buf[0] = '\0';
            const char *s = ed->iface->vtbl->id_to_string(ed->iface, id, buf, (int)sizeof buf);
            if (s && s[0]) {
                QStandardItem *it = new QStandardItem(QString::fromUtf8(s));
                it->setData(QVariant(id), Qt::UserRole);
                m->appendRow(it);
                ed->entity_index[std::string(s)] = row++;   /* id-string -> model row (O(1) seed lookup) */
            }
        }
    }
    return m;
}

/* O(1) event-def name -> catalog/model index (the catalog is constant -> a process-static map; replaces the
 * O(1611) findText per event row that, x many rows, was a big chunk of the open-FREEZE). */
static int tl_event_index(const QString &name)
{
    static std::unordered_map<std::string, int> *idx = nullptr;
    if (!idx) {
        idx = new std::unordered_map<std::string, int>();
        idx->reserve((size_t)SH_EVENT_CATALOG_N * 2);
        for (int i = 0; i < SH_EVENT_CATALOG_N; i++) (*idx)[std::string(SH_EVENT_CATALOG[i].name)] = i;
    }
    std::unordered_map<std::string, int>::const_iterator it = idx->find(std::string(name.toUtf8().constData()));
    return it != idx->end() ? it->second : -1;
}

/* Rebuild a row's arg widgets from catalog event `catIdx` (the picker action). Tears down the existing arg
 * widgets + their labels, then builds one LABELLED editor per arg via the SAME tl_build_arg_widget dispatch
 * the load path uses (built-ins -> field; idDecl* -> the +0x110 constrained combo; enum/other -> editable
 * combo). RESIDUAL: a decl-pointer arg-type that does NOT begin "idDecl" (e.g. idSoundShader*) lands as an
 * editable combo + commits the FLAT keyed form; the nested {"decl":{...}} commit for those needs the engine
 * type->declType mapping (OG FUN_18000994c). The LOAD path is unaffected -- it reads the serialized keys. */
/* Reduce a catalog RAW dumpevent arg-type to its on-disk KEY (the rawmap reader/writer key, NOT the raw
 * signature type). RE timeline-asset-enum-re (WRITER FUN_1409d2870 tag->key, cross-checked vs the READER +
 * the proven startSoundShader ground-truth): idAnimWebPath& -> animWebPath, idJointName& -> jointName,
 * idMD6Anim* -> md6Anim, idJointTag& -> jointTag. idSoundShader* or idDecl<X>* keep the raw type so
 * tl_classify_arg/tl_decl_alias route them to the decl path (commit "decl"); built-ins + <X>_t enums keep their
 * type-name. animAlias is keyed by the arg NAME (its raw dumpevent type is the generic "string"). WITHOUT this,
 * the PICKER would commit the raw signature type as the JSON key -> a corrupt timeline. (The LOAD path is
 * unaffected -- it already reads the reduced keys from the serialized JSON.) */
static std::string tl_reduce_arg_key(const std::string &typeName, const std::string &argName)
{
    if (argName == "animAlias")        return "animAlias";   /* name-keyed (raw type is "string") */
    if (typeName == "idAnimWebPath&")  return "animWebPath";
    if (typeName == "idJointName&")    return "jointName";
    if (typeName == "idJointTag&")     return "jointTag";
    if (typeName == "idMD6Anim*")      return "md6Anim";
    return typeName;
}

/* A short, FACTUAL role hint for a known arg name/type (NO fabricated behavior). Feeds the per-event description
 * box so an author knows what each parameter is for. Unknown args -> just their type is shown (still honest). */
static const char *tl_arg_role(const std::string &name, const std::string &type)
{
    if (name == "modelIndex")                          return "which render-model (index)";
    if (name == "tag" || name == "tagName")            return "skeleton attach point (joint tag)";
    if (name == "anim" || name == "animHandle")        return "an animation";
    if (name == "animAlias")                           return "a named animation";
    if (type == "animWebPath")                         return "an anim-web path";
    if (name == "shader" || type == "idSoundShader*")  return "a sound shader";
    if (type == "soundChannel_t")                      return "the audio channel";
    if (type == "entity")                              return "a target entity";
    if (type == "bool")                                return "on / off";
    if (type == "vec3")                                return "an x y z vector";
    if (type == "color")                               return "an r g b a color";
    if (type == "int" || type == "float")              return "a number";
    return NULL;
}

/* O(1) event-name -> our description record (sh_event_docs.h is sorted; we build a process-static map once). */
static const ShEvtDoc *tl_event_doc(const char *name)
{
    static std::unordered_map<std::string, const ShEvtDoc *> *idx = nullptr;
    if (!idx) {
        idx = new std::unordered_map<std::string, const ShEvtDoc *>();
        idx->reserve((size_t)SH_EVENT_DOCS_N * 2);
        for (int i = 0; i < SH_EVENT_DOCS_N; i++) (*idx)[std::string(SH_EVENT_DOCS[i].name)] = &SH_EVENT_DOCS[i];
    }
    if (!name) return nullptr;
    std::unordered_map<std::string, const ShEvtDoc *>::const_iterator it = idx->find(std::string(name));
    return it != idx->end() ? it->second : nullptr;
}

static const char *tl_arg_doc(const ShEvtDoc *doc, const char *argName)
{
    if (!doc || !argName) return nullptr;
    for (int i = 0; i < doc->nArgs; i++)
        if (doc->args[i].name && strcmp(doc->args[i].name, argName) == 0) return doc->args[i].desc;
    return nullptr;
}

/* Build the author-facing event description box (the EXPLAIN pillar). When we have a generated doc
 * (sh_event_docs.h -- our knowledge base, since the engine ships no prose) show the rich summary + a granular
 * line per parameter; a low-confidence (inferred) entry is flagged. When absent (rare) fall back to the factual
 * signature (name + category + per-arg role hints). */
static void tl_set_event_description(ShEventRow *row, int catIdx)
{
    if (!row || !row->descLabel) return;
    if (catIdx < 0 || catIdx >= SH_EVENT_CATALOG_N) { row->descLabel->clear(); return; }
    const ShEvtDef &def = SH_EVENT_CATALOG[catIdx];
    const ShEvtDoc *doc = tl_event_doc(def.name);

    QString s = QString("<b>%1</b>").arg(QString::fromUtf8(def.name));
    if (doc && doc->source && strcmp(doc->source, "handler") == 0)
        s += " <span style='color:#6aa84f'>&#10003;</span>";   /* RE-verified from the engine handler decompile */
    if (doc && doc->summary && doc->summary[0]) {
        s += QString(" &mdash; %1").arg(QString::fromUtf8(doc->summary));
        if (doc->confidence && strcmp(doc->confidence, "low") == 0)
            s += " <span style='color:#c08040'>(inferred)</span>";
        for (int j = 0; j < def.argc; j++) {
            const char *an = (def.args[j].name && def.args[j].name[0]) ? def.args[j].name : def.args[j].type;
            const char *ad = tl_arg_doc(doc, def.args[j].name);
            if (!ad) ad = tl_arg_role(def.args[j].name ? def.args[j].name : "", def.args[j].type ? def.args[j].type : "");
            s += QString("<br>&nbsp;&nbsp;<b>%1</b>%2").arg(QString::fromUtf8(an ? an : ""))
                     .arg(ad ? QString(" &mdash; %1").arg(QString::fromUtf8(ad)) : QString());
        }
    } else {
        bool isAnim = (def.name && strncmp(def.name, "ae_", 3) == 0);
        s += QString(" &mdash; %1").arg(isAnim ? "animation-frame event" : "timeline event");
        if (def.argc == 0) s += ", no parameters.";
        else {
            QStringList parts;
            for (int j = 0; j < def.argc; j++) {
                std::string an = def.args[j].name ? def.args[j].name : "";
                std::string at = def.args[j].type ? def.args[j].type : "";
                const char *role = tl_arg_role(an, at);
                QString p = QString("<b>%1</b>").arg(QString::fromUtf8(an.empty() ? at.c_str() : an.c_str()));
                if (role) p += QString(" = %1").arg(QString::fromUtf8(role));
                parts << p;
            }
            s += "&nbsp;&middot;&nbsp; " + parts.join(",&nbsp; ");
        }
    }
    row->descLabel->setText(s);
}

/* the arg form label: "<name>  (<type>)" so the author sees both the parameter name and its engine type. */
static QString tl_arg_label(const char *name, const char *type)
{
    if (name && name[0])
        return QString("%1  (%2)").arg(QString::fromUtf8(name)).arg(QString::fromUtf8(type ? type : ""));
    return QString::fromUtf8(type ? type : "");
}

static void tl_rebuild_args_from_catalog(ShTimelineEditor *ed, ShEventRow *row, int catIdx)
{
    if (!ed || !row || !row->argForm || catIdx < 0 || catIdx >= SH_EVENT_CATALOG_N) return;
    while (row->argForm->rowCount() > 0) row->argForm->removeRow(0);   /* deletes the prior label+editor pairs */
    row->args.clear();
    tl_set_event_description(row, catIdx);   /* the "what is this event" box (EXPLAIN pillar v1) */

    const ShEvtDef &def = SH_EVENT_CATALOG[catIdx];
    for (int j = 0; j < def.argc; j++) {
        const char *an = def.args[j].name;
        const char *at = def.args[j].type;
        std::string key = tl_reduce_arg_key(at, an ? an : "");   /* RAW dumpevent type -> the on-disk KEY (correctness) */
        ShArgWidget aw = tl_build_arg_widget(ed, row->rowWidget, key, "", "", an ? an : "", row->inheritSlug);
        if (aw.kind == SH_ARG_DECL) {                 /* idDecl* -> the nested {"decl":{<declType>:..}} commit */
            aw.declType = tl_decl_resclass(at);
            aw.typeName = "decl";
        }
        row->args.push_back(aw);
        if (aw.editor) row->argForm->addRow(tl_arg_label(an, at), aw.editor);
    }
}

/* ================================================================ COLLECT-from-decl (open) =========
 * Parse the live timeline entity's componentTimeLine (from the serialized JSON) into the UI model + build
 * the rows. The serialize emits entityDef.state.edit.componentTimeLine as the schema object (or
 * encounterComponent for an idEncounterManager). The args carry their type-name as the item[k] key; a decl
 * arg's value is the nested {<declType>:<declName>} object. Mirrors FUN_1800120a4 (populate). */

/* engine list object -> ordered items: walk item[0..num-1] (the engine list shape {item[i]..,num}). */
static QJsonArray tl_list_items(const QJsonObject &listObj)
{
    QJsonArray out;
    int num = listObj.value("num").toInt(0);
    if (num <= 0) num = listObj.value("\nnum").toInt(0);
    for (int i = 0; i < num; i++) {
        QString k = QString("item[%1]").arg(i);
        if (listObj.contains(k)) out.append(listObj.value(k));
    }
    return out;
}

/* the eventDef key tries the three OG spellings (FUN_180010b68): "eventDef" / "eventHandle_t eventDef" /
 * "\neventHandle_t eventDef". */
static QString tl_event_def(const QJsonObject &eventCall)
{
    static const char *keys[] = { "eventDef", "eventHandle_t eventDef", "\neventHandle_t eventDef" };
    for (int i = 0; i < 3; i++)
        if (eventCall.contains(keys[i])) return eventCall.value(keys[i]).toString();
    return QString();
}

static void tl_build_event_row(ShTimelineEditor *ed, ShEntityTab *tab, const QJsonObject &eventObj);

static ShEventRow *tl_make_event_row(ShTimelineEditor *ed, QWidget *parent)
{
    ShEventRow *row = new ShEventRow();
    QGroupBox *box = new QGroupBox(parent);
    row->rowWidget = box;
    QVBoxLayout *vl = new QVBoxLayout(box);

    /* header: [Event: <combo, stretches>]   [Time: <edit>] */
    QHBoxLayout *hdr = new QHBoxLayout();
    hdr->addWidget(new QLabel(QApplication::translate("QtWidgetsApplication1Class", "Event:"), box));
    row->eventDefCombo = new QComboBox(box);
    row->eventDefCombo->setEditable(true);
    if (ed->event_model) row->eventDefCombo->setModel(ed->event_model);   /* SHARED model -- no per-row fill */
    row->eventDefCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->eventDefCombo->setMinimumContentsLength(28);   /* fixed width -> no O(items) size-hint scan on first show */
    if (row->eventDefCombo->completer()) {              /* FLOW: type to filter the 1611 events by substring */
        row->eventDefCombo->completer()->setCompletionMode(QCompleter::PopupCompletion);
        row->eventDefCombo->completer()->setFilterMode(Qt::MatchContains);
    }
    hdr->addWidget(row->eventDefCombo, 1);
    hdr->addWidget(new QLabel(QApplication::translate("QtWidgetsApplication1Class", "Time:"), box));
    row->eventTimeEdit = new QLineEdit(box);
    row->eventTimeEdit->setMaximumWidth(80);
    hdr->addWidget(row->eventTimeEdit);
    vl->addLayout(hdr);

    /* the author-facing description box -- on event-select it shows the event's parameter signature + factual
     * role hints (the engine ships no prose for the 1611 events, so v1 states what we KNOW; see
     * tl_set_event_description). This is the first surfacing of the "EXPLAIN" pillar of the authoring framework. */
    row->descLabel = new QLabel(box);
    row->descLabel->setWordWrap(true);
    row->descLabel->setTextFormat(Qt::RichText);
    row->descLabel->setStyleSheet("color:#9aa0a6; padding:2px 0;");
    vl->addWidget(row->descLabel);

    /* the per-arg "name: editor" form -- one labelled row per arg (legible, not all crammed in one line) */
    row->argForm = new QFormLayout();
    vl->addLayout(row->argForm);

    /* a user PICK rebuilds this row's args from the catalog (model row i == catalog index i). A programmatic
     * setCurrentIndex (the load path) does NOT emit activated, so decl-loaded args are preserved. */
    QObject::connect(row->eventDefCombo,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
        [ed, row](int i) { tl_rebuild_args_from_catalog(ed, row, i); });
    return row;
}

static void tl_build_event_row(ShTimelineEditor *ed, ShEntityTab *tab, const QJsonObject &eventObj)
{
    ShEventRow *row = tl_make_event_row(ed, tab->page);
    row->inheritSlug = tab->inheritSlug;   /* the entity class -> the per-entity asset dropdowns (modelIndex/animWebPath) */
    if (tab->eventProxy) row->eventDefCombo->setModel(tab->eventProxy);   /* SCOPE: the dimmed (scoped) event picker */

    /* eventTime: emitted only if the line-edit parses; seed from the loaded int. */
    if (eventObj.contains("eventTime"))
        row->eventTimeEdit->setText(QString::number(eventObj.value("eventTime").toInt(0)));

    QJsonObject eventCall = eventObj.value("eventCall").toObject();
    QString evName = tl_event_def(eventCall);
    if (!evName.isEmpty()) {
        int idx = tl_event_index(evName);   /* O(1) map (was findText -> O(1611) scan per event row) */
        if (idx >= 0) row->eventDefCombo->setCurrentIndex(idx);  /* programmatic -> no activated -> loaded args kept */
        else          row->eventDefCombo->setEditText(evName);
    }

    /* args: each item[k] = { "<argTypeName>": value }. The arg NAME (needed for the modelIndex dropdown -- its
     * on-disk key is the generic "int", indistinguishable from a plain int without the name) comes from the
     * event catalog by position. */
    QJsonObject argsObj = eventCall.value("args").toObject();
    QJsonArray items = tl_list_items(argsObj);
    int catIdx = tl_event_index(evName);
    tl_set_event_description(row, catIdx);   /* the "what is this event" box, for loaded events too */
    for (int k = 0; k < items.size(); k++) {
        QJsonObject argObj = items[k].toObject();
        std::string argName;
        if (catIdx >= 0 && k < SH_EVENT_CATALOG[catIdx].argc && SH_EVENT_CATALOG[catIdx].args[k].name)
            argName = SH_EVENT_CATALOG[catIdx].args[k].name;
        /* the single key = the arg type-name; the value = its (possibly nested) value. */
        for (QJsonObject::const_iterator it = argObj.begin(); it != argObj.end(); ++it) {
            std::string typeName = std::string(it.key().toLocal8Bit().constData());
            std::string initVal, initDeclType, lbl;
            QJsonValue v = it.value();
            ShArgWidget aw;
            if (typeName == "decl" && v.isObject()) {
                /* the NESTED {<declType>:<declName>} form -> capture both; the WIDGET type-name is the inner
                 * declType promoted to an idDecl* class so the +0x110 combobox constrains to that class. */
                QJsonObject inner = v.toObject();
                QJsonObject::const_iterator di = inner.begin();
                if (di != inner.end()) {
                    initDeclType = std::string(di.key().toLocal8Bit().constData());
                    initVal      = std::string(di.value().toString().toLocal8Bit().constData());
                }
                /* synthesize the idDecl* type-name from the declType so tl_classify_arg -> SH_ARG_DECL and
                 * tl_decl_resclass reduces back to the declType (e.g. "sound" -> "idDeclsound*" -> "sound").
                 * The OUTER emit key stays "decl" (commit re-wraps); declType is kept verbatim. */
                std::string synth = "idDecl" + initDeclType + "*";
                aw = tl_build_arg_widget(ed, row->rowWidget, synth, initVal, initDeclType, argName, tab->inheritSlug);
                aw.typeName = "decl";          /* the OUTER arg key */
                lbl = initDeclType.empty() ? std::string("decl") : initDeclType;
            } else {
                if (v.isString())      initVal = std::string(v.toString().toLocal8Bit().constData());
                else if (v.isBool())   initVal = v.toBool() ? "true" : "false";
                else if (v.isDouble()) {
                    double d = v.toDouble();
                    char b[64];
                    if (d == (double)(long long)d) _snprintf_s(b, sizeof b, _TRUNCATE, "%lld", (long long)d);
                    else                           _snprintf_s(b, sizeof b, _TRUNCATE, "%g", d);
                    initVal = b;
                }
                else if (v.isObject()) {
                    /* a STRUCTURED vec3/angles/color arg {<key>:float,...} -> join into the internal space-separated
                     * "v0 v1 v2" the SH_ARG_VECN widget splits into its component fields (symmetric with the
                     * structured commit in tl_build_arg_json). Reads the components in tl_vecn_keys order. */
                    QJsonObject vo = v.toObject();
                    int n = 0;
                    const char *const *keys = tl_vecn_keys(typeName, &n);
                    QStringList parts;
                    for (int i = 0; i < n; i++) {
                        double comp = vo.value(QString::fromUtf8(keys[i])).toDouble(0.0);
                        char b[64];
                        if (comp == (double)(long long)comp) _snprintf_s(b, sizeof b, _TRUNCATE, "%lld", (long long)comp);
                        else                                 _snprintf_s(b, sizeof b, _TRUNCATE, "%g", comp);
                        parts << QString::fromUtf8(b);
                    }
                    initVal = std::string(parts.join(' ').toLocal8Bit().constData());
                }
                aw = tl_build_arg_widget(ed, row->rowWidget, typeName, initVal, "", argName, tab->inheritSlug);
                lbl = typeName;
            }
            row->args.push_back(aw);
            if (aw.editor && row->argForm) row->argForm->addRow(QString::fromStdString(lbl), aw.editor);
            break;   /* one key per arg item */
        }
    }

    tab->eventsLayout->addWidget(row->rowWidget);
    tab->events.push_back(row);
}

/* SCOPE: is this catalog event "fillable" for an entity class (slug)? Path A (timeline-entity-event-coupling-re):
 * every ASSET arg must have a non-empty list on this entity (md6Anim/animWebPath/animAlias/modelIndex/tagName);
 * entity-independent args (int/float/string/enum/decl/entity/jointName/...) always pass. Drives the DIM (advisory,
 * non-blocking) of events whose asset args the entity can't provide -- e.g. ae_turretFire on a keycard. */
static bool tl_event_resolves(int catIdx, const std::string &slug)
{
    if (catIdx < 0 || catIdx >= SH_EVENT_CATALOG_N) return true;
    const ShEntityAssets *ea = tl_lookup_entity_assets(slug);
    const ShEvtDef &def = SH_EVENT_CATALOG[catIdx];
    for (int j = 0; j < def.argc; j++) {
        std::string an = def.args[j].name ? def.args[j].name : "";
        std::string key = tl_reduce_arg_key(def.args[j].type ? def.args[j].type : "", an);
        if      (key == "md6Anim")     { if (!ea || ea->nMd6Anim   == 0) return false; }
        else if (key == "animWebPath") { if (!ea || ea->nAnimWeb   == 0) return false; }
        else if (an == "animAlias")    { if (!ea || ea->nAnimAlias == 0) return false; }
        else if (an == "modelIndex")   { if (!ea || ea->nModels    == 0) return false; }
        else if (an == "tag" || an == "tagName") { if (!ea || ea->nTagName == 0) return false; }
    }
    return true;
}

/* rebuild the tab's event-picker dim-set for its current entity class. */
static void tl_tab_rescope(ShEntityTab *tab)
{
    if (!tab || !tab->eventProxy) return;
    std::vector<char> valid((size_t)SH_EVENT_CATALOG_N, (char)1);
    for (int i = 0; i < SH_EVENT_CATALOG_N; i++)
        valid[(size_t)i] = tl_event_resolves(i, tab->inheritSlug) ? (char)1 : (char)0;
    tab->eventProxy->setValid(valid);
}

static ShEntityTab *tl_build_entity_tab(ShTimelineEditor *ed, const QJsonObject &entityEventObj)
{
    ShEntityTab *tab = new ShEntityTab();
    tab->page = new QWidget();
    QVBoxLayout *pageLayout = new QVBoxLayout(tab->page);

    /* the entity combobox (the entity the events run ON) -- shares the prebuilt entity model; editable so an
     * unlisted id can still be typed. */
    tab->entityCombo = new QComboBox(tab->page);
    tab->entityCombo->setEditable(true);
    if (ed->entity_model) tab->entityCombo->setModel(ed->entity_model);
    tab->entityCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    tab->entityCombo->setMinimumContentsLength(40);   /* fixed width -> no 7218-item size-hint scan on first tab-show */
    tab->entityId = std::string(entityEventObj.value("entity").toString().toLocal8Bit().constData());
    if (!tab->entityId.empty()) {
        std::unordered_map<std::string, int>::const_iterator ei = ed->entity_index.find(tab->entityId);
        if (ei != ed->entity_index.end()) tab->entityCombo->setCurrentIndex(ei->second);  /* O(1) (was O(7218) scan) */
        else                              tab->entityCombo->setEditText(QString::fromStdString(tab->entityId));
    }
    /* resolve THIS tab entity's class slug (entityDef.inherit) -> the per-entity asset dropdowns. The int id is
     * the entity combo's UserRole (tl_build_entity_model seeds it). One serialize+JSON read, UI-thread-safe. */
    if (tab->entityCombo->currentData(Qt::UserRole).isValid())
        tab->inheritSlug = tl_entity_inherit_slug(ed, tab->entityCombo->currentData(Qt::UserRole).toInt());
    /* re-resolve if the user re-targets the tab to a different entity, so a subsequent Add-Eventcall scopes right. */
    QObject::connect(tab->entityCombo,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        [ed, tab](int) {
            tab->inheritSlug = tab->entityCombo->currentData(Qt::UserRole).isValid()
                ? tl_entity_inherit_slug(ed, tab->entityCombo->currentData(Qt::UserRole).toInt())
                : std::string();
            tl_tab_rescope(tab);   /* SCOPE: re-dim the event picker for the re-targeted entity */
        });
    /* SCOPE: this tab's event-picker proxy over the shared event model -- dims events the entity can't fill. */
    tab->eventProxy = new ShEventScopeProxy(ed->tl_tabs);
    if (ed->event_model) tab->eventProxy->setSourceModel(ed->event_model);
    tl_tab_rescope(tab);
    pageLayout->addWidget(tab->entityCombo);

    /* the per-tab "Add Eventcall" button (OG FUN_1800114fc builds this inside each entity page; clicking it N
     * times = N events on this one entity -- the OG's N-events-per-item mechanism). A runtime child, no
     * objectName needed (faithful). Capturing `tab` by value is safe: tabs are torn down only in tl_destroy. */
    QPushButton *addEvt = new QPushButton(QApplication::translate("QtWidgetsApplication1Class", "Add Eventcall"),
                                          tab->page);
    addEvt->setMaximumWidth(120);
    pageLayout->addWidget(addEvt);
    QObject::connect(addEvt, &QPushButton::clicked, [ed, tab]() {
        tl_build_event_row(ed, tab, QJsonObject());
        tl_mark_dirty(ed); tl_connect_dirty(ed, tab->page);   /* adding an eventcall is an unsaved change */
    });

    /* the events area. */
    QWidget *eventsHost = new QWidget(tab->page);
    tab->eventsLayout = new QVBoxLayout(eventsHost);
    pageLayout->addWidget(eventsHost);

    QScrollArea *scroll = new QScrollArea();
    scroll->setWidget(tab->page);
    scroll->setWidgetResizable(true);

    /* the events list. */
    QJsonObject eventsObj = entityEventObj.value("events").toObject();
    QJsonArray eventItems = tl_list_items(eventsObj);
    for (int j = 0; j < eventItems.size(); j++)
        tl_build_event_row(ed, tab, eventItems[j].toObject());

    ed->tl_tabs->addTab(scroll, QString("item[%1]").arg((int)ed->tabs.size()));
    ed->tabs.push_back(tab);
    return tab;
}

/* ================================================================ the COLLECT-from-decl entry ====== */
static void tl_collect_from_decl(ShTimelineEditor *ed)
{
    if (ed->entity_id < 0) return;   /* broken/new timeline -- nothing to load (faithful) */
    std::string full = tl_iface_serialize_entity(ed->iface, ed->entity_id);
    if (full.empty()) return;

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(full.data(), (int)full.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
    QJsonObject edit = doc.object().value("entityDef").toObject()
                          .value("state").toObject()
                          .value("edit").toObject();

    /* componentTimeLine (idTarget_Timeline) or encounterComponent (idEncounterManager). */
    QJsonObject comp;
    if (edit.contains("componentTimeLine")) { comp = edit.value("componentTimeLine").toObject(); ed->is_encounter = false; }
    else if (edit.contains("encounterComponent")) { comp = edit.value("encounterComponent").toObject(); ed->is_encounter = true; }
    else return;   /* no timeline data on this entity */

    QJsonObject entityEvents = comp.value("entityEvents").toObject();
    QJsonArray items = tl_list_items(entityEvents);
    for (int i = 0; i < items.size(); i++)
        tl_build_entity_tab(ed, items[i].toObject());
}

/* ================================================================ COMMIT (UI -> nested decl) =======
 * Rebuild componentTimeLine from the UI model (the GOOD nested {<declType>:<declName>} decl form), patch it
 * into a fresh serialize of the timeline entity, and SCHEDULE the apply via +0xd0 (the C2 clone_bss_apply
 * path). Mirrors FUN_180012458 -> FUN_1800122a4 -> FUN_180011a88 -> FUN_180010ee0 (collect), but emits JSON
 * (the clone's apply path is the +0xc8/+0xd0 serialize/schedule, not the OG idDict tree). */

static QJsonObject tl_build_arg_json(const ShArgWidget &aw)
{
    QJsonObject argObj;
    std::string val = tl_read_arg_value(aw);
    if (aw.kind == SH_ARG_DECL) {
        /* the NESTED reader/LOAD form: {"decl":{<declType>:<declName>}} (R2 -- the OUTER key is "decl"). */
        QJsonObject inner;
        inner.insert(QString::fromStdString(aw.declType.empty() ? "decl" : aw.declType),
                     QString::fromStdString(val));
        argObj.insert("decl", inner);
    } else if (aw.kind == SH_ARG_BOOL) {
        argObj.insert(QString::fromStdString(aw.typeName), QJsonValue(val == "true"));
    } else if (aw.kind == SH_ARG_INT || aw.kind == SH_ARG_MODEL_INDEX) {   /* modelIndex commits {int:<index>} */
        argObj.insert(QString::fromStdString(aw.typeName), QJsonValue(atoi(val.c_str())));
    } else if (aw.kind == SH_ARG_FLOAT) {
        argObj.insert(QString::fromStdString(aw.typeName), QJsonValue(atof(val.c_str())));
    } else if (aw.kind == SH_ARG_VECN) {
        /* STRUCTURED object: {<type>:{<key>:float,...}} -- idVec3={x,y,z}/idAngles={pitch,yaw,roll}/idColor={r,g,b,a}.
         * The engine event-arg reader opens an object and reads named float fields; a space-separated STRING is NOT
         * accepted (the old string form threw "SetEntityEditState: failed to parse typeinfo state" at map load).
         * Emit ALL components explicitly (the reader leaves an absent field uninitialized). `val` is the internal
         * space-joined "v0 v1 v2" from the component editors (tl_read_arg_value). */
        int n = 0;
        const char *const *keys = tl_vecn_keys(aw.typeName, &n);
        QStringList parts = QString::fromStdString(val).split(' ', QString::SkipEmptyParts);
        QJsonObject vecObj;
        for (int i = 0; i < n; i++) {
            double comp = (i < parts.size()) ? parts[i].toDouble() : 0.0;
            vecObj.insert(QString::fromUtf8(keys[i]), QJsonValue(comp));
        }
        argObj.insert(QString::fromStdString(aw.typeName), vecObj);
    } else {
        /* string / enum / path-family -> the member/value as a string keyed by the type-name. */
        argObj.insert(QString::fromStdString(aw.typeName), QString::fromStdString(val));
    }
    return argObj;
}

static QJsonObject tl_build_event_json(ShEventRow *row)
{
    QJsonObject ev;
    QJsonObject eventCall;
    eventCall.insert("\neventHandle_t eventDef",
                     QString::fromLocal8Bit(row->eventDefCombo->currentText().toLocal8Bit()));

    /* args: { "\nnum": K, item[k]: {<typeName>: value} } (the engine list shape). */
    QJsonObject argsObj;
    int k = 0;
    for (size_t a = 0; a < row->args.size(); a++) {
        argsObj.insert(QString("item[%1]").arg(k), tl_build_arg_json(row->args[a]));
        k++;
    }
    argsObj.insert("\nnum", k);
    eventCall.insert("args", argsObj);
    ev.insert("eventCall", eventCall);

    /* eventTime: emitted ONLY if the line-edit parses as a uint (OG FUN_180011a88 toUInt gate). */
    QByteArray etb = row->eventTimeEdit->text().toLocal8Bit();
    if (etb.size() > 0) {
        bool ok = false;
        QString ets = QString::fromLocal8Bit(etb);
        uint v = ets.toUInt(&ok, 10);
        if (ok) ev.insert("eventTime", (int)v);
    }
    return ev;
}

/* Build the RESOLVABLE engine entity NAME for the tab's target entity. The engine resolves a timeline event's
 * `entity` (idManagedClassPtr<idEntity>) by an EXACT-STRING name intern (resolver 0x1a25770 / table 0x143082b60;
 * writer 0x9d2870 case 'e' -> live name @entity+0x388, non-live -> GetUnresolvedEntityName 0x9c78d0). NOT the UI
 * display label: id_to_string appends " (no module)" for a global/no-module entity -- a clone-UI-only string that
 * does NOT exist in the engine binary, so it can NEVER match a live entity -> a fresh dangling index -> a silent
 * downstream deref crash (the map "loads" then crashes at play/resolve; RE timeline-entityref-serialize-re). We
 * RE-READ id_to_string LIVE from the combo's entity index (so a re-placed entity's CURRENT module is used; once
 * this ref RESOLVES at commit the engine holds a live managed ptr that then tracks further module moves itself),
 * then STRIP the " (no module)" suffix -> the module-qualified "<modIdx>_<modName>/<inherit>_<uid>" (proven
 * resolvable) or, for a still-global entity, the bare "<inherit>_<uid>" tail. A typed (not picked) entry commits
 * its text verbatim (also stripped). */
static std::string tl_resolve_entity_ref(ShTimelineEditor *ed, ShEntityTab *tab)
{
    std::string ref;
    QVariant d = tab->entityCombo->currentData(Qt::UserRole);   /* the picked entity's engine index */
    bool okIdx = false;
    int idx = d.isValid() ? d.toInt(&okIdx) : 0;
    if (okIdx && idx >= 0 && ed && ed->iface && ed->iface->vtbl && ed->iface->vtbl->id_to_string) {
        char buf[256]; buf[0] = '\0';
        const char *s = ed->iface->vtbl->id_to_string(ed->iface, idx, buf, (int)sizeof buf);
        if (s && s[0]) ref = s;                                  /* LIVE ref (current module) */
    }
    if (ref.empty())                                            /* typed text (no picked entity) -> verbatim */
        ref = std::string(tab->entityCombo->currentText().toLocal8Bit().constData());
    static const std::string NOMOD = " (no module)";
    if (ref.size() >= NOMOD.size() && ref.compare(ref.size() - NOMOD.size(), NOMOD.size(), NOMOD) == 0)
        ref.erase(ref.size() - NOMOD.size());                   /* the display suffix never belongs in the ref */
    return ref;
}

static QJsonObject tl_build_entity_event_json(ShTimelineEditor *ed, ShEntityTab *tab)
{
    QJsonObject ee;
    ee.insert("entity", QString::fromStdString(tl_resolve_entity_ref(ed, tab)));

    QJsonObject eventsObj;
    int j = 0;
    for (size_t e = 0; e < tab->events.size(); e++) {
        eventsObj.insert(QString("item[%1]").arg(j), tl_build_event_json(tab->events[e]));
        j++;
    }
    eventsObj.insert("num", j);
    ee.insert("events", eventsObj);
    return ee;
}

/* build the full componentTimeLine object from the UI model. */
static QJsonObject tl_build_component_json(ShTimelineEditor *ed)
{
    QJsonObject entityEvents;
    int i = 0;
    for (size_t t = 0; t < ed->tabs.size(); t++) {
        entityEvents.insert(QString("item[%1]").arg(i), tl_build_entity_event_json(ed, ed->tabs[t]));
        i++;
    }
    entityEvents.insert("num", i);

    QJsonObject comp;
    comp.insert("entityEvents", entityEvents);
    /* NO top-level "num": the engine serializes componentTimeLine as {entityEvents:{...,num:N}} -- the item
     * count lives INSIDE entityEvents ONLY. A top-level num is spurious: the BIND-ALL-DOORS save round-trip
     * showed it made our output diverge from the engine baseline by +11B (componentTimeLine keys [entityEvents]
     * vs our [entityEvents,num]). componentTimeLine is an object, not a list, so it carries no list-count. */
    return comp;
}

/* patch entityDef.state.edit.<compKey> = comp into the full serialized entity JSON; returns the compact
 * patched JSON (or "" on a shape failure). Preserves every existing ~type tag (QJson keeps unknown keys). */
static std::string tl_patch_component(const std::string &full_json, const char *compKey,
                                      const QJsonObject &comp)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(full_json.data(), (int)full_json.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::string();
    QJsonObject root = doc.object();
    if (!root.contains("entityDef") || !root.value("entityDef").isObject()) return std::string();
    QJsonObject ed   = root.value("entityDef").toObject();
    QJsonObject st   = ed.value("state").isObject() ? ed.value("state").toObject() : QJsonObject();
    QJsonObject edit = st.value("edit").isObject() ? st.value("edit").toObject() : QJsonObject();
    edit.insert(compKey, comp);
    st.insert("edit", edit);
    ed.insert("state", st);
    root.insert("entityDef", ed);
    QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return std::string(out.constData(), (size_t)out.size());
}

/* ================================================================ the public entry points ========== */

static ShTimelineEditor *tl_get(ShWinController *win)
{
    return win ? static_cast<ShTimelineEditor *>(win->timeline_tl) : nullptr;
}

/* tear down an existing editor (the OG FUN_180012018 removeTab + delete the host widgets). */
static void tl_destroy(ShWinController *win)
{
    ShTimelineEditor *ed = tl_get(win);
    if (!ed) return;
    /* host the QTabWidget inside the Timeline-Editor tab's groupbox layout; deleting it drops the children. */
    if (ed->tl_tabs) ed->tl_tabs->deleteLater();
    for (size_t t = 0; t < ed->tabs.size(); t++) {
        for (size_t e = 0; e < ed->tabs[t]->events.size(); e++) delete ed->tabs[t]->events[e];
        delete ed->tabs[t];
    }
    delete ed;
    win->timeline_tl = nullptr;
}

/* build the per-timeline QTabWidget into the Timeline-Editor tab's "Current Timeline" groupbox
 * (verticalLayout_3, SH_UI_verticalLayout_3) -- the OG hosts TL there. */
static QTabWidget *tl_build_qtabwidget(ShWinController *win)
{
    QTabWidget *tabs = new QTabWidget();
    tabs->setObjectName(QStringLiteral("timeline_editor_tabwidget"));
    QWidget *gb = static_cast<QWidget *>(win->ui[SH_UI_timeline_groupbox]);
    if (gb && gb->layout())
        gb->layout()->addWidget(tabs);
    return tabs;
}

void sh_timeline_open(ShWinController *win, int id)
{
    if (!win) return;
    tl_destroy(win);   /* rebuild from scratch (the OG FUN_1800102a0 re-builds the TL) */

    ShTimelineEditor *ed = new ShTimelineEditor();
    ed->iface        = win->iface;
    ed->win          = win;       /* for the dirty helpers' Save-button reach */
    ed->loading      = true;      /* suppress dirty during the programmatic open/collect fills */
    ed->is_encounter = false;
    ed->tl_tabs      = tl_build_qtabwidget(win);
    /* OG TL+0xf8 = the timeline entity id; -1 if the id is invalid/unresolved (the broken Create-New shape). */
    ed->entity_id    = tl_iface_is_valid(win->iface, id) ? id : -1;
    /* QOL: title the groupbox "Current Timeline - <name>" (the timeline entity's displayName). */
    {
        QGroupBox *gb = qobject_cast<QGroupBox *>(static_cast<QWidget *>(win->ui[SH_UI_timeline_groupbox]));
        std::string nm;
        if (ed->entity_id >= 0 && win->iface && win->iface->vtbl && win->iface->vtbl->get_displayname) {
            char dn[128] = {0};
            win->iface->vtbl->get_displayname(win->iface, ed->entity_id, dn, (int)sizeof(dn));
            nm = dn;
        }
        if (nm.empty()) {
            if (ed->entity_id < 0) nm = "(new)";
            else { char b[32]; _snprintf_s(b, sizeof b, _TRUNCATE, "id %d", ed->entity_id); nm = b; }
        }
        if (gb) gb->setTitle(QString::fromStdString("Current Timeline - " + nm));
    }
    /* build the SHARED combo models ONCE (parented to tl_tabs -> auto-freed with it in tl_destroy). The event
     * model is the freeze fix (built once, shared by every event combo); the entity model backs the entity
     * combo + every entity-typed arg dropdown. */
    /* [DIAGNOSTIC] time each open phase -> a toast (debugger-readable) to root-cause the busy-timeline freeze. */
    QElapsedTimer __tt; __tt.start();
    int __nent = (ed->iface && ed->iface->vtbl && ed->iface->vtbl->entity_count)
                 ? ed->iface->vtbl->entity_count(ed->iface) : -1;
    ed->event_model  = tl_build_event_model(ed->tl_tabs);       qint64 __t_evm  = __tt.restart();
    ed->entity_model = tl_build_entity_model(ed, ed->tl_tabs);  qint64 __t_entm = __tt.restart();
    win->timeline_tl = ed;

    /* switch to the Timeline-Editor tab (tab 5, objectName tab_8). */
    QTabWidget *mainTabs = static_cast<QTabWidget *>(win->ui[SH_UI_tabWidget]);
    QWidget *teTab = static_cast<QWidget *>(win->ui[SH_UI_tab_8]);
    if (mainTabs && teTab) mainTabs->setCurrentIndex(mainTabs->indexOf(teTab));

    tl_collect_from_decl(ed);                                   qint64 __t_coll = __tt.restart();
    /* QOL dirty-tracking: the editor is freshly loaded -> no unsaved changes. Connect the user-edit signals,
     * then gray the Save Timeline button until the user actually changes something. */
    ed->loading = false;
    ed->dirty   = false;
    tl_connect_dirty(ed, ed->tl_tabs);
    tl_set_save_enabled(win, false);
    {
        char __tb[256];
        _snprintf_s(__tb, sizeof __tb, _TRUNCATE,
            "event_model=%lldms entity_model=%lldms(%d ents) collect=%lldms (%d tabs)",
            (long long)__t_evm, (long long)__t_entm, __nent, (long long)__t_coll, (int)ed->tabs.size());
        tl_iface_toast(ed->iface, "TL-open-timing", __tb);
    }
}

void sh_timeline_open_pending(ShWinController *win)
{
    if (!win || win->pending_select_id < 0) return;
    int id = win->pending_select_id;
    win->pending_select_id = -1;
    sh_timeline_open(win, id);
}

void sh_timeline_insert_event(ShWinController *win)
{
    /* OG "Insert Entity Event" (top-level, FUN_180011e9c) = ADD A NEW ENTITY TAB (a new entityEvents item / a
     * new target entity). Adding EVENTS to an existing entity is the per-tab "Add Eventcall" button built in
     * tl_build_entity_tab. (Was: append an event row to the current tab -- that conflated the OG's two levels.
     * RE: timeline-multievent-re.) */
    ShTimelineEditor *ed = tl_get(win);
    if (!ed) return;
    ShEntityTab *tab = tl_build_entity_tab(ed, QJsonObject());   /* empty -> entity combo + "Add Eventcall" + empty area */
    if (tab && ed->tl_tabs) ed->tl_tabs->setCurrentIndex((int)ed->tabs.size() - 1);   /* focus the new tab */
    tl_mark_dirty(ed);                                           /* inserting an entity event is an unsaved change */
    if (tab) tl_connect_dirty(ed, tab->page);
}

/* ================================================================ Create-New-Timeline (EXCEEDS OG) ===
 * The empty componentTimeLine a fresh timeline host carries (entityEvents with a zero count). The on-disk
 * count lives in entityEvents (NO top-level num -- see the header THE ON-DISK SHAPE). */
static QJsonObject tl_empty_component()
{
    QJsonObject entityEvents; entityEvents.insert("num", 0);
    QJsonObject comp;         comp.insert("entityEvents", entityEvents);
    return comp;
}

/* MORPH an existing entity into a timeline host: take its serialized JSON and set, in state.edit,
 * className="idTarget_Timeline", inherit="snapmaps/unknown", an empty componentTimeLine, and STRIP the render
 * model (renderModelInfo.model="") so it draws as the generic unknown (FACET 4 -- snapmaps/unknown imposes no
 * model; create-timeline-re). The exact morph the OG commit-builder FUN_180012458 performs, plus the model-strip.
 * Returns the patched JSON (empty on parse failure). */
static std::string tl_build_timeline_host_json(const std::string &full_json)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(full_json.data(), (int)full_json.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::string();
    QJsonObject root = doc.object();
    if (!root.contains("entityDef") || !root.value("entityDef").isObject()) return std::string();
    QJsonObject ed   = root.value("entityDef").toObject();
    QJsonObject st   = ed.value("state").isObject() ? ed.value("state").toObject() : QJsonObject();
    QJsonObject edit = st.value("edit").isObject() ? st.value("edit").toObject() : QJsonObject();

    edit.insert("componentTimeLine", tl_empty_component());
    /* model-strip: clear renderModelInfo.model (keep the rest of renderModelInfo if present). */
    QJsonObject rmi = edit.value("renderModelInfo").isObject() ? edit.value("renderModelInfo").toObject()
                                                               : QJsonObject();
    rmi.insert("model", QString(""));
    edit.insert("renderModelInfo", rmi);

    /* className/inherit live on the entityDef object (the OG sets them there: defsub+0x60/+0x58). */
    ed.insert("className", QString("idTarget_Timeline"));
    ed.insert("inherit",   QString("snapmaps/unknown"));

    st.insert("edit", edit);
    ed.insert("state", st);
    root.insert("entityDef", ed);
    QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return std::string(out.constData(), (size_t)out.size());
}

/* "Timeline N": N = 1 + count of existing timeline-class entities (idTarget_Timeline / idEncounterManager -- the
 * same set the Timelines list filters on, sh_tabs.cpp). A fresh timeline gets a sensible default name in the list
 * instead of a blank/inherited one (the Timelines list labels each row by the entity displayName). */
static std::string tl_next_timeline_name(sh_iface *iface)
{
    int n = 0;
    int cnt = (iface && iface->vtbl && iface->vtbl->entity_count) ? iface->vtbl->entity_count(iface) : 0;
    for (int id = 0; id < cnt; ++id) {
        char cls[128] = {0};
        if (iface->vtbl->get_classname_copy &&
            iface->vtbl->get_classname_copy(iface, id, cls, (int)sizeof(cls))) {
            if (!strcmp(cls, "idTarget_Timeline") || !strcmp(cls, "idEncounterManager")) ++n;
        }
    }
    char buf[32];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "Timeline %d", n + 1);
    return std::string(buf);
}

/* replace the FIRST occurrence of `from` with `to` in `s`; returns true if found. */
static bool tl_replace_first(std::string &s, const char *from, const std::string &to)
{
    size_t pos = s.find(from);
    if (pos == std::string::npos) return false;
    s.replace(pos, strlen(from), to);
    return true;
}

/* Build a fresh idSnapEntityPrefab for a NEW timeline host by SUBSTITUTING the byte-exact, engine-accepted OG
 * mkcmd prefab template (SH_MKCMD_PREFAB_TEMPLATE -- the same one snapstack.cpp::h_mkcmd pastes). The template is
 * an idTarget_Command; we turn it into an idTarget_Timeline with 3 targeted string replaces:
 *   - displayName  "idTarget_Command" -> "Timeline N"               (a sensible default name in the list)
 *   - className    "idTarget_Command" -> "idTarget_Timeline"
 *   - the edit body  commandText placeholder -> an empty componentTimeLine
 * Everything else (grabAxis/grabDistance/cameraYaw/customIcon/variables/references/targets/spawnPosition) is kept
 * VERBATIM, so the float leaves carry the engine-required ".0" + customIcon is present -- the two things a
 * Qt-rebuilt prefab got WRONG (it serialized 0.0 -> "0" -> "...grabAxis.mat[0].z is not a floating point", and
 * omitted "...entities[0].customIcon"). The inherit is already "snapmaps/unknown" in the template (no model). The
 * entity is left GRABBED (the template's grab transform) so the engine paste-instantiate places it at the cursor.
 * NOTE (v2 polish): the template carries 16 unused persistentInteger vars (idTarget_Command's) -- cosmetic +
 * engine-valid on a timeline; strip later if it clutters the variable panel. Returns "" on a substitution miss. */
static std::string tl_build_spawn_prefab(sh_iface *iface)
{
    std::string p(SH_MKCMD_PREFAB_TEMPLATE);
    std::string name = tl_next_timeline_name(iface);   /* plain ASCII "Timeline N" -- no JSON escaping needed */
    if (!tl_replace_first(p, "\"displayName\":\"idTarget_Command\"", "\"displayName\":\"" + name + "\""))
        return std::string();
    if (!tl_replace_first(p, "\"className\":\"idTarget_Command\"", "\"className\":\"idTarget_Timeline\""))
        return std::string();
    /* the template edit body is: {"commandText" : "__SNAPHAK_MKCMD_COMMANDTEXT__","spawnPosition":{...}} */
    if (!tl_replace_first(p, "\"commandText\" : \"__SNAPHAK_MKCMD_COMMANDTEXT__\"",
                             "\"componentTimeLine\":{\"entityEvents\":{\"num\":0}}"))
        return std::string();
    /* STRIP the OG command-entity's 16 persistentInteger variables -- idTarget_Command baggage a timeline has no
     * use for, whose `idRange< int >` bounds spam "Issue deserializing variable" console warnings. Replace the
     * whole variables block (unique outer "~type":"idSnapVariables") with an EMPTY idSnapVariables (no vars of any
     * type). persistentInteger=[] removes the bounds entirely -> no idRange warning, and a fresh timeline carries
     * zero phantom variables. */
    {
        size_t vpos = p.find("\"variables\":{");
        size_t vend = (vpos == std::string::npos) ? std::string::npos
                                                   : p.find("\"~type\":\"idSnapVariables\"}", vpos);
        if (vpos != std::string::npos && vend != std::string::npos) {
            vend += strlen("\"~type\":\"idSnapVariables\"}");
            p.replace(vpos, vend - vpos,
                "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"boolean\":[],\"cachedEntity\":[],"
                "\"color\":[],\"customEvent\":[],\"integer\":[],\"number\":[],\"persistentInteger\":[],"
                "\"playerResource\":[],\"string\":[],\"teamResource\":[],\"~type\":\"idSnapVariables\"}");
        }
    }
    return p;
}

void sh_timeline_create_new(ShWinController *win)
{
    /* CREATE-NEW-TIMELINE (clone EXCEEDS OG -- the OG button is unwired; create-timeline-re). Two branches,
     * both gated on being TABBED INSIDE a module (EntityMode):
     *   - an entity is SELECTED -> MORPH it into a timeline host (className=idTarget_Timeline,
     *     inherit=snapmaps/unknown, empty componentTimeLine, model stripped) via the C2 bss-apply, then open it.
     *   - NOTHING selected -> SPAWN a fresh idTarget_Timeline host GRABBED at the camera center (the prefab
     *     paste). The user then double-clicks it in the Timelines list to author it. */
    if (!win || !win->iface) return;
    sh_iface *iface = win->iface;

    /* GATE (FACET 1): only inside a module. (The Qt button is also grayed out off-EntityMode -- belt + braces.) */
    if (!tl_iface_is_entity_mode(iface)) {
        tl_iface_toast(iface, "Timeline", "Create New Timeline: tab into a module first.");
        return;
    }

    /* is anything selected NOW? (FACET 2 -- get_selection count > 0). */
    int ids[1] = {-1};
    int nsel = (iface->vtbl && iface->vtbl->get_selection) ? iface->vtbl->get_selection(iface, ids, 1) : 0;

    /* The selection must be a LIVE entity. After a DELETE the editor selection can dangle at the freed slot; the
     * MORPH branch below would then reclass that freed entity, and the atomic +0x268 apply's class-derive compat
     * check reads the dangling className idStr -> an access-violation in the engine string compare (the observed
     * delete-timeline-then-create-timeline crash). A stale/invalid selection is treated as "nothing selected" so
     * we fall through to the SPAWN branch (create a fresh timeline) -- the same is_valid_id guard every other
     * per-entity read uses (tl_build_entity_model, sh_tabs). */
    bool sel_live = (nsel > 0 && ids[0] >= 0 && tl_iface_is_valid(iface, ids[0]));

    if (sel_live) {
        /* MORPH branch: reclass the selected entity into a timeline host + strip its model. LIVE-FIX 2026-06-25:
         * the class+inherit must go through the ATOMIC +0x268 apply, NOT the bss-apply alone -- the bss changed
         * className but left the engine inherit at the entity's original (e.g. snapmaps/audio/2d_speaker), so the
         * snapmaps/unknown-forced model-strip never landed AND later commits got REJECTED by the compat guard
         * ("idTarget_Timeline does not derive from <old-inherit>'s base"). Mirror the SnapStack/Save-to-Decl
         * pattern: atomic apply (FINAL-pair check) -> bss-apply for componentTimeLine -> re-assert (the explicit
         * pair wins over the rebuild's revert, so the morph sticks in defsub+0x60/+0x58). */
        int hostId = ids[0];
        std::string mname = tl_next_timeline_name(iface);   /* computed BEFORE the reclass (hostId not yet a timeline) */
        int r = tl_iface_apply_class_inherit(iface, hostId, "idTarget_Timeline", "snapmaps/unknown");  /* +0x268 atomic */
        if (r == 0) {
            tl_iface_toast(iface, "Timeline", "Create New Timeline: this entity can't become a timeline host (incompatible class).");
            return;
        }
        std::string full = tl_iface_serialize_entity(iface, hostId);
        if (full.empty()) { tl_iface_toast(iface, "Timeline", "Create New Timeline: could not read the selected entity."); return; }
        std::string patched = tl_build_timeline_host_json(full);
        if (patched.empty()) { tl_iface_toast(iface, "Timeline", "Create New Timeline: host-morph build failed."); return; }
        if (!tl_iface_schedule_apply(iface, hostId, patched, "create-timeline")) {
            tl_iface_toast(iface, "Timeline", "Create New Timeline: morph schedule failed (editor down?).");
            return;
        }
        tl_iface_apply_class_inherit(iface, hostId, "idTarget_Timeline", "snapmaps/unknown");  /* +0x268 RE-ASSERT */
        /* default-name ONLY if the source entity had no meaningful displayName (don't clobber an intentional one).
         * The Timelines list labels each row by displayName, so an unnamed morph would otherwise show blank. */
        if (iface->vtbl->get_displayname && iface->vtbl->set_entity_0x170) {
            char dn[128] = {0};
            iface->vtbl->get_displayname(iface, hostId, dn, (int)sizeof(dn));
            if (!dn[0]) iface->vtbl->set_entity_0x170(iface, hostId, mname.c_str());
        }
        /* in-place reclass leaves entity_count UNCHANGED, so the count-gated list poll won't catch it -> force a
         * Timelines-list re-scan so the morphed host appears WITHOUT a manual Refresh (Task B). */
        if (win) win->flagword |= SH_FLAG_REBUILD_LIST;
        tl_iface_toast(iface, "Timeline", "Create New Timeline: converted the selected entity into a timeline host.");
        /* open the editor on the morphed host so it commits thereafter (TL+0xf8 = hostId). */
        sh_timeline_open(win, hostId);
        return;
    }

    /* SPAWN branch (Path B): build the timeline prefab from the byte-exact OG template (tl_build_spawn_prefab),
     * then schedule a kind=2 STAGE+INSTANTIATE: the backend deserializes it into editor+0x209a8 AND calls the
     * engine paste-instantiate (PasteInstantiate FUN_14054f950, RE'd by timeline-spawn-instantiate-re) so the
     * host is actually PLACED in the map (camera-relative grab drop + AddToSelection) -> it auto-appears in the
     * Timelines list (entity_count +1 -> the count-poll rebuild). Plain mkcmd stays kind=1 (stage-only). */
    std::string prefab = tl_build_spawn_prefab(iface);
    if (prefab.empty()) { tl_iface_toast(iface, "Timeline", "Create New Timeline: prefab build failed (template substitution miss)."); return; }
    sh_apply_item it;
    it.kind = 2;                 /* stage + INSTANTIATE (place the new timeline), not stage-only mkcmd */
    it.id   = 0;
    it.text = prefab.c_str();
    int ok = (iface->vtbl && iface->vtbl->apply_edit) ? iface->vtbl->apply_edit(iface, &it, 1, "create-timeline") : 0;
    if (!ok) { tl_iface_toast(iface, "Timeline", "Create New Timeline: could not schedule (editor down?)."); return; }
    /* the deferred instantiate places the host + its className resolves a beat later -> re-scan the Timelines
     * list for ~1.5s so it self-appears without a manual Refresh (sh_dispatch_flagword consumes this). */
    win->spawn_rebuild_frames = 45;
    /* The backend places + selects the host on the game thread and toasts the real result ("create-timeline:
     * applied 1/1"); this frontend toast is the scheduled-acknowledgement. */
    tl_iface_toast(iface, "Timeline", "Create New Timeline: placing a new timeline at the camera (it will be selected).");
}

void sh_timeline_commit(ShWinController *win)
{
    ShTimelineEditor *ed = tl_get(win);
    if (!ed) return;

    /* the OG commit guard: if(TL+0xf8 != -1). A broken/Create-New timeline (entity_id == -1) SKIPS the
     * commit -- the faithful brokenness. */
    if (ed->entity_id < 0) {
        tl_iface_toast(ed->iface, "Timeline", "no committable timeline (unresolved id -- OG quirk).");
        return;
    }
    if (!tl_iface_is_valid(ed->iface, ed->entity_id)) {
        tl_iface_toast(ed->iface, "Timeline", "timeline entity no longer valid.");
        return;
    }

    /* COLLECT the UI -> the nested componentTimeLine decl object (the GOOD form). */
    QJsonObject comp = tl_build_component_json(ed);
    const char *compKey = ed->is_encounter ? "encounterComponent" : "componentTimeLine";

    /* fresh-serialize the entity, patch state.edit.<compKey>, SCHEDULE via +0xd0 (the C2 apply path). */
    std::string full = tl_iface_serialize_entity(ed->iface, ed->entity_id);
    if (full.empty()) { tl_iface_toast(ed->iface, "Timeline", "timeline serialize failed (editor down?)."); return; }
    std::string patched = tl_patch_component(full, compKey, comp);
    if (patched.empty()) { tl_iface_toast(ed->iface, "Timeline", "timeline patch failed."); return; }

    if (!tl_iface_schedule_apply(ed->iface, ed->entity_id, patched, "timeline")) {
        tl_iface_toast(ed->iface, "Timeline", "timeline commit: schedule failed (editor down?).");
        return;
    }
    /* QOL: committed -> no unsaved changes -> gray the Save Timeline button until the next edit. */
    ed->dirty = false;
    tl_set_save_enabled(ed->win, false);
}
