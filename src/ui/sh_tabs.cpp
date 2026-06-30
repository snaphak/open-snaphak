/* sh_tabs.cpp -- the per-DATA-tab handlers + the FUN_180014e7c flag-word dispatch bodies.
 *
 * Clean-room, FAITHFUL port of the OG snaphakui.dll data-tab logic (the 4 DATA tabs: Entities /
 * Entity-State / Prefabs / Timelines -- NOT the Timeline Editor, which is C3b). Built entirely from the
 * RE'd OG handlers (FUN_1800147e8 entity-list populate / FUN_180017384 ctx-menu / FUN_180017d00 Save-to-Decl
 * capture / FUN_180013c50 prefab create / FUN_180017538 prefab load / FUN_180017444 timelines dblclick) +
 * the master dispatch FUN_180014e7c (the |1/|2/|8/|0x20/|0x40 flag consumers + the per-frame Entity-State
 * read-sync). Zero OG bytes. All editor work routes through the BACKEND-owned interface vtable (the
 * engine-touch slots); the frontend does ONLY the Qt + the decision logic.
 *
 * Reproduces OG behavior incl. its quirks (the fidelity bar -- match OG's observable behavior, quirks and all; a later-fix pass corrects them):
 *   - the Entity-State Save does NO class/inherit compatibility check (a mismatch faults on next use,
 *     caught by the backend fault-shield instead of faulting DOOM; reproduced deliberately -- a later-fix pass adds the check).
 *   - the |2 consumer commit ORDER is +0x78(class) -> +0x128(displayname) -> +0x80(inherit) ->
 *     +0x40(decl-rebuild) -> re-assert +0x78/+0x80/+0x128 (the OG re-assert so the explicit boxes win over
 *     the rebuild), exactly as FUN_180014e7c.
 *   - the entity list dual-adds idTarget_Timeline / idEncounterManager into the Timelines list (OG quirk).
 *   - the prefab path = %USERPROFILE%/snaphak/prefabs/<name>.json (the +0xc0 resolver).
 */
#include "sh_controller.h"
#include "snapstack.h"
#include "snaphak_iface.h"
#include "sh_timeline.h"   /* C3b: the Timeline-Editor (the |0x80 commit + the dblclick open) */
#include "sh_entity_desc.h" /* GENERATED: OUR RE-extracted Inherit/Classname descriptions (the desc box) */

#include <string>
#include <unordered_map>   /* CLONE EXTENSION: the entity-desc name->ShEntDesc lookup map */
#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QMainWindow>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QComboBox>
#include <QAbstractItemView>   /* QComboBox::view()->isVisible() popup guard */
#include <cstring>             /* strlen -- walking the packed valid-class buffer */
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QClipboard>
#include <QInputDialog>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>

/* ================================================================ iface-call wrappers ==============
 * Thin null-checked wrappers over the interface vtable's engine-touch slots (the backend SEH-guards each
 * body). Every one null-checks the slot so a partial backend build degrades cleanly (empty string / no-op),
 * never crashes. Mirror the snapstack.cpp helper style. */

static int iface_entity_count(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->entity_count) return 0;
    return iface->vtbl->entity_count(iface);
}
static bool iface_is_valid_id(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->is_valid_id) return false;
    return iface->vtbl->is_valid_id(iface, id) != 0;
}
static std::string iface_id_string(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->id_to_string) {
        char d[24]; _snprintf_s(d, sizeof d, _TRUNCATE, "%d", id); return std::string(d);
    }
    char buf[512]; buf[0] = '\0';
    const char *s = iface->vtbl->id_to_string(iface, id, buf, (int)sizeof(buf));
    if (s && s[0]) return std::string(s);
    char d[24]; _snprintf_s(d, sizeof d, _TRUNCATE, "%d", id); return std::string(d);
}
static std::string iface_classname(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_classname_copy) return std::string();
    char buf[256]; buf[0] = '\0';
    const char *s = iface->vtbl->get_classname_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
static std::string iface_inherit(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_inherit_copy) return std::string();
    char buf[256]; buf[0] = '\0';
    const char *s = iface->vtbl->get_inherit_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
static std::string iface_displayname(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_displayname) return std::string();
    char buf[256]; buf[0] = '\0';
    const char *s = iface->vtbl->get_displayname(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
static std::string iface_declsource(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_declsource_copy) return std::string();
    /* the canonical decl-source can be large; size the buffer generously. */
    static char buf[64 * 1024];
    buf[0] = '\0';
    const char *s = iface->vtbl->get_declsource_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
static void iface_set_classname(sh_iface *iface, int id, const std::string &v)
{
    if (iface && iface->vtbl && iface->vtbl->set_classname) iface->vtbl->set_classname(iface, id, v.c_str());
}
static void iface_set_inherit(sh_iface *iface, int id, const std::string &v)
{
    if (iface && iface->vtbl && iface->vtbl->set_inherit) iface->vtbl->set_inherit(iface, id, v.c_str());
}
static void iface_set_displayname(sh_iface *iface, int id, const std::string &v)
{
    if (iface && iface->vtbl && iface->vtbl->set_entity_0x170)
        iface->vtbl->set_entity_0x170(iface, id, v.c_str());
}
static void iface_rebuild_declsource(sh_iface *iface, int id, const std::string &v)
{
    if (iface && iface->vtbl && iface->vtbl->rebuild_set_declsource)
        iface->vtbl->rebuild_set_declsource(iface, id, v.c_str());
}
/* +0x268 ATOMIC class+inherit set: one FINAL-pair check then BOTH idStr fields, guard-bypassed.
 * 1 = applied, 0 = rejected (fatal combo -- the caller MUST skip the rebuild), -1 = slot absent (old backend). */
static int iface_apply_class_inherit(sh_iface *iface, int id, const std::string &cls, const std::string &inh)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_class_inherit) return -1;
    return iface->vtbl->apply_class_inherit(iface, id, cls.c_str(), inh.c_str());
}
static void iface_remove_from_selection(sh_iface *iface, int id)
{
    if (iface && iface->vtbl && iface->vtbl->selection_guard)
        iface->vtbl->selection_guard(iface, id);
}
static std::string iface_serialize_selection(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->serialize_selection) return std::string();
    static char buf[256 * 1024];
    buf[0] = '\0';
    int n = iface->vtbl->serialize_selection(iface, buf, (int)sizeof(buf));
    if (n <= 0) return std::string();
    return std::string(buf, (size_t)n);
}
static std::string iface_resolve_prefab_path(sh_iface *iface, const std::string &prefix,
                                             const std::string &name)
{
    if (!iface || !iface->vtbl || !iface->vtbl->resolve_prefab_path) return std::string();
    char buf[1024]; buf[0] = '\0';
    int ok = iface->vtbl->resolve_prefab_path(iface, prefix.c_str(), name.c_str(), buf, (int)sizeof(buf));
    if (!ok || !buf[0]) return std::string();
    return std::string(buf);
}

/* +0x270 ENUMERATE the engine-valid classes for `inherit` -> packed NUL-terminated strings (double-NUL end). */
static int iface_valid_classes(sh_iface *iface, const char *inherit, char *buf, int cap, int *count)
{
    if (count) *count = 0;
    if (!iface || !iface->vtbl || !iface->vtbl->enum_valid_classes || !buf || cap < 2) return 0;
    return iface->vtbl->enum_valid_classes(iface, inherit, buf, cap, count);
}

/* repopulate the class QComboBox with the engine-valid classes for `inherit` (the linked dropdown),
 * and DEFAULT the selection to the FIRST valid child of the new inherit's base. A changed inherit invalidates
 * the old class (it is almost never a child of the new inherit's base type), so snapping to a guaranteed-
 * compatible class beats feeding the Save a fatal pair. The read-sync's class block runs AFTER this and
 * overrides to the LIVE class for a real morph / entity-switch, so this default only "wins" for a user-driven
 * inherit pick (the intended UX). blockSignals so it doesn't re-trigger the class combo's own handler. An
 * unresolvable inherit (ok==0 / slot absent) -> the combo is left editable-empty (the free-text hatch). */
void sh_repopulate_class_combo(ShWinController *win, const QString &inherit)
{
    if (!win) return;
    QComboBox *cls = static_cast<QComboBox *>(win->ui[SH_UI_entity_classname_edit]); /* WUI macro is #defined below */
    if (!cls) return;
    /* QOL: only repopulate (which resets the class to index 0) when the inherit ACTUALLY changed. The inherit
     * combo's editingFinished fires on the lineEdit's focus-loss -- e.g. just clicking the dropdown ARROW to
     * browse -- with an UNCHANGED inherit, which used to spuriously reset the classname to idAASObstacle. Cache
     * the last inherit; no-op if unchanged so the class only auto-switches when the user picks a NEW inherit. */
    std::string inh_key = inherit.toStdString();
    if (inh_key == win->last_class_combo_inherit && cls->count() > 0) return;
    win->last_class_combo_inherit = inh_key;
    static char buf[64 * 1024]; int count = 0;
    int ok = iface_valid_classes(win->iface, inherit.toLocal8Bit().constData(), buf, (int)sizeof(buf), &count);
    cls->blockSignals(true);
    cls->clear();
    if (ok && count > 0) {
        for (const char *p = buf; *p; p += std::strlen(p) + 1)   /* packed NUL-terminated strings, double-NUL end */
            cls->addItem(QString::fromLocal8Bit(p));
        cls->setCurrentIndex(0);                          /* default to the FIRST valid child of the new inherit */
    } else {
        cls->clearEditText();                             /* unresolvable inherit -> empty editable hatch */
    }
    cls->blockSignals(false);
}

static bool iface_editor_ready(sh_iface *iface)
{
    /* +0x28 IS-VALID(id) is also the editor-readiness gate the OG uses (it calls (*(+0x28))() with no id
     * -> a "map open?" probe). We treat a non-null is_valid_id slot + a positive entity_count as ready. */
    if (!iface || !iface->vtbl) return false;
    return iface_entity_count(iface) >= 0 && iface->vtbl->is_valid_id != nullptr;
}
static void iface_toast(sh_iface *iface, const char *title, const char *text)
{
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, title, text);
}

/* widget fetch shortcut. */
#define WUI(idx) (win->ui[(idx)])

/* ================================================================ ENTITIES tab =====================
 * Port of FUN_1800147e8 (per-id list populate), called in a count loop by FUN_180014e7c's |1/|8 rebuild.
 * Per id: resolve id->string; skip names starting "NULL_"; if classname is idTarget_Timeline /
 * idEncounterManager ALSO add to the Timelines list; if HideBuiltin checked skip id <= 0x37; else apply the
 * substring filter (the line_edit_entity_filter text) + addItem. The item's data role 0x30 carries the id. */

/* OG stores the id at QListWidgetItem+0x30 via setData; we use Qt::UserRole. */
static const int SH_ITEM_ID_ROLE = 0x100;   /* Qt::UserRole (32) base; any custom role >= UserRole works */

static void populate_one_entity(ShWinController *win, sh_iface *iface, int id)
{
    /* skip invalid (the +0x28 guard). */
    if (!iface_is_valid_id(iface, id)) return;

    std::string idStr = iface_id_string(iface, id);
    /* skip placeholder/builtin slots whose id-string starts "NULL_" (OG: *(idstr)==0x4c4c554e && [4]=='_'). */
    if (idStr.size() >= 5 && idStr.compare(0, 5, "NULL_") == 0) return;

    std::string cls = iface_classname(iface, id);

    /* dual-add idTarget_Timeline / idEncounterManager into the Timelines list (OG quirk). */
    if (cls == "idTarget_Timeline" || cls == "idEncounterManager") {
        QListWidget *tl = static_cast<QListWidget *>(WUI(SH_UI_timeline_list));
        if (tl) {
            /* label with the timeline's displayName ("BIND ALL DOORS") -- the user-given name -- not the raw
             * id-string ("snapmaps/unknown_<id>"). Fall back to the id-string for an unnamed timeline. The id
             * still travels in SH_ITEM_ID_ROLE so the double-click open is unchanged. */
            std::string dn = iface_displayname(iface, id);
            QString qlabel = QString::fromStdString(dn.empty() ? idStr : dn);
            /* Timelines searchbar: if non-empty, require the row label CONTAIN the filter text (case-insensitive). */
            QLineEdit *tflt = static_cast<QLineEdit *>(WUI(SH_UI_line_edit_timeline_filter));
            bool keep = !tflt || tflt->text().isEmpty() || qlabel.contains(tflt->text(), Qt::CaseInsensitive);
            if (keep) {
                QListWidgetItem *it = new QListWidgetItem(qlabel);
                it->setData(SH_ITEM_ID_ROLE, id);
                tl->addItem(it);
            }
        }
    }

    /* HideBuiltin: when checked, skip ids <= 0x37 (builtins are 0..55). */
    QCheckBox *hide = static_cast<QCheckBox *>(WUI(SH_UI_hide_builtin_checkbox));
    bool hideBuiltin = hide && hide->isChecked();
    if (hideBuiltin && id <= 0x37) return;

    /* OG (FUN_1800147e8) appends ":<displayName>" to the id-string when the entity has a name (the +0x70
     * get_displayname_ptr slot), and labels/filters the COMBINED string -> the list shows the entity name
     * beside its id AND is searchable by name. Our prior port used the bare id-string for both, dropping the
     * name. Restore the OG label; the colon suffix appears only for a named entity (displayName non-empty). */
    std::string dn = iface_displayname(iface, id);
    std::string label = dn.empty() ? idStr : (idStr + ":" + dn);

    /* the substring filter (line_edit_entity_filter). Empty -> no filter (OG: if filter non-empty, require
     * the COMBINED label CONTAIN it -> searchable by id-string OR name). */
    QLineEdit *flt = static_cast<QLineEdit *>(WUI(SH_UI_line_edit_entity_filter));
    if (flt) {
        QByteArray f = flt->text().toLocal8Bit();
        if (f.size() > 0 && label.find(f.constData()) == std::string::npos) return;
    }

    QListWidget *lst = static_cast<QListWidget *>(WUI(SH_UI_widget_entity_list));
    if (lst) {
        QListWidgetItem *it = new QListWidgetItem(QString::fromStdString(label));
        it->setData(SH_ITEM_ID_ROLE, id);
        lst->addItem(it);
    }
}

void sh_rebuild_entity_list(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;

    QListWidget *lst = static_cast<QListWidget *>(WUI(SH_UI_widget_entity_list));
    QListWidget *tl  = static_cast<QListWidget *>(WUI(SH_UI_timeline_list));

    /* BULK-INSERT batching: block per-item signals (each clear/add otherwise fires currentItemChanged ->
     * the handler), disable sorting (a sorted list insertion-sorts O(N) per add), and defer the per-add
     * repaint during the bulk fill, then restore. (Defensive -- on a full map the real cost was a backend
     * fault per entity, fixed there; this keeps the Qt side O(N) regardless.) */
    bool lstSort = lst && lst->isSortingEnabled();
    bool tlSort  = tl && tl->isSortingEnabled();
    bool lstBlk = lst && lst->blockSignals(true);
    bool tlBlk  = tl && tl->blockSignals(true);
    if (lst) { lst->setUpdatesEnabled(false); lst->setSortingEnabled(false); lst->clear(); }
    if (tl)  { tl->setUpdatesEnabled(false);  tl->setSortingEnabled(false);  tl->clear(); }

    int count = iface_entity_count(iface);
    for (int id = 0; id < count; id++)
        populate_one_entity(win, iface, id);

    if (lst) { lst->setSortingEnabled(lstSort); lst->blockSignals(lstBlk); lst->setUpdatesEnabled(true); }
    if (tl)  { tl->setSortingEnabled(tlSort);   tl->blockSignals(tlBlk);   tl->setUpdatesEnabled(true); }
}

/* the Entities right-click ctx-menu (port FUN_180017384): Copy ID / Delete / Push to stack 0. */
void sh_entity_context_menu(ShWinController *win, int id, const std::vector<int> &selected_ids,
                            const QPoint &global_pos)
{
    if (!win) return;
    sh_iface *iface = win->iface;

    /* With 2+ entities selected, show ONLY "Push to stack 0" (Copy ID / Delete are single-entity ops --
     * user request). The right-click handler (sh_setupui.cpp) has already collapsed the selection to the
     * clicked row if it was outside the selection, so `multi` reflects a deliberate multi-pick. */
    const bool multi = selected_ids.size() > 1;
    QMenu menu(nullptr);
    QAction *copyId = nullptr;
    QAction *del    = nullptr;
    if (!multi) {
        copyId = menu.addAction(QApplication::translate("QtWidgetsApplication1Class", "Copy ID"));
        del    = menu.addAction(QApplication::translate("QtWidgetsApplication1Class", "Delete"));
    }
    QAction *pushStk0 = menu.addAction(QApplication::translate("QtWidgetsApplication1Class", "Push to stack 0"));

    QAction *chosen = menu.exec(global_pos);
    if (chosen == nullptr) return;

    if (chosen == copyId) {
        /* Copy ID -> QClipboard::setText of the FULL in-game id string (Qt clipboard, NOT iface +0x178).
         * DIVERGENCE (known, tied to the id-string-not-byte-captured limitation): the OG FUN_180018304
         * copies the entity NAME from snaphakui's DAT_180031818 name table (stride 0xa8, string at
         * record+0x88); the clone copies the +0x18 id-string which falls back to the DECIMAL id (the reference
         * implementation entityIdString). So the clone yields e.g. "42" where the OG yields the entity name. The list
         * items are labeled by the same decimal id, so it is internally consistent; capturing the real
         * name table is a later cleanup item (capabilities gui_tab_entities.faithful_quirk). */
        std::string idStr = iface_id_string(iface, id);
        QClipboard *cb = QApplication::clipboard();
        if (cb) cb->setText(QString::fromStdString(idStr));
    } else if (chosen == del) {
        /* Delete -> iface +0x130 remove-from-selection (the OG WIN+0x228 pending-delete routes here). */
        iface_remove_from_selection(iface, id);
        /* rebuild so the deleted entity drops out of the list. */
        win->flagword |= SH_FLAG_REBUILD_LIST;
    } else if (chosen == pushStk0) {
        /* Push to stack 0. Multi-pick (Ctrl/Shift-selected 2+ rows) -> stack ALL selected (dedup happens on
         * the stack push). Single -> push the right-clicked row (the handler already collapsed the selection
         * to it). Repeat right-click -> push to build a stack one at a time, or multi-select to do it at once. */
        if (multi) sh_snapstack_push_ids(0, selected_ids);
        else       sh_snapstack_push_one(0, id);
    }
}

/* ================================================================ ENTITY-STATE tab =================
 * Save click capture (port FUN_180017d00): snapshot the 4 editable widgets into the WIN Save members +
 * raise |2. The per-frame |2 consumer (in sh_dispatch_flagword) does the +0x78/+0x128/+0x80/+0x40 commit. */
void sh_entity_state_save_clicked(ShWinController *win)
{
    if (!win) return;
    QPlainTextEdit *decl = static_cast<QPlainTextEdit *>(WUI(SH_UI_entity_text_editor));
    QComboBox *cls = static_cast<QComboBox *>(WUI(SH_UI_entity_classname_edit));   /* linked combos */
    QComboBox *inh = static_cast<QComboBox *>(WUI(SH_UI_entity_inherit_edit));
    QLineEdit *dnm = static_cast<QLineEdit *>(WUI(SH_UI_entity_displayname_lineedit));

    /* OG FUN_180017d00 snapshots decl-text(WIN[0xc]), classname(WIN[0x10]), inherit(WIN[0x14]),
     * displayname(WIN[0x18]) -- the id box is NOT captured. (combos: currentText) */
    win->save_decl_text   = decl ? std::string(decl->toPlainText().toLocal8Bit().constData()) : std::string();
    win->save_classname   = cls  ? std::string(cls->currentText().toLocal8Bit().constData())   : std::string();
    win->save_inherit     = inh  ? std::string(inh->currentText().toLocal8Bit().constData())   : std::string();
    win->save_displayname = dnm  ? std::string(dnm->text().toLocal8Bit().constData())          : std::string();

    win->flagword |= SH_FLAG_APPLY_STATE;
}

/* the per-frame Entity-State READ-SYNC (port of FUN_180014e7c's read block): when an entity is selected +
 * the Synchronize checkbox is on, repopulate the 4 widgets + the decl text from the LIVE entity. Runs every
 * frame (cheap; the OG does it under the loop mutex). `id` = win->displayed_id. */
/* ---- CLONE EXTENSION: the Entity-State Inherit/Classname description box --------------------------
 * Renders OUR RE-extracted descriptions (sh_entity_desc.h -- mined from the source-of-record decls,
 * deepened by decompiled behavior) for the CURRENTLY shown Inherit-path + Classname into the read-only
 * desc box. Called from the read-sync tail (which is polled), so it reflects both the synced entity AND a
 * manual dropdown pick; a last-key cache makes it a no-op when nothing changed (no per-frame churn). */
static const ShEntDesc *es_lookup_desc(const std::string &name)
{
    static std::unordered_map<std::string, const ShEntDesc *> *idx = nullptr;
    if (!idx) {
        idx = new std::unordered_map<std::string, const ShEntDesc *>();
        idx->reserve((size_t)SH_ENTITY_DESCS_N * 2);
        for (int i = 0; i < SH_ENTITY_DESCS_N; i++)
            (*idx)[std::string(SH_ENTITY_DESCS[i].name)] = &SH_ENTITY_DESCS[i];
    }
    if (name.empty()) return nullptr;
    auto it = idx->find(name);
    return it == idx->end() ? nullptr : it->second;
}

static void es_set_desc(ShWinController *win)
{
    if (!win) return;
    QPlainTextEdit *box = static_cast<QPlainTextEdit *>(WUI(SH_UI_entity_desc_box));
    if (!box) return;
    QComboBox *inh = static_cast<QComboBox *>(WUI(SH_UI_entity_inherit_edit));
    QComboBox *cls = static_cast<QComboBox *>(WUI(SH_UI_entity_classname_edit));
    std::string inhName = inh ? inh->currentText().toStdString() : "";
    std::string clsName = cls ? cls->currentText().toStdString() : "";
    std::string key = inhName + "\x1f" + clsName;
    if (key == win->last_desc_key) return;   /* idempotent -- nothing changed this poll */
    win->last_desc_key = key;

    /* show the EVIDENCE TIER after each name (honesty -- the user asked specifically which descriptions are
     * decompiled vs decl-inferred). source: "decompile"=read from the engine's decompiled behavior;
     * "decompile+schema"/"schema+decl"=field layout from the idlib schema + the decl I/O grammar;
     * "decl"=the source-of-record decls (wiring grammar) interpreted into prose. */
    auto tier = [](const ShEntDesc *d) -> QString {
        if (!d || !d->source || !d->confidence) return QString();
        return QString("   [%1 evidence; %2 confidence]").arg(d->source).arg(d->confidence);
    };
    QString text;
    if (!inhName.empty()) {
        const ShEntDesc *d = es_lookup_desc(inhName);
        text += "INHERIT  " + QString::fromStdString(inhName) + tier(d) + "\n";
        text += d ? QString::fromStdString(d->summary)
                  : QStringLiteral("(no reverse-engineered description on file for this inherit)");
        if (!clsName.empty()) text += "\n\n";
    }
    if (!clsName.empty()) {
        const ShEntDesc *d = es_lookup_desc(clsName);
        text += "CLASS  " + QString::fromStdString(clsName) + tier(d) + "\n";
        text += d ? QString::fromStdString(d->summary)
                  : QStringLiteral("(no reverse-engineered description on file for this class)");
    }
    box->setPlainText(text);
}

static void entity_state_read_sync(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;
    int id = win->displayed_id;
    es_set_desc(win);   /* QOL: refresh the desc box from the CURRENT combos EVERY frame (this runs before the
                         * id<0 early-return below) -- so the box initializes on map open + follows dropdown
                         * browsing, not just entity selection. Key-cached, so it's a no-op when unchanged. */

    QPlainTextEdit *decl = static_cast<QPlainTextEdit *>(WUI(SH_UI_entity_text_editor));
    QComboBox *cls = static_cast<QComboBox *>(WUI(SH_UI_entity_classname_edit));   /* linked combos */
    QComboBox *inh = static_cast<QComboBox *>(WUI(SH_UI_entity_inherit_edit));
    QLineEdit *dnm = static_cast<QLineEdit *>(WUI(SH_UI_entity_displayname_lineedit));
    QLineEdit *idl = static_cast<QLineEdit *>(WUI(SH_UI_entity_id_lineedit));

    /* GAP-2 clear-on-deselect (OG FUN_180014e7c L250-264): when a synced entity goes invalid (deleted /
     * map-switch / editor-down) the OG BLANKS the panel + resets the synced id. Our old code just early-
     * returned, leaving the dead entity's decl/class/inherit/displayname/id on screen + displayed_id pinned to a
     * dead id (a later Save-to-Decl could aim at it). Now we clear, like the OG. */
    if (id >= 0 && !iface_is_valid_id(iface, id)) {
        win->displayed_id = -1;
        win->last_synced_id = -1;
        win->last_synced_decl.clear();
        win->last_synced_inherit.clear();
        win->last_synced_classname.clear();
        win->last_synced_displayname.clear();
        if (decl) decl->clear();
        if (cls)  cls->clearEditText();
        if (inh)  inh->clearEditText();
        if (dnm)  dnm->clear();
        if (idl)  idl->clear();
        es_set_desc(win);   /* clear the desc box too (the combos are now empty) */
        return;
    }
    if (id < 0) {   /* nothing selected (and not a deselect transition) -- nothing to save, so gray the button */
        if (QPushButton *sb = static_cast<QPushButton *>(WUI(SH_UI_button_save_state_to_decl))) sb->setEnabled(false);
        return;
    }

    /* GAP-3 read-populate TRIGGER (the OG is a pending-id one-shot + a decl-divergence re-trigger; we were
     * setText/setPlainText EVERY frame, which reset the decl box scroll/caret ~60x/sec while unfocused and
     * clobbered an unsaved typed edit on focus-loss). Mirror the OG: only push a widget when its value actually
     * CHANGED (the id changed OR the live value differs from what we last synced). The decl box uses the
     * last_synced_decl cache (OG WIN+0x1c). A widget is only auto-overwritten when NOT focused (preserves an
     * in-progress edit). */
    bool id_changed = (id != win->last_synced_id);
    win->last_synced_id = id;

    if (decl && !decl->hasFocus()) {
        std::string live = iface_declsource(iface, id);
        if (id_changed || live != win->last_synced_decl) {
            decl->setPlainText(QString::fromStdString(live));
            win->last_synced_decl = live;
        }
    }
    /* sync the INHERIT combo -- but ONLY when the LIVE inherit actually CHANGED (a morph or a new
     * entity), exactly like the decl box above. The old code compared the COMBO to live and snapped it back,
     * which reverted the user's in-flight pick every frame: picking from a dropdown drops focus, so the
     * !hasFocus guard did not protect it. Cache the live value + push only on a real change. When the inherit
     * IS pushed we repopulate the class list so it matches the new inherit's valid children. */
    if (inh && !inh->hasFocus() && !inh->view()->isVisible()) {
        std::string live = iface_inherit(iface, id);
        if (id_changed || live != win->last_synced_inherit) {
            inh->setEditText(QString::fromStdString(live));
            win->last_synced_inherit = live;
            sh_repopulate_class_combo(win, QString::fromStdString(live));
        }
    }
    if (cls && !cls->hasFocus()) {
        std::string live = iface_classname(iface, id);   /* lag-fixed: reads defsub+0x60 direct */
        if (id_changed || live != win->last_synced_classname) {
            cls->setEditText(QString::fromStdString(live));
            win->last_synced_classname = live;
        }
    }
    if (dnm && !dnm->hasFocus()) {
        /* Change-GATED like the class/inherit combos above: push only when the id changed or the LIVE
         * displayname actually changed -- NOT whenever the box text differs from live. The old eager
         * `text != live -> setText(live)` reverted the user's TYPED name the instant the box lost focus
         * (e.g. on clicking Save), so Save captured the reverted old value -> the displayName never updated.
         * Caching last_synced_displayname makes an unsaved edit survive focus-out so Save captures it. */
        std::string live = iface_displayname(iface, id);
        if (id_changed || live != win->last_synced_displayname) {
            dnm->setText(QString::fromStdString(live));
            win->last_synced_displayname = live;
        }
    }
    if (idl) {   /* gate on id change OR a class change: the id-string does a module-path resolve (a loaded-map
                  * scan) that's the per-frame perf hit, so DON'T recompute every frame -- but a MORPH keeps the
                  * id while changing the className the id-string embeds, so we'd otherwise show a stale class
                  * (the "ID mismatch" a user can misread as corruption). The classname read here is the cheap
                  * defsub+0x60 read; the expensive id-string recompute fires only when id or class actually flips. */
        std::string liveCls = iface_classname(iface, id);
        if (id_changed || liveCls != win->last_idbox_class) {
            QString live = QString::fromStdString(iface_id_string(iface, id));   /* read-only */
            if (idl->text() != live) idl->setText(live);
            win->last_idbox_class = liveCls;
        }
    }

    /* GRAY-OUT: disable "Save to Decl" when the panel has NO unsaved edits -- every editable widget already
     * matches the LIVE entity, so a Save would be a no-op. Reads the live values directly (the same cheap
     * calls the sync above makes) so it stays correct even while a field is focused; flips only on a change. */
    if (QPushButton *saveBtn = static_cast<QPushButton *>(WUI(SH_UI_button_save_state_to_decl))) {
        bool dirty =
            (decl && decl->toPlainText() != QString::fromStdString(iface_declsource(iface, id)))  ||
            (cls  && cls->currentText()  != QString::fromStdString(iface_classname(iface, id)))    ||
            (inh  && inh->currentText()  != QString::fromStdString(iface_inherit(iface, id)))      ||
            (dnm  && dnm->text()         != QString::fromStdString(iface_displayname(iface, id)));
        if (saveBtn->isEnabled() != dirty) saveBtn->setEnabled(dirty);
    }

    es_set_desc(win);   /* CLONE EXTENSION: refresh the Inherit/Classname description box (key-cached no-op if unchanged) */
}

/* Public one-shot: load a specific entity's state into the Entity-State panel. Used by the entities-list
 * double-click (open the panel populated with THAT entity, sync off). Sets the displayed id + forces a
 * repopulate (last_synced_id = -1 -> the read-sync sees id_changed and pushes every widget). */
void sh_entity_state_load(ShWinController *win, int id)
{
    if (!win) return;
    win->displayed_id = id;
    win->last_synced_id = -1;
    entity_state_read_sync(win);
}

/* the |2 APPLY_STATE consumer (port of FUN_180014e7c's `& 2` branch): the EXACT commit order
 * +0x78(class) -> +0x128(displayname) -> +0x80(inherit) -> +0x40(decl-rebuild) -> re-assert
 * +0x78/+0x80/+0x128. OG had NO class/inherit compatibility check (a mismatch faulted on next use). The clone
 * added the +0x268 atomic morph guard (rejects a fatal class+inherit pair -> SKIP the rebuild, r==0); a later pass
 * adds the user-visible warn on that refusal. Gated on a valid displayed id. */
static void apply_entity_state(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;
    int id = win->displayed_id;
    if (id < 0 || !iface_is_valid_id(iface, id)) return;

    /* route the class+inherit pair through the ATOMIC slot (+0x268) -- one FINAL-pair check then both
     * fields, guard-bypassed -- so a cross-family MORPH applies instead of being rejected at its invalid
     * intermediate. The +0x40 rebuild + the re-assert are GATED on r != 0: on a REJECTED fatal pair (r==0)
     * feeding the fatal new headers (save_decl_text) to DeclSourceRebuild would re-introduce the unrecoverable
     * reparse fault the guard just prevented, so we SKIP the rebuild + re-assert. displayname (+0x128) is an
     * independent field -- it always lands (now after the pair instead of between class+inherit; inert). The
     * r==-1 legacy branch (slot absent / old backend) keeps the prior unconditional behavior. */
    int r = iface_apply_class_inherit(iface, id, win->save_classname, win->save_inherit);  /* +0x268 */
    if (r == -1) {                                              /* slot absent (old backend) -> legacy guarded */
        iface_set_classname(iface, id, win->save_classname);    /* +0x78 */
        iface_set_inherit(iface, id, win->save_inherit);        /* +0x80 */
    }
    iface_set_displayname(iface, id, win->save_displayname);    /* +0x128 (unguarded; always lands) */
    if (r != 0) {                                              /* applied (1) or legacy (-1): rebuild + re-assert */
        iface_rebuild_declsource(iface, id, win->save_decl_text);   /* +0x40 Save-to-Decl route */
        /* re-assert AFTER the rebuild so the explicit boxes win over the rebuild's re-emitted headers. */
        int r2 = iface_apply_class_inherit(iface, id, win->save_classname, win->save_inherit); /* +0x268 re-assert */
        if (r2 == -1) {
            iface_set_classname(iface, id, win->save_classname);    /* +0x78 */
            iface_set_inherit(iface, id, win->save_inherit);        /* +0x80 */
        }
        iface_set_displayname(iface, id, win->save_displayname);    /* +0x128 re-assert */
        /* a Save-to-Decl className change can add/remove this entity from the Timelines filter, and a displayName
         * change updates its list label -- both leave entity_count UNCHANGED, so the count-gated poll misses them.
         * Force a list re-scan so the Entities + Timelines lists reflect the edit without a manual Refresh (Task B). */
        win->flagword |= SH_FLAG_REBUILD_LIST;
    }
    else {
        /* FIX (warn+refuse): r==0 -> the +0x268 morph guard REJECTED a fatal class+inherit pair. The
         * rebuild is already SKIPPED (above) so the entity is NOT corrupted; surface the refusal to the user
         * (OG silently produced a malformed def that faulted on next use). */
        iface_toast(iface, "Entity-State Save",
                    "Save refused: incompatible class+inherit combination (would corrupt the entity).");
    }
}

/* ================================================================ PREFABS tab ======================
 * create (port FUN_180013c50): QInputDialog name -> resolve the path (+0xc0) -> addItem + raise |0x40.
 * The |0x40 consumer (sh_dispatch_flagword) serializes the selection (+0xb0) + fwrites it.
 * load (port FUN_180017538): resolve the path -> fread the file body into WIN[0x2d] -> raise |0x20. The
 * |0x20 consumer deserializes it via +0xb8 into editor+0x209a8 (the Ctrl+V paste slot). */

void sh_prefab_create_clicked(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;
    QMainWindow *parent = win->window;

    bool ok = false;
    QString name = QInputDialog::getText(
        parent,
        QApplication::translate("QtWidgetsApplication1Class", "Create New Prefab"),
        QApplication::translate("QtWidgetsApplication1Class", "Prefab name"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    /* resolve %USERPROFILE%/snaphak/prefabs/<name>.json (the OG: prefix "prefabs/" + name + ".json"). */
    std::string nm = std::string(name.toLocal8Bit().constData());
    std::string path = iface_resolve_prefab_path(iface, "prefabs/", nm + ".json");
    if (path.empty()) return;

    win->prefab_create_path = path;
    /* addItem(the typed name) to the prefab listview + raise the write flag. */
    QListWidget *lst = static_cast<QListWidget *>(WUI(SH_UI_prefab_listview));
    if (lst) lst->addItem(name);
    win->flagword |= SH_FLAG_WRITE_PREFAB;
}

void sh_prefab_item_double_clicked(ShWinController *win, const QString &stem)
{
    if (!win) return;
    sh_iface *iface = win->iface;

    std::string nm = std::string(stem.toLocal8Bit().constData());
    std::string path = iface_resolve_prefab_path(iface, "prefabs/", nm + ".json");
    if (path.empty()) {   /* OG FUN_180017538 +0x98 "Failed to get path for prefab %s." -> the live +0x1b8 toast */
        iface_toast(iface, "Prefabs", (std::string("Failed to get path for prefab ") + nm + ".").c_str());
        return;
    }

    /* fread the whole file into WIN[0x2d] (the |0x20 paste source). OG: fopen rb -> ftell -> fread. */
    FILE *fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) {   /* OG +0x98 "Failed to open prefab file %s." */
        iface_toast(iface, "Prefabs", (std::string("Failed to open prefab file ") + path + ".").c_str());
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return; }
    std::string body;
    body.resize((size_t)sz);
    size_t got = fread(&body[0], 1, (size_t)sz, fp);
    fclose(fp);
    body.resize(got);

    win->prefab_loaded_body = body;
    win->flagword |= SH_FLAG_SUBMIT_MKCMD;   /* |0x20 -> paste into editor+0x209a8 */
}

/* re-scan %USERPROFILE%/snaphak/prefabs and repopulate the list (OG ctor directory_iterator showing stems).
 * The dir is resolved via the +0xc0 resolver with an empty name (giving the dir prefix), then QDir lists it. */
void sh_prefab_list_populate(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;
    QListWidget *lst = static_cast<QListWidget *>(WUI(SH_UI_prefab_listview));
    if (!lst) return;
    lst->clear();

    /* resolve the prefabs DIR (prefix "prefabs/", empty name -> the trailing dir path). */
    std::string dirPath = iface_resolve_prefab_path(iface, "prefabs/", "");
    if (dirPath.empty()) return;

    QDir dir(QString::fromStdString(dirPath));
    if (!dir.exists()) return;
    QStringList filters;
    filters << "*.json";
    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : entries)
        lst->addItem(fi.completeBaseName());   /* the stem (no .json), like the OG */
}

/* ================================================================ TIMELINES tab ====================
 * double-click (port FUN_180017444): stash the selected timeline id into the timeline editor's pending slot
 * (OG: *(WIN+0x1d0) = item id) -- the Timeline Editor (C3b) picks it up. The DUAL-POPULATE of the Timelines
 * list happens during the entity-list rebuild (populate_one_entity dual-adds), per the OG quirk. */
void sh_timeline_item_double_clicked(ShWinController *win, int id)
{
    if (!win) return;
    /* C3b: the OG FUN_180017444 stashes the chosen id onto the Timeline-Editor's deferred-load slot
     * (TL+0x1d0) + switches to tab 5, then a load repopulates the editor. The clone OPENs the editor
     * directly on the clicked id (sh_timeline_open builds the TL QTabWidget, caches the id + iface, COLLECTs
     * the live componentTimeLine into the UI rows, and switches to the Timeline-Editor tab). */
    sh_timeline_open(win, id);
}

/* Camera-Origin / Lock-Position sync (OG FUN_180014e7c L154-207). Runs every frame UNCONDITIONALLY
 * (the OG runs it OUTSIDE the Synchronize gate). Reads the editor camera-origin vec3 (slot +0x08 = editor+0x170);
 * if Lock-Position is checked, freezes it to the stored vec3; for each unfocused axis whose value changed, shows
 * it in the X/Y/Z box; if the user committed an edit (cam_dirty, set on editingFinished), reads the box back into
 * the vec3; if any axis is dirty OR Lock is set, writes the vec3 back to the editor (slot +0x00) and clears the
 * dirty flags; caches cam_xyz for next-frame change-detect + the Lock source. A clean no-op off-editor. */
static void camera_vec3_sync(ShWinController *win)
{
    sh_iface *iface = win->iface;
    if (!iface || !iface->vtbl || !iface->vtbl->get_editor_vec3) return;
    float cam[3] = {0.0f, 0.0f, 0.0f};
    iface->vtbl->get_editor_vec3(iface, cam);

    QCheckBox *lock = static_cast<QCheckBox *>(WUI(SH_UI_checkbox_lock_position));
    bool locked = lock && lock->isChecked();
    QLineEdit *bx[3] = {
        static_cast<QLineEdit *>(WUI(SH_UI_camera_x)),
        static_cast<QLineEdit *>(WUI(SH_UI_camera_y)),
        static_cast<QLineEdit *>(WUI(SH_UI_camera_z)),
    };
    bool any_dirty = false;
    for (int i = 0; i < 3; i++) {
        if (win->cam_dirty[i]) {                        /* the user committed an edit -> push it to the editor */
            if (bx[i]) cam[i] = bx[i]->text().toFloat();
            any_dirty = true;
        } else if (locked) {                            /* Lock -> freeze to the stored origin */
            cam[i] = win->cam_xyz[i];
        } else if (bx[i] && !bx[i]->hasFocus()) {       /* else show the live camera value (changed + unfocused) */
            QString s = QString::number((double)cam[i], 'g', 8);
            if (bx[i]->text() != s) bx[i]->setText(s);
        }
    }
    if (any_dirty || locked) {                          /* flush user edits / the locked origin back to the editor */
        if (iface->vtbl->set_editor_vec3) iface->vtbl->set_editor_vec3(iface, cam);
        win->cam_dirty[0] = win->cam_dirty[1] = win->cam_dirty[2] = false;
    }
    win->cam_xyz[0] = cam[0]; win->cam_xyz[1] = cam[1]; win->cam_xyz[2] = cam[2];   /* cache */
}

/* ================================================================ the flag-word dispatch ===========
 * The bodies for FUN_180014e7c's per-frame consumers. Runs under the think-loop (the +0x1a0 drain
 * already ran the queued `sh` ops on this thread). ALSO runs the Entity-State read-sync every frame. */
void sh_dispatch_flagword(ShWinController *win)
{
    if (!win) return;
    sh_iface *iface = win->iface;

    /* The Entity-State panel populates from win->displayed_id. The Qt entity-list CLICK sets displayed_id
     * (currentItemChanged, sh_setupui.cpp) REGARDLESS of the Synchronize checkbox -- so clicking a list entity must
     * populate the panel even when Synchronize is OFF. The Synchronize checkbox gates ONLY the EDITOR-selection
     * poll below (whether the panel ALSO follows the 3D-editor selection). The OG read-populate is likewise gated
     * on the pending id (set by either the list or the editor poll), not on the checkbox. */
    {
        QCheckBox *sync = static_cast<QCheckBox *>(WUI(SH_UI_synchronize_checkbox));
        if (sync && sync->isChecked()) {
            /* "Synchronize With Editor" FOLLOWS the EDITOR's live selection (OG FUN_180014e7c @0x14e9c-0x14f60):
             * poll slot +0x150 (get_selection) every frame; when EXACTLY ONE entity is selected and its id differs
             * from what's shown, re-point displayed_id at it. The oldSel/newSel 2-frame gate mirrors the OG (don't
             * sync the first frame a selection appears; 0 or >1 selected -> newSel=-1 -> keep the last). */
            static int s_last_editor_sel = -1;
            int oldSel = s_last_editor_sel, newSel = -1;
            if (iface && iface->vtbl && iface->vtbl->get_selection) {
                int ids[8];
                int n = iface->vtbl->get_selection(iface, ids, 8);
                if (n == 1 && iface_is_valid_id(iface, ids[0])) newSel = ids[0];
            }
            s_last_editor_sel = newSel;
            if (oldSel != -1 && newSel != -1 && win->displayed_id != newSel)
                win->displayed_id = newSel;
        }
    }
    entity_state_read_sync(win);   /* ALWAYS -- populate from displayed_id (set by the LIST click OR the editor poll) */

    camera_vec3_sync(win);   /* Camera-Origin: UNCONDITIONAL -- the OG runs it outside the Synchronize gate. */

    /* Create-New-Timeline GATE (clone EXCEEDS OG): gray the button out unless TABBED INSIDE a module (EntityMode,
     * iface +0x1c0). Cheap per-frame read; the create handler re-checks (belt + braces). create-timeline-re. */
    {
        QPushButton *cnt_btn = static_cast<QPushButton *>(WUI(SH_UI_button_create_new_timeline));
        if (cnt_btn) {
            bool inModule = iface && iface->vtbl && iface->vtbl->is_entity_mode
                            && iface->vtbl->is_entity_mode(iface) != 0;
            if (cnt_btn->isEnabled() != inModule) cnt_btn->setEnabled(inModule);
        }
    }

    /* AUTO-POPULATE the entity list on map load. entity_count goes 0 -> N when a map loads (and changes on
     * add/delete); a real change raises |1 so the list fills itself instead of requiring a manual Refresh.
     * Change-gated (cnt != last) so it fires once per change, never per frame. */
    {
        int cnt = iface_entity_count(iface);
        if (cnt != win->last_entity_count) {
            win->last_entity_count = cnt;
            win->flagword |= SH_FLAG_REBUILD_LIST;
        }
    }

    /* QOL: re-scan a few times over ~1.5s after a from-scratch timeline SPAWN. The spawn places the entity on
     * the game thread (deferred) + its className resolves a beat later, so the one-shot count-poll above can
     * rebuild BEFORE the new entity reads as idTarget_Timeline -> it misses the className-filtered Timelines list
     * on the first rebuild (the manual Refresh worked only because it ran later). This auto-Refreshes for ~1.5s. */
    if (win->spawn_rebuild_frames > 0) {
        win->spawn_rebuild_frames--;
        if ((win->spawn_rebuild_frames % 15) == 0) win->flagword |= SH_FLAG_REBUILD_LIST;
    }

    uint64_t f = win->flagword;
    if (f == 0)
        return;

    if (f & SH_FLAG_REBUILD_LIST) {
        /* |1 rebuild entity list (FUN_1800147e8 loop). */
        sh_rebuild_entity_list(win);
        win->flagword &= ~uint64_t(SH_FLAG_REBUILD_LIST);
    }
    if (f & SH_FLAG_REFILTER) {
        /* |8 refilter -- same build path (it applies the filter text). */
        sh_rebuild_entity_list(win);
        win->flagword &= ~uint64_t(SH_FLAG_REFILTER);
    }
    if (f & SH_FLAG_APPLY_STATE) {
        /* |2 apply Entity-State edit (the +0x78/+0x128/+0x80/+0x40 commit order; NO compat check). */
        apply_entity_state(win);
        win->flagword &= ~uint64_t(SH_FLAG_APPLY_STATE);
    }
    if (f & SH_FLAG_COPY_TO_PREFAB) {
        /* |0x10 copy selection -> prefab text (the OG raises this from the |0x40 path; it serializes the
         * selection into WIN[0x35]). We fold the serialize into the |0x40 write below, so this is a no-op
         * clear for parity. */
        win->flagword &= ~uint64_t(SH_FLAG_COPY_TO_PREFAB);
    }
    if (f & SH_FLAG_SUBMIT_MKCMD) {
        /* |0x20 prefab PASTE: deserialize the loaded prefab body via +0xb8 into editor+0x209a8 (the Ctrl+V
         * paste slot). We schedule a kind=1 (mkcmd/prefab) apply with the loaded body as the text so the
         * backend deserializes it as idSnapEntityPrefab on the DOOM main thread (decl-safe). */
        if (iface && iface->vtbl && iface->vtbl->apply_edit && !win->prefab_loaded_body.empty()) {
            sh_apply_item it;
            it.kind = 1;                                  /* mkcmd/prefab paste */
            it.id   = 0;
            it.text = win->prefab_loaded_body.c_str();
            iface->vtbl->apply_edit(iface, &it, 1, "prefab_paste");
        }
        win->flagword &= ~uint64_t(SH_FLAG_SUBMIT_MKCMD);
    }
    if (f & SH_FLAG_WRITE_PREFAB) {
        /* |0x40 write prefab file: serialize the selection (+0xb0) -> fwrite to the create path. */
        std::string body = iface_serialize_selection(iface);
        if (!body.empty() && !win->prefab_create_path.empty()) {
            FILE *fp = nullptr;
            if (fopen_s(&fp, win->prefab_create_path.c_str(), "wb") == 0 && fp) {
                fwrite(body.data(), 1, body.size(), fp);
                fclose(fp);
            } else {   /* OG dispatch +0x98 "Failed to open prefab file %s for saving." -> the live +0x1b8 toast */
                iface_toast(iface, "Prefabs",
                    (std::string("Failed to open prefab file ") + win->prefab_create_path + " for saving.").c_str());
            }
        }
        win->prefab_create_path.clear();
        win->flagword &= ~uint64_t(SH_FLAG_WRITE_PREFAB);
    }
    if (f & SH_FLAG_APPLY_TIMELINE) {
        /* |0x80 apply timeline edit (C3b): COLLECT the Timeline-Editor UI -> the nested componentTimeLine
         * decl object -> patch into a fresh serialize -> SCHEDULE via iface +0xd0 onto the timeline entity
         * (the clone_bss_apply path). Guarded `if(entity_id != -1)` inside sh_timeline_commit (the OG
         * TL+0xf8 != -1 commit guard -- a broken/Create-New timeline is a no-op, faithful R6). */
        sh_timeline_commit(win);
        win->flagword &= ~uint64_t(SH_FLAG_APPLY_TIMELINE);
    }
}
