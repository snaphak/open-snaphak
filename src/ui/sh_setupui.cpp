/* sh_setupui.cpp -- the faithful Qt widget-tree port of OG snaphakui.dll setupUi (FUN_18000cb6c @ RVA
 * 0xcb6c) + retranslateUi (FUN_18000e658 @ RVA 0xe658) + the WIN[0] flag-word dispatch skeleton
 * (FUN_180014e7c @ RVA 0x14e7c).
 *
 * CLEAN-ROOM: built entirely from the RE'd DATA (the decompiled widget tree + the decoded QStringLiteral
 * objectNames/titles + the QRect geometries). Zero OG bytes. Reproduces OG EXACTLY incl. its quirks
 * (the clone's fidelity bar -- match OG's observable behavior, quirks and all; a later-fix pass corrects them):
 *   - the QTabWidget objectName is "lua_scripts_page" (NOT "tabWidget") -- an OG mislabel.
 *   - the 6th tab (Editor Lua, objectName "tab_2") is an EMPTY QWidget -- no children, no layout.
 *   - the "X" label has a hardcoded setGeometry(QRect(0,0,5,19)) while its siblings are layout-managed.
 *   - the menuBar has an explicit setGeometry(QRect(0,0,1481,20)).
 *   - the window icon path is chrispy's literal dev path "../../../../Desktop/1076.bmp" (won't resolve;
 *     addFile silently no-ops -> no icon, same as OG).
 *   - the Camera-Origin label row order is X / Z / Y (label / label_3 / label_2) interleaved with the
 *     camera_x / camera_y / camera_z edits.
 *
 * Each quirk is reproduced deliberately (not a clone bug); the later-fix pass corrects it.
 *
 * HANDLERS = STUBS: connectSlotsByName(window) is called (faithful) and each interactive widget is wired
 * via explicit connect() to a stub lambda that only sets/clears the WIN[0] flag bits -- the real
 * entity/decl/prefab/timeline behavior and the SnapStack ops come later. The build is moc-free, so the
 * stub slots are lambdas (not Q_OBJECT on_<name>_<signal> methods); connectSlotsByName still runs to match
 * OG's call exactly (it finds no auto-slots on the plain QMainWindow, a no-op, as intended).
 */
#include "sh_controller.h"
#include "sh_timeline.h"   /* C3b: the Timeline-Editor wiring (insert-event / create-new) */

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QComboBox>
#include <QPoint>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QGroupBox>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QIcon>
#include <QSizePolicy>
#include <QRect>
#include <QString>
#include <QMetaObject>
#include <QMouseEvent>            /* drag-scrub on the Camera-Origin fields */
#include <QEvent>
#include "sh_inherit_universe.h"   /* SH_INHERIT_UNIVERSE[] -- the inherit-dropdown list */

/* OG QWIDGETSIZE_MAX = 16777215 = 0xffffff (the height in every maxSize call). */
static const int SH_QWIDGETSIZE_MAX = 16777215;

/* Drag-to-scrub for the Camera-Origin coordinate fields: press + drag horizontally on a box to scrub its value
 * (and mark that axis dirty so the per-frame camera_vec3_sync pushes it to the live editor camera in REAL TIME).
 * A plain click still places the caret -- scrubbing only begins once the drag passes a few px. moc-free: a
 * QObject that only overrides eventFilter (no signals/slots), parented to its QLineEdit so it auto-frees. */
class ShCamScrub : public QObject {
public:
    ShCamScrub(QLineEdit *e, ShWinController *w, int axis)
        : QObject(e), edit(e), win(w), ax(axis), dragging(false), scrubbed(false), startX(0), startVal(0.0)
    {
        edit->installEventFilter(this);
        edit->setCursor(Qt::SizeHorCursor);   /* hint: this field is draggable */
    }
    bool eventFilter(QObject *o, QEvent *ev) override
    {
        if (o != edit) return false;
        if (ev->type() == QEvent::MouseButtonPress) {
            QMouseEvent *m = static_cast<QMouseEvent *>(ev);
            if (m->button() == Qt::LeftButton) {
                dragging = true; scrubbed = false;
                startX = m->globalX(); startVal = edit->text().toDouble();
            }
            return false;   /* let the click through so a plain click still places the caret */
        }
        if (ev->type() == QEvent::MouseMove && dragging) {
            QMouseEvent *m = static_cast<QMouseEvent *>(ev);
            int dx = m->globalX() - startX;
            if (scrubbed || dx > 3 || dx < -3) {
                scrubbed = true;
                double v = startVal + (double)dx * 0.25;   /* 0.25 world units per pixel */
                edit->setText(QString::number(v, 'f', 3));
                if (win && ax >= 0 && ax < 3) win->cam_dirty[ax] = true;   /* -> per-frame sync pushes it live */
                return true;   /* consume so no text-selection happens while scrubbing */
            }
            return false;
        }
        if (ev->type() == QEvent::MouseButtonRelease) dragging = false;
        return false;
    }
private:
    QLineEdit *edit; ShWinController *win; int ax;
    bool dragging, scrubbed; int startX; double startVal;
};

/* Translation context literal (OG @0x30040). retranslate uses QCoreApplication::translate(ctx, text). */
static const char *SH_CTX = "QtWidgetsApplication1Class";

#define UISET(idx, ptr) (win->ui[(idx)] = (ptr))
#define UIGET(idx)      (win->ui[(idx)])

/* ---------------------------------------------------------------- retranslateUi (FUN_18000e658) ----
 * Sets every widget's text/title + the 6 tab titles. DIRECT from 0xe658 (25 translate calls + the tab
 * setTabText/indexOf pairs). Called at the tail of setupUi (OG: FUN_18000cb6c calls FUN_18000e658). */
static void sh_retranslateUi(QMainWindow *window, ShWinController *win)
{
    QTabWidget *tabs = static_cast<QTabWidget *>(UIGET(SH_UI_tabWidget));

    window->setWindowTitle(QApplication::translate(SH_CTX, "SnapHak Studio"));

    /* (OG's "Hide Builtin Snap Objects" checkbox was removed -- no text to set.) */
    static_cast<QLineEdit *>(UIGET(SH_UI_line_edit_entity_filter))
        ->setToolTip(QApplication::translate(SH_CTX, "Filter for entity names"));
    static_cast<QPushButton *>(UIGET(SH_UI_button_refresh_entity_list))
        ->setText(QApplication::translate(SH_CTX, "Refresh"));
    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_tab))),
                     QApplication::translate(SH_CTX, "Entities"));

    static_cast<QLabel *>(UIGET(SH_UI_label_5))->setText(QApplication::translate(SH_CTX, "Inherit:"));
    static_cast<QLabel *>(UIGET(SH_UI_label_4))->setText(QApplication::translate(SH_CTX, "Classname:"));
    static_cast<QLabel *>(UIGET(SH_UI_label_7))->setText(QApplication::translate(SH_CTX, "Entity Displayname"));
    static_cast<QCheckBox *>(UIGET(SH_UI_synchronize_checkbox))
        ->setToolTip(QApplication::translate(SH_CTX, "Synchronize with the selected object in SnapMap"));
    static_cast<QCheckBox *>(UIGET(SH_UI_synchronize_checkbox))
        ->setText(QApplication::translate(SH_CTX, "Synchronize With Editor"));
    static_cast<QLineEdit *>(UIGET(SH_UI_entity_id_lineedit))
        ->setToolTip(QApplication::translate(SH_CTX, "Full ingame id"));
    static_cast<QPushButton *>(UIGET(SH_UI_button_save_state_to_decl))
        ->setText(QApplication::translate(SH_CTX, "Save to Decl"));
    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_tab_3))),
                     QApplication::translate(SH_CTX, "Entity State"));

    /* (no create_new_prefab text -- the Prefabs tab is a "Coming soon" stub; the button was removed, so
     * UIGET(SH_UI_create_new_prefab) is null and setText'ing it here would AV at setupUi -- that was the crash.) */
    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_tab_4))),
                     QApplication::translate(SH_CTX, "Prefabs"));

    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_widget))),
                     QApplication::translate(SH_CTX, "Timelines"));

    static_cast<QGroupBox *>(UIGET(SH_UI_timeline_groupbox))
        ->setTitle(QApplication::translate(SH_CTX, "Current Timeline"));
    static_cast<QPushButton *>(UIGET(SH_UI_insert_entity_event))
        ->setText(QApplication::translate(SH_CTX, "Insert Entity Event"));
    {
        QPushButton *svt = static_cast<QPushButton *>(UIGET(SH_UI_save_entity_timeline));
        svt->setText(QApplication::translate(SH_CTX, "Save Timeline"));
        svt->setEnabled(false);   /* grayed until a timeline is open + has unsaved changes (dirty-tracking) */
    }
    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_tab_8))),
                     QApplication::translate(SH_CTX, "Timeline Editor"));
    tabs->setTabText(tabs->indexOf(static_cast<QWidget *>(UIGET(SH_UI_lua_page_tab_2))),
                     QApplication::translate(SH_CTX, "Editor Lua"));

    static_cast<QGroupBox *>(UIGET(SH_UI_groupBox))
        ->setTitle(QApplication::translate(SH_CTX, "Camera Origin"));
    static_cast<QLabel *>(UIGET(SH_UI_label))->setText(QApplication::translate(SH_CTX, "X"));
    static_cast<QLabel *>(UIGET(SH_UI_label_3))->setText(QApplication::translate(SH_CTX, "Z"));
    static_cast<QLabel *>(UIGET(SH_UI_label_2))->setText(QApplication::translate(SH_CTX, "Y"));
    static_cast<QCheckBox *>(UIGET(SH_UI_checkbox_lock_position))
        ->setText(QApplication::translate(SH_CTX, "Lock Position"));
}

/* ---------------------------------------------------------------- the signal wiring ----------------
 * These wire the REAL data-tab handlers (sh_tabs.cpp). The simple flag-raisers stay as the OG signal
 * graph (refresh/filter just raise the WIN[0] bits the per-frame dispatch consumes); the richer handlers
 * (Save capture, prefab create/load, ctx-menu, selection->displayed_id, timeline dblclick) call into the
 * sh_tabs.cpp bodies. The Timeline-Editor save stays a C3b flag stub (|0x80). */
static int sh_item_id_role() { return 0x100; }   /* matches SH_ITEM_ID_ROLE in sh_tabs.cpp */

static void sh_wire_stub_handlers(ShWinController *win)
{
    /* Entities: refresh.clicked -> |1 ; filter.textChanged -> |8 (the OG signal graph). */
    QObject::connect(static_cast<QPushButton *>(UIGET(SH_UI_button_refresh_entity_list)),
                     &QPushButton::clicked, [win]() { win->flagword |= SH_FLAG_REBUILD_LIST; });
    QObject::connect(static_cast<QLineEdit *>(UIGET(SH_UI_line_edit_entity_filter)),
                     &QLineEdit::textChanged, [win](const QString &) { win->flagword |= SH_FLAG_REFILTER; });
    /* Timelines searchbar (clone extension): textChanged -> refilter (rebuild re-applies the timeline name filter). */
    QObject::connect(static_cast<QLineEdit *>(UIGET(SH_UI_line_edit_timeline_filter)),
                     &QLineEdit::textChanged, [win](const QString &) { win->flagword |= SH_FLAG_REFILTER; });
    /* (OG's "Hide Builtin Snap Objects" checkbox was removed -- no toggle signal to wire.) */

    /* Entities list: selection -> set the synced/displayed id (the Entity-State read-sync target). */
    QListWidget *elist = static_cast<QListWidget *>(UIGET(SH_UI_widget_entity_list));
    /* ENHANCEMENT beyond OG (OG is single-select): allow Ctrl/Shift multi-select so several entities can be
     * pushed to a stack in one action. The right-click handler keeps it unconfusing -- right-clicking a row
     * that is NOT part of the current selection collapses the selection down to just that row. */
    elist->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QObject::connect(elist, &QListWidget::currentItemChanged,
                     [win](QListWidgetItem *cur, QListWidgetItem *) {
                         win->displayed_id = cur ? cur->data(sh_item_id_role()).toInt() : -1;
                     });
    /* Entities list: right-click context menu. With 2+ rows selected the menu shows ONLY "Push to stack 0"
     * (Copy ID / Delete are single-entity ops); a right-click on an UNSELECTED row first collapses the
     * selection to that row, so the menu + push always act on exactly what you clicked. */
    elist->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(elist, &QListWidget::customContextMenuRequested,
                     [win, elist](const QPoint &pos) {
                         QListWidgetItem *it = elist->itemAt(pos);
                         if (!it) return;
                         /* right-click outside the current selection -> select just this row (no stale
                          * multi-selection acting behind the user's back). A right-click WITHIN a multi-
                          * selection keeps it, so "Push to stack 0" stacks them all. */
                         if (!it->isSelected()) {
                             elist->clearSelection();
                             it->setSelected(true);
                             elist->setCurrentItem(it);
                         }
                         int id = it->data(sh_item_id_role()).toInt();
                         std::vector<int> selected_ids;
                         const QList<QListWidgetItem *> sel = elist->selectedItems();
                         for (QListWidgetItem *s : sel)
                             selected_ids.push_back(s->data(sh_item_id_role()).toInt());
                         sh_entity_context_menu(win, id, selected_ids, elist->mapToGlobal(pos));
                     });
    /* Entities list: DOUBLE-CLICK -> open the Entity-State panel populated with THAT entity, sync OFF (so it's a
     * snapshot you can edit, not a live mirror of the editor selection). (The single-click selection still just
     * sets displayed_id; the double-click is the explicit "open it" gesture the user asked for.) */
    {
        QCheckBox *syncCb = static_cast<QCheckBox *>(UIGET(SH_UI_synchronize_checkbox));
        QTabWidget *mainTabs = static_cast<QTabWidget *>(UIGET(SH_UI_tabWidget));
        QObject::connect(elist, &QListWidget::itemDoubleClicked,
                         [win, syncCb, mainTabs](QListWidgetItem *it) {
                             if (!it) return;
                             if (syncCb) syncCb->setChecked(false);                 /* sync UNCHECKED */
                             sh_entity_state_load(win, it->data(sh_item_id_role()).toInt());
                             if (mainTabs) mainTabs->setCurrentIndex(1);            /* the Entity State tab */
                         });
    }

    /* Entity State: Save.clicked -> capture the 4 widgets + raise |2 (FUN_180017d00). */
    QObject::connect(static_cast<QPushButton *>(UIGET(SH_UI_button_save_state_to_decl)),
                     &QPushButton::clicked, [win]() { sh_entity_state_save_clicked(win); });

    /* Entity State: inherit combo COMMIT (dropdown pick or typed+Enter) -> repopulate the class
     * combo with the engine-valid classes for that inherit. COMMIT-fire (activated + editingFinished), NOT
     * currentTextChanged, so we don't run up to 412 live ancestry walks per keystroke (verify_correctness
     * DEFECT 3). The dropdown then offers only classes a Save will accept (same sh_typeinfo_class_derives). */
    {
        QComboBox *inhCombo = static_cast<QComboBox *>(UIGET(SH_UI_entity_inherit_edit));
        QObject::connect(inhCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
                         [win, inhCombo](int) { sh_repopulate_class_combo(win, inhCombo->currentText()); });
        if (inhCombo->lineEdit())
            QObject::connect(inhCombo->lineEdit(), &QLineEdit::editingFinished,
                             [win, inhCombo]() { sh_repopulate_class_combo(win, inhCombo->currentText()); });
    }

    /* pre-populate the INHERIT dropdown with the placeable inherits + default to the universal hatch,
     * and seed the linked CLASS dropdown for it. sh_repopulate_class_combo -> the +0x270 slot, which serves the
     * dropdown from a thread-safe corpus snapshot when the live engine type-lookup is unavailable on this (UI)
     * thread -- so a direct call here is safe + instant. After this the inherit-commit + read-sync drive it
     * (the SAME path), so a populated class dropdown == the linked behavior works. */
    {
        QComboBox *inhCombo = static_cast<QComboBox *>(UIGET(SH_UI_entity_inherit_edit));
        inhCombo->blockSignals(true);
        /* LIVE entityDef registry (every valid inherit, ~2,500) via the +0x278 slot; static list is the
         * pre-boot fallback. Editable, so a custom or EMPTY inherit can still be typed (empty is engine-valid). */
        sh_populate_inherit_combo(win, inhCombo);
        inhCombo->setEditText(QStringLiteral("snapmaps/unknown"));
        inhCombo->blockSignals(false);
        sh_repopulate_class_combo(win, QStringLiteral("snapmaps/unknown"));
    }

    /* Camera-Origin: mark an axis dirty on editingFinished -> the per-frame camera_vec3_sync pushes the
     * typed coordinate to the editor camera (OG per-axis dirty flags WIN+8/+0x41/+0x42). */
    {
        QLineEdit *cx = static_cast<QLineEdit *>(UIGET(SH_UI_camera_x));
        QLineEdit *cy = static_cast<QLineEdit *>(UIGET(SH_UI_camera_y));
        QLineEdit *cz = static_cast<QLineEdit *>(UIGET(SH_UI_camera_z));
        if (cx) QObject::connect(cx, &QLineEdit::editingFinished, [win]() { win->cam_dirty[0] = true; });
        if (cy) QObject::connect(cy, &QLineEdit::editingFinished, [win]() { win->cam_dirty[1] = true; });
        if (cz) QObject::connect(cz, &QLineEdit::editingFinished, [win]() { win->cam_dirty[2] = true; });
        /* drag-to-edit: scrub a coordinate by dragging horizontally on its field -> live camera move (cam_dirty
         * -> the same per-frame camera_vec3_sync). Typing still works; a plain click still places the caret. */
        if (cx) new ShCamScrub(cx, win, 0);
        if (cy) new ShCamScrub(cy, win, 1);
        if (cz) new ShCamScrub(cz, win, 2);
    }

    /* Prefabs: tab is STUBBED ("Coming soon", deferred) -- the create/load controls are not built, so
     * there is no create/double-click wiring here (the sh_prefab_* bodies remain for later work). */

    /* Timelines: list double-click -> stash the timeline id (FUN_180017444; the Timeline Editor is C3b). */
    QListWidget *tlist = static_cast<QListWidget *>(UIGET(SH_UI_timeline_list));
    QObject::connect(tlist, &QListWidget::itemDoubleClicked,
                     [win](QListWidgetItem *it) {
                         if (it) sh_timeline_item_double_clicked(win, it->data(sh_item_id_role()).toInt());
                     });

    /* Timeline Editor: save_entity_timeline.clicked -> |0x80 (the per-frame |0x80 consumer runs
     * sh_timeline_commit on the UI thread, the reference implementation FIX-B style). */
    QObject::connect(static_cast<QPushButton *>(UIGET(SH_UI_save_entity_timeline)),
                     &QPushButton::clicked, [win]() { win->flagword |= SH_FLAG_APPLY_TIMELINE; });
    /* Timeline Editor: insert_entity_event.clicked -> append an empty event-row (FUN_180011e9c). */
    QObject::connect(static_cast<QPushButton *>(UIGET(SH_UI_insert_entity_event)),
                     &QPushButton::clicked, [win]() { sh_timeline_insert_event(win); });
    /* (No "Create New Timeline" button: the clone cannot fabricate a timeline entity outside the engine's own
     * creation path -- both a from-scratch spawn and a reclass-a-selected-entity morph corrupted the map. A
     * timeline is created by PLACING one from the in-game SnapMap entity palette instead; the Timeline Editor
     * here only authors events on an already-placed, engine-validated timeline.) */

    /* (prefab-list populate removed -- the Prefabs tab is a "Coming soon" stub, no list to fill.) */
}

/* ================================================================ setupUi (FUN_18000cb6c) ========== */
void sh_setupUi(QMainWindow *window, ShWinController *win)
{
    /* OG: if window objectName empty -> setObjectName(&DAT_180030040). DIRECT from FUN_18000cb6c line 80
     * (setupui.log) + qstrs.log line 44: DAT_180030040 decodes to "QtWidgetsApplication1Class" -- the SAME
     * literal OG reuses as the translation context (SH_CTX), which is why the DAT is shared. NOT "MainWindow"
     * (that was a uic-convention assumption; the binary disagrees). */
    if (window->objectName().isEmpty())
        window->setObjectName(QStringLiteral("QtWidgetsApplication1Class"));

    /* resize(0x5ca, 0x3b0) = 1482 x 944. */
    window->resize(1482, 944);

    /* QSizePolicy(Expanding, Expanding, DefaultType); H/V stretch 0; applied to the window.
     * DIRECT from FUN_18000cb6c line 85: QSizePolicy::QSizePolicy(local_res8,7,7,1) -- arg4=1. Per
     * qsizepolicy.h (Qt 5.9.9) ControlType DefaultType=0x1 (Frame=0x10), so the control type is DefaultType,
     * NOT Frame. Policy 7,7 = Expanding,Expanding (GrowFlag|ShrinkFlag|ExpandFlag = 7). */
    {
        QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Expanding, QSizePolicy::DefaultType);
        sp.setHorizontalStretch(0);
        sp.setVerticalStretch(0);
        sp.setHeightForWidth(window->sizePolicy().hasHeightForWidth());
        window->setSizePolicy(sp);
    }

    /* window icon: OG's literal dev path (won't resolve at runtime -> no icon; faithful quirk). */
    {
        QIcon icon;
        icon.addFile(QStringLiteral("../../../../Desktop/1076.bmp"), QSize(), QIcon::Normal, QIcon::On);
        window->setWindowIcon(icon);
    }

    /* Ui[0] centralWidget (parent=window). */
    QWidget *centralWidget = new QWidget(window);
    centralWidget->setObjectName(QStringLiteral("centralWidget"));
    UISET(SH_UI_centralWidget, centralWidget);

    /* Ui[1] verticalLayout_5 (on centralWidget). */
    QVBoxLayout *verticalLayout_5 = new QVBoxLayout(centralWidget);
    verticalLayout_5->setSpacing(6);
    verticalLayout_5->setContentsMargins(11, 11, 11, 11);
    verticalLayout_5->setObjectName(QStringLiteral("verticalLayout_5"));
    UISET(SH_UI_verticalLayout_5, verticalLayout_5);

    /* Ui[2] the QTabWidget -- QUIRK: objectName "lua_scripts_page" (NOT "tabWidget"). */
    QTabWidget *tabWidget = new QTabWidget(centralWidget);
    tabWidget->setObjectName(QStringLiteral("lua_scripts_page"));
    UISET(SH_UI_tabWidget, tabWidget);

    /* ---------------- Tab 1: Entities (objectName "tab") ---------------- */
    QWidget *tab = new QWidget();
    tab->setObjectName(QStringLiteral("tab"));
    UISET(SH_UI_tab, tab);
    QVBoxLayout *verticalLayout_4 = new QVBoxLayout(tab);
    verticalLayout_4->setSpacing(6);
    verticalLayout_4->setContentsMargins(11, 11, 11, 11);
    verticalLayout_4->setObjectName(QStringLiteral("verticalLayout_4"));
    UISET(SH_UI_verticalLayout_4, verticalLayout_4);

    /* OG's "Hide Builtin Snap Objects" checkbox (param_1[5]) is intentionally NOT created -- the
     * `snapEdit_enableDevLayer` cvar dev-layer gate replaces it (populate_one_entity, sh_tabs.cpp). */

    QListWidget *widget_entity_list = new QListWidget(tab);
    widget_entity_list->setObjectName(QStringLiteral("widget_entity_list"));
    UISET(SH_UI_widget_entity_list, widget_entity_list);
    verticalLayout_4->addWidget(widget_entity_list);

    QLineEdit *line_edit_entity_filter = new QLineEdit(tab);
    line_edit_entity_filter->setObjectName(QStringLiteral("line_edit_entity_filter"));
    /* match the Timelines-tab searchbar style: full-width + a "Search ..." placeholder (was a 300px cap, no hint). */
    line_edit_entity_filter->setPlaceholderText(QApplication::translate(SH_CTX, "Search entities..."));
    UISET(SH_UI_line_edit_entity_filter, line_edit_entity_filter);
    verticalLayout_4->addWidget(line_edit_entity_filter);

    QPushButton *button_refresh = new QPushButton(tab);
    button_refresh->setObjectName(QStringLiteral("button_refresh_entity_list"));
    button_refresh->setMaximumSize(QSize(50, 80));
    UISET(SH_UI_button_refresh_entity_list, button_refresh);
    verticalLayout_4->addWidget(button_refresh);

    tabWidget->addTab(tab, QString());

    /* ---------------- Tab 2: Entity State (objectName "tab_3") ---------------- */
    QWidget *tab_3 = new QWidget();
    tab_3->setObjectName(QStringLiteral("tab_3"));
    UISET(SH_UI_tab_3, tab_3);
    QGridLayout *gridLayout = new QGridLayout(tab_3);
    gridLayout->setSpacing(6);
    gridLayout->setContentsMargins(11, 11, 11, 11);
    gridLayout->setObjectName(QStringLiteral("gridLayout"));
    UISET(SH_UI_gridLayout, gridLayout);

    /* verticalLayout -- nested (no parent widget). */
    QVBoxLayout *verticalLayout = new QVBoxLayout();
    verticalLayout->setSpacing(6);
    verticalLayout->setObjectName(QStringLiteral("verticalLayout"));
    UISET(SH_UI_verticalLayout, verticalLayout);

    QPlainTextEdit *entity_text_editor = new QPlainTextEdit(tab_3);
    entity_text_editor->setObjectName(QStringLiteral("entity_text_editor"));
    UISET(SH_UI_entity_text_editor, entity_text_editor);
    verticalLayout->addWidget(entity_text_editor, 2);   /* QOL: decl editor gets ~2/3 of the tab (stretch 2:1) */

    /* horizontalLayout -- nested (no parent widget). */
    QHBoxLayout *horizontalLayout = new QHBoxLayout();
    horizontalLayout->setSpacing(6);
    horizontalLayout->setObjectName(QStringLiteral("horizontalLayout"));
    UISET(SH_UI_horizontalLayout, horizontalLayout);

    QLabel *label_5 = new QLabel(tab_3);
    label_5->setObjectName(QStringLiteral("label_5"));
    UISET(SH_UI_label_5, label_5);
    horizontalLayout->addWidget(label_5);

    QComboBox *entity_inherit_edit = new QComboBox(tab_3);          /* linked dropdown (was QLineEdit) */
    entity_inherit_edit->setObjectName(QStringLiteral("entity_inherit_edit"));
    entity_inherit_edit->setEditable(true);                        /* keep the free-text hatch (snapmaps/unknown etc.) */
    entity_inherit_edit->setInsertPolicy(QComboBox::NoInsert);
    entity_inherit_edit->setMaximumSize(QSize(300, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_entity_inherit_edit, entity_inherit_edit);
    horizontalLayout->addWidget(entity_inherit_edit);

    QLabel *label_4 = new QLabel(tab_3);
    label_4->setObjectName(QStringLiteral("label_4"));
    UISET(SH_UI_label_4, label_4);
    horizontalLayout->addWidget(label_4);

    QComboBox *entity_classname_edit = new QComboBox(tab_3);        /* linked dropdown (was QLineEdit) */
    entity_classname_edit->setObjectName(QStringLiteral("entity_classname_edit"));
    entity_classname_edit->setEditable(true);                      /* editable: a new-build class can still be typed */
    entity_classname_edit->setInsertPolicy(QComboBox::NoInsert);
    entity_classname_edit->setMaximumSize(QSize(200, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_entity_classname_edit, entity_classname_edit);
    horizontalLayout->addWidget(entity_classname_edit);

    QLabel *label_7 = new QLabel(tab_3);
    label_7->setObjectName(QStringLiteral("label_7"));
    UISET(SH_UI_label_7, label_7);
    horizontalLayout->addWidget(label_7);

    QLineEdit *entity_displayname_lineedit = new QLineEdit(tab_3);
    entity_displayname_lineedit->setObjectName(QStringLiteral("entity_displayname_lineedit"));
    entity_displayname_lineedit->setMaximumSize(QSize(200, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_entity_displayname_lineedit, entity_displayname_lineedit);
    horizontalLayout->addWidget(entity_displayname_lineedit);

    QCheckBox *synchronize_checkbox = new QCheckBox(tab_3);
    synchronize_checkbox->setObjectName(QStringLiteral("synchronize_with_editor_checkbox"));
    /* DEVIATION from OG (OG ships this UNCHECKED): default it ON so the Entity-State panel follows the live
     * 3D-editor selection out of the box (the read-sync's editor-selection poll is gated on isChecked()).
     * User-requested 2026-06-27 -- a usability default, not OG-faithful. */
    synchronize_checkbox->setChecked(true);
    UISET(SH_UI_synchronize_checkbox, synchronize_checkbox);
    horizontalLayout->addWidget(synchronize_checkbox);

    QLineEdit *entity_id_lineedit = new QLineEdit(tab_3);
    entity_id_lineedit->setObjectName(QStringLiteral("entity_id_lineedit"));
    entity_id_lineedit->setReadOnly(true);
    UISET(SH_UI_entity_id_lineedit, entity_id_lineedit);
    horizontalLayout->addWidget(entity_id_lineedit);

    QPushButton *button_save_state = new QPushButton(tab_3);
    button_save_state->setObjectName(QStringLiteral("button_save_state_to_decl"));
    UISET(SH_UI_button_save_state_to_decl, button_save_state);
    horizontalLayout->addWidget(button_save_state);

    verticalLayout->addLayout(horizontalLayout);

    /* CLONE EXTENSION (not OG): the Entity Description box -- shows OUR RE-extracted descriptions
     * (sh_entity_desc.h, mined from the source-of-record decls + deepened by decompiled behavior) for the
     * selected entity's Inherit + Classname. Read-only, word-wrapped, modest height; sits right under the
     * Inherit/Classname fields it describes. Populated by es_set_desc (sh_tabs.cpp) on selection/pick. */
    QGroupBox *desc_group = new QGroupBox(QApplication::translate(SH_CTX, "Entity Description (reverse-engineered)"), tab_3);
    QVBoxLayout *desc_layout = new QVBoxLayout(desc_group);
    desc_layout->setContentsMargins(6, 4, 6, 6);
    QPlainTextEdit *entity_desc_box = new QPlainTextEdit(desc_group);
    entity_desc_box->setObjectName(QStringLiteral("entity_desc_box"));
    entity_desc_box->setReadOnly(true);
    entity_desc_box->setPlaceholderText(QApplication::translate(SH_CTX,
        "Select an entity (or pick an Inherit / Classname) to see what it does."));
    UISET(SH_UI_entity_desc_box, entity_desc_box);
    desc_layout->addWidget(entity_desc_box);
    verticalLayout->addWidget(desc_group, 1);   /* QOL: description gets ~1/3 (stretch 1 vs the decl editor's 2) */

    gridLayout->addLayout(verticalLayout, 0, 0, 1, 1);

    tabWidget->addTab(tab_3, QString());

    /* ---------------- Prefabs (objectName "tab_4") -- STUBBED; displayed as the 2nd-last tab ---------------- */
    QWidget *tab_4 = new QWidget();
    tab_4->setObjectName(QStringLiteral("tab_4"));
    tab_4->setLayoutDirection(Qt::LeftToRight);
    UISET(SH_UI_tab_4, tab_4);
    QGridLayout *gridLayout_2 = new QGridLayout(tab_4);
    gridLayout_2->setSpacing(6);
    gridLayout_2->setContentsMargins(11, 11, 11, 11);
    gridLayout_2->setObjectName(QStringLiteral("gridLayout_2"));
    UISET(SH_UI_gridLayout_2, gridLayout_2);

    /* STUBBED: Prefabs is deferred -- a centered "Coming soon" placeholder, no list/create controls.
     * (Prefabs DID work in OG -- this is a deliberate defer, not a parity gap; the feature returns later.) */
    QLabel *prefab_soon = new QLabel(QApplication::translate(SH_CTX, "Coming soon"), tab_4);
    prefab_soon->setObjectName(QStringLiteral("prefab_coming_soon"));
    prefab_soon->setAlignment(Qt::AlignCenter);
    gridLayout_2->addWidget(prefab_soon, 0, 0, 1, 1);
    /* tab_4's addTab is DEFERRED to the end so Prefabs + Lua are the last two tabs -- see below. */

    /* ---------------- Tab 4: Timelines (objectName "widget") ---------------- */
    QWidget *widget = new QWidget();
    widget->setObjectName(QStringLiteral("widget"));
    UISET(SH_UI_widget, widget);
    QGridLayout *gridLayout_3 = new QGridLayout(widget);
    gridLayout_3->setSpacing(6);
    gridLayout_3->setContentsMargins(11, 11, 11, 11);
    gridLayout_3->setObjectName(QStringLiteral("gridLayout_3"));
    UISET(SH_UI_gridLayout_3, gridLayout_3);

    /* CLONE EXTENSION: the Timelines searchbar (mirrors the Entities-tab line_edit_entity_filter). Row 1, above
     * the list; filters the list by timeline name (populate_one_entity reads SH_UI_line_edit_timeline_filter). */
    QLineEdit *line_edit_timeline_filter = new QLineEdit(widget);
    line_edit_timeline_filter->setObjectName(QStringLiteral("line_edit_timeline_filter"));
    line_edit_timeline_filter->setPlaceholderText(QApplication::translate(SH_CTX, "Search timelines..."));
    UISET(SH_UI_line_edit_timeline_filter, line_edit_timeline_filter);
    gridLayout_3->addWidget(line_edit_timeline_filter, 0, 0, 1, 1);

    QListWidget *timeline_list = new QListWidget(widget);
    timeline_list->setObjectName(QStringLiteral("timeline_list"));
    UISET(SH_UI_timeline_list, timeline_list);
    gridLayout_3->addWidget(timeline_list, 1, 0, 1, 1);

    tabWidget->addTab(widget, QString());

    /* ---------------- Tab 5: Timeline Editor (objectName "tab_8") ---------------- */
    QWidget *tab_8 = new QWidget();
    tab_8->setObjectName(QStringLiteral("tab_8"));
    UISET(SH_UI_tab_8, tab_8);
    QGridLayout *gridLayout_4 = new QGridLayout(tab_8);
    gridLayout_4->setSpacing(6);
    gridLayout_4->setContentsMargins(11, 11, 11, 11);
    gridLayout_4->setObjectName(QStringLiteral("gridLayout_4"));
    UISET(SH_UI_gridLayout_4, gridLayout_4);

    QGroupBox *timeline_groupbox = new QGroupBox(tab_8);
    timeline_groupbox->setObjectName(QStringLiteral("timeline_groupbox"));
    UISET(SH_UI_timeline_groupbox, timeline_groupbox);
    QVBoxLayout *verticalLayout_3 = new QVBoxLayout(timeline_groupbox);
    verticalLayout_3->setSpacing(6);
    verticalLayout_3->setContentsMargins(11, 11, 11, 11);
    verticalLayout_3->setObjectName(QStringLiteral("verticalLayout_3"));
    UISET(SH_UI_verticalLayout_3, verticalLayout_3);

    /* verticalLayout_2 -- nested (no parent widget); addLayout'd into verticalLayout_3. */
    QVBoxLayout *verticalLayout_2 = new QVBoxLayout();
    verticalLayout_2->setSpacing(6);
    verticalLayout_2->setObjectName(QStringLiteral("verticalLayout_2"));
    UISET(SH_UI_verticalLayout_2, verticalLayout_2);
    verticalLayout_3->addLayout(verticalLayout_2);

    QPushButton *insert_entity_event = new QPushButton(timeline_groupbox);
    insert_entity_event->setObjectName(QStringLiteral("insert_entity_event"));
    insert_entity_event->setMaximumSize(QSize(100, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_insert_entity_event, insert_entity_event);
    verticalLayout_3->addWidget(insert_entity_event);

    QPushButton *save_entity_timeline = new QPushButton(tab_8);
    save_entity_timeline->setObjectName(QStringLiteral("save_entity_timeline"));
    save_entity_timeline->setMaximumSize(QSize(100, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_save_entity_timeline, save_entity_timeline);

    gridLayout_4->addWidget(timeline_groupbox, 1, 0, 1, 2);

    /* Save at the tab's BOTTOM-RIGHT corner -- matching the Entity-Decl tab's Save-to-Decl placement
     * (a right-aligned button on its own bottom row, not stacked inside the timeline group box). */
    QHBoxLayout *tlsave_row = new QHBoxLayout();
    tlsave_row->addStretch(1);
    tlsave_row->addWidget(save_entity_timeline);
    gridLayout_4->addLayout(tlsave_row, 2, 0, 1, 2);

    tabWidget->addTab(tab_8, QString());

    /* ---------------- Editor Lua (objectName "tab_2") -- STUBBED "Coming soon"; the LAST tab ---------------- */
    /* OG's Lua tab was "incomplete and has no function" (per the Guide) -- so a "Coming soon" stub IS faithful. */
    QWidget *lua_page = new QWidget();
    lua_page->setObjectName(QStringLiteral("tab_2"));   /* QUIRK: the empty Lua page is "tab_2" */
    UISET(SH_UI_lua_page_tab_2, lua_page);
    QGridLayout *gridLayout_lua = new QGridLayout(lua_page);
    gridLayout_lua->setContentsMargins(11, 11, 11, 11);
    gridLayout_lua->setObjectName(QStringLiteral("gridLayout_lua"));
    QLabel *lua_soon = new QLabel(QApplication::translate(SH_CTX, "Coming soon"), lua_page);
    lua_soon->setObjectName(QStringLiteral("lua_coming_soon"));
    lua_soon->setAlignment(Qt::AlignCenter);
    gridLayout_lua->addWidget(lua_soon, 0, 0, 1, 1);

    /* Prefabs + Editor Lua are the LAST two tabs (both stubbed) -- add them now, after Timeline Editor, so
     * the order is: Entity State / Entities / Timelines / Timeline Editor / Prefabs / Editor Lua. */
    tabWidget->addTab(tab_4, QString());      /* 2nd-last: Prefabs (stub) */
    tabWidget->addTab(lua_page, QString());   /* last: Editor Lua (stub) */

    /* the tabWidget goes into verticalLayout_5 AFTER all the tabs are added. */
    verticalLayout_5->addWidget(tabWidget);

    /* ---------------- Camera Origin (NOT a tab) -- objectName "groupBox" ---------------- */
    QGroupBox *groupBox = new QGroupBox(centralWidget);
    groupBox->setObjectName(QStringLiteral("groupBox"));
    groupBox->setMaximumSize(QSize(400, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_groupBox, groupBox);
    QHBoxLayout *horizontalLayout_3 = new QHBoxLayout(groupBox);
    horizontalLayout_3->setSpacing(6);
    horizontalLayout_3->setContentsMargins(11, 11, 11, 11);
    horizontalLayout_3->setObjectName(QStringLiteral("horizontalLayout_3"));
    UISET(SH_UI_horizontalLayout_3, horizontalLayout_3);

    QWidget *widget_2 = new QWidget(groupBox);
    widget_2->setObjectName(QStringLiteral("widget_2"));
    UISET(SH_UI_widget_2, widget_2);

    /* the "X" axis label. The OG wrapped it in widget_2 with a hardcoded 5px-wide geometry, which clipped it to
     * an invisible sliver (the user's "missing X label" bug). Parent it to the groupBox + add it DIRECTLY to the
     * row like its "Y"/"Z" siblings (label_2/label_3) so it renders. widget_2 is kept (UISET'd) but no longer
     * wraps/clips the label. */
    QLabel *label = new QLabel(groupBox);
    label->setObjectName(QStringLiteral("label"));
    UISET(SH_UI_label, label);
    horizontalLayout_3->addWidget(label);

    QLineEdit *camera_x = new QLineEdit(groupBox);
    camera_x->setObjectName(QStringLiteral("camera_x"));
    camera_x->setMaximumSize(QSize(100, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_camera_x, camera_x);
    horizontalLayout_3->addWidget(camera_x);

    QLabel *label_3 = new QLabel(groupBox);   /* "Z" */
    label_3->setObjectName(QStringLiteral("label_3"));
    UISET(SH_UI_label_3, label_3);
    horizontalLayout_3->addWidget(label_3);

    QLineEdit *camera_y = new QLineEdit(groupBox);
    camera_y->setObjectName(QStringLiteral("camera_y"));
    camera_y->setMaximumSize(QSize(100, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_camera_y, camera_y);
    horizontalLayout_3->addWidget(camera_y);

    QLabel *label_2 = new QLabel(groupBox);   /* "Y" */
    label_2->setObjectName(QStringLiteral("label_2"));
    UISET(SH_UI_label_2, label_2);
    horizontalLayout_3->addWidget(label_2);

    QLineEdit *camera_z = new QLineEdit(groupBox);
    camera_z->setObjectName(QStringLiteral("camera_z"));
    camera_z->setMaximumSize(QSize(100, SH_QWIDGETSIZE_MAX));
    UISET(SH_UI_camera_z, camera_z);
    horizontalLayout_3->addWidget(camera_z);

    QCheckBox *checkbox_lock_position = new QCheckBox(groupBox);
    checkbox_lock_position->setObjectName(QStringLiteral("checkbox_lock_position"));
    UISET(SH_UI_checkbox_lock_position, checkbox_lock_position);
    horizontalLayout_3->addWidget(checkbox_lock_position);

    verticalLayout_5->addWidget(groupBox);

    /* ---------------- window chrome ---------------- */
    window->setCentralWidget(centralWidget);

    QMenuBar *menuBar = new QMenuBar(window);
    menuBar->setObjectName(QStringLiteral("menuBar"));
    menuBar->setGeometry(QRect(0, 0, 1481, 20));   /* explicit uic-frozen geometry */
    UISET(SH_UI_menuBar, menuBar);
    window->setMenuBar(menuBar);

    QToolBar *mainToolBar = new QToolBar(window);
    mainToolBar->setObjectName(QStringLiteral("mainToolBar"));
    UISET(SH_UI_mainToolBar, mainToolBar);
    window->addToolBar(Qt::TopToolBarArea, mainToolBar);

    QStatusBar *statusBar = new QStatusBar(window);
    statusBar->setObjectName(QStringLiteral("statusBar"));
    UISET(SH_UI_statusBar, statusBar);
    window->setStatusBar(statusBar);

    /* OG tail: retranslateUi -> setCurrentIndex -> connectSlotsByName. DEVIATION from OG: OG opens on tab 1
     * ("Entity State"); the user wants the window to initialize on tab 0 ("Entities" -- the entity LIST). */
    sh_retranslateUi(window, win);
    tabWidget->setCurrentIndex(0);   /* "Entities" (the entity-list tab) -- user default (OG used 1) */
    sh_wire_stub_handlers(win);                /* stub signal graph (sets WIN[0] flag bits) */
    QMetaObject::connectSlotsByName(window);   /* faithful OG call (no auto-slots on the plain window) */
}

/* The FUN_180014e7c flag-word dispatch (+ the per-frame Entity-State read-sync) is now the REAL
 * implementation in sh_tabs.cpp (sh_dispatch_flagword). The stub skeleton that lived here is superseded;
 * the think-loop calls the sh_tabs.cpp body. */
