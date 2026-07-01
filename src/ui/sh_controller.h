/* sh_controller.h -- the WIN controller object + the setupUi port surface.
 *
 * A clean-room, FAITHFUL port of the OG snaphakui.dll window controller (`WIN`, OG `local_258`) and the
 * setupUi widget tree (OG FUN_18000cb6c @ RVA 0xcb6c, body 6889 bytes). RE-confirmed this session against
 * the OG decompiles (setupUi 0xcb6c + retranslateUi 0xe658). Zero OG bytes; our own C++ on Qt 5.9.
 *
 * THE Ui ARRAY (DIRECT, 0xcb6c): OG setupUi takes `param_1` = a pointer array; param_1[N] caches the Nth
 * widget by Ui index (the qFindChild slots). retranslateUi (0xe658) indexes the SAME array by byte offset
 * (param_1 + N*8). We mirror it as `ui[SH_UI_COUNT]` -- index N == OG param_1[N] == byte offset N*8.
 *
 * THE WIN[0] FLAG WORD (DIRECT, ui-window-shell.md): the per-frame dispatch flag-word OG holds at WIN[0].
 * setupUi's signal handlers set/clear its bits; the think-loop's FUN_180014e7c consumes them. setupUi wires the
 * stub handlers (set/clear the bits) + the FUN_180014e7c dispatch SKELETON (reads WIN[0], routes -- bodies
 * stubbed for later). NO real entity/decl/prefab/timeline logic here (filled in later); NO SnapStack ops yet.
 */
#ifndef SH_CONTROLLER_H
#define SH_CONTROLLER_H

#include <cstdint>
#include <string>
#include <vector>
#include <QObject>

class QApplication;
class QMainWindow;
class QTabWidget;
struct sh_iface;

/* ---- the WIN[0] flag-word bits (DIRECT, FUN_180014e7c map) --------------------------------------- */
enum ShWinFlag : uint64_t {
    SH_FLAG_REBUILD_LIST   = 0x01,  /* &1   rebuild entity list (Entities refresh.clicked) */
    SH_FLAG_APPLY_STATE    = 0x02,  /* &2   apply Entity-State edit to decl (Save.clicked) */
    SH_FLAG_REFILTER       = 0x08,  /* &8   refilter entity list (filter.textChanged) */
    SH_FLAG_COPY_TO_PREFAB = 0x10,  /* &0x10 copy selection -> prefab text */
    SH_FLAG_SUBMIT_MKCMD   = 0x20,  /* &0x20 submit mkcmd / prefab paste */
    SH_FLAG_WRITE_PREFAB   = 0x40,  /* &0x40 write prefab file (Prefabs create.clicked) */
    SH_FLAG_APPLY_TIMELINE = 0x80,  /* &0x80 apply timeline edit (save_entity_timeline) */
};

/* ---- the Ui index map (DIRECT from setupUi 0xcb6c; index == OG param_1[N]) ----------------------- */
enum ShUiIndex {
    SH_UI_centralWidget                 = 0x00,
    SH_UI_verticalLayout_5              = 0x01,
    SH_UI_tabWidget                     = 0x02,  /* QUIRK: objectName is "lua_scripts_page" */
    /* Tab 1 -- Entities */
    SH_UI_tab                           = 0x03,
    SH_UI_verticalLayout_4              = 0x04,
    SH_UI_hide_builtin_checkbox         = 0x05,
    SH_UI_widget_entity_list            = 0x06,
    SH_UI_line_edit_entity_filter       = 0x07,
    SH_UI_button_refresh_entity_list    = 0x08,
    /* Tab 2 -- Entity State */
    SH_UI_tab_3                         = 0x09,
    SH_UI_gridLayout                    = 0x0a,
    SH_UI_verticalLayout                = 0x0b,
    SH_UI_entity_text_editor            = 0x0c,
    SH_UI_horizontalLayout              = 0x0d,
    SH_UI_label_5                       = 0x0e,  /* "Inherit:" */
    SH_UI_entity_inherit_edit           = 0x0f,
    SH_UI_label_4                       = 0x10,  /* "Classname:" */
    SH_UI_entity_classname_edit         = 0x11,
    SH_UI_label_7                       = 0x12,  /* "Entity Displayname" */
    SH_UI_entity_displayname_lineedit   = 0x13,
    SH_UI_synchronize_checkbox          = 0x14,
    SH_UI_entity_id_lineedit            = 0x15,  /* readonly */
    SH_UI_button_save_state_to_decl     = 0x16,
    /* Tab 3 -- Prefabs */
    SH_UI_tab_4                         = 0x17,
    SH_UI_gridLayout_2                  = 0x18,
    SH_UI_prefab_listview               = 0x19,
    SH_UI_create_new_prefab             = 0x1a,
    /* Tab 4 -- Timelines */
    SH_UI_widget                        = 0x1b,
    SH_UI_gridLayout_3                  = 0x1c,
    SH_UI_timeline_list                 = 0x1d,
    SH_UI_button_create_new_timeline    = 0x1e,
    /* Tab 5 -- Timeline Editor */
    SH_UI_tab_8                         = 0x1f,
    SH_UI_gridLayout_4                  = 0x20,
    SH_UI_timeline_groupbox             = 0x21,  /* "Current Timeline" */
    SH_UI_verticalLayout_3              = 0x22,
    SH_UI_verticalLayout_2              = 0x23,
    SH_UI_insert_entity_event           = 0x24,
    SH_UI_save_entity_timeline          = 0x25,
    /* Tab 6 -- Editor Lua (EMPTY, faithful) */
    SH_UI_lua_page_tab_2                = 0x26,  /* objectName "tab_2"; empty */
    /* Camera Origin (NOT a tab) */
    SH_UI_groupBox                      = 0x27,  /* "Camera Origin" */
    SH_UI_horizontalLayout_3            = 0x28,
    SH_UI_widget_2                      = 0x29,
    SH_UI_label                         = 0x2a,  /* "X"; hardcoded setGeometry */
    SH_UI_camera_x                      = 0x2b,
    SH_UI_label_3                       = 0x2c,  /* "Z" */
    SH_UI_camera_y                      = 0x2d,
    SH_UI_label_2                       = 0x2e,  /* "Y" */
    SH_UI_camera_z                      = 0x2f,
    SH_UI_checkbox_lock_position        = 0x30,  /* "Lock Position" */
    /* Window chrome */
    SH_UI_menuBar                       = 0x31,
    SH_UI_mainToolBar                   = 0x32,
    SH_UI_statusBar                     = 0x33,
    /* CLONE EXTENSION (not in OG setupUi): the Entity-State INHERIT/CLASSNAME description box -- shows OUR
     * RE-extracted descriptions (sh_entity_desc.h) for the selected entity's inherit + class. */
    SH_UI_entity_desc_box               = 0x34,
    /* CLONE EXTENSION (not in OG setupUi): the Timelines-tab searchbar -- filters the Timelines list by name,
     * mirroring the Entities-tab line_edit_entity_filter. */
    SH_UI_line_edit_timeline_filter     = 0x35,
    SH_UI_COUNT                         = 0x36,
};

/* ---- the WIN controller (OG `local_258`) -------------------------------------------------------- */
/* The flag-word at WIN[0], the QApplication/QMainWindow/interface, plus the Ui pointer array (the
 * qFindChild'd widget pointers OG caches at param_1[N]). The bring-up shipped {flagword, app, window, iface}, and setupUi
 * adds the `ui[]` array + the live timeline TL ptr (OG WIN[0x3b], built in C3). */
struct ShWinController {
    uint64_t      flagword;            /* WIN[0]  the per-frame dispatch flag-word */
    QApplication *app;                 /* WIN[1] */
    QMainWindow  *window;              /* WIN[2] */
    void         *win3;                /* WIN[3]  (OG secondary controller field; unused) */
    sh_iface     *iface;              /* WIN[4]  the shared backend interface object */
    void         *ui[SH_UI_COUNT];     /* the qFindChild'd widget pointers (param_1[N]) */
    void         *timeline_tl;         /* OG WIN[0x3b] -- the live Timeline-Editor QTabWidget */

    /* ---- data-tab state (the data-tab consumers; OG holds these as WIN byte-offset members) ---------
     * The OG WIN tracks the synced/displayed entity id (WIN+0x54), a pending-select id (WIN+0x50), and
     * the Entity-State Save snapshot (the 4 idStr members WIN[0xc/0x10/0x14/0x18]). We mirror them by
     * name (the OG's read-sync repopulates the widgets from these; the |2 apply commits them). */
    int           displayed_id;       /* OG *(WIN+0x54): the entity id currently synced into Entity-State (-1=none) */
    int           pending_select_id;  /* OG WIN[0x50]/+0x50: a click-to-select id queued for the next frame (-1=none).
                                       * NOTE: also reused as the Timelines-dblclick carrier (placeholder). The
                                       * OG's real timeline-open target is the Timeline-Editor QTabWidget's TL+0x1d0
                                       * deferred-load slot (timeline_tl/WIN[0x3b]), NOT WIN+0x50 -- C3b rewires it there. */
    /* read-populate change-detect (the OG pending-id one-shot + WIN+0x1c decl cache; we mirror it so the
     * read-sync only repopulates a widget when the value actually changed -- not every frame). */
    int           last_synced_id = -1;   /* the id last pushed into the Entity-State widgets (-1=none) */
    std::string   last_synced_decl;      /* the decl-source last pushed into the decl box (OG WIN+0x1c) */
    std::string   last_synced_inherit;   /* the LIVE inherit last pushed into the combo -- read-sync overwrites only
                                          * on a real live change, NOT when the user picks a new value (no revert) */
    std::string   last_synced_classname; /* the LIVE classname last pushed into the combo -- same change-detect */
    std::string   last_synced_displayname; /* the LIVE displayname last pushed into the box -- read-sync overwrites
                                          * ONLY on a real live change, NOT when the user types a new name (else the
                                          * typed edit is clobbered on focus-out before Save can capture it). */
    std::string   last_desc_key;         /* CLONE EXTENSION: the (inherit\x1fclass) last rendered into the
                                          * Entity-Description box -- es_set_desc no-ops when unchanged. */
    std::string   last_class_combo_inherit; /* CLONE EXTENSION: the inherit the class dropdown was last
                                          * repopulated for -- sh_repopulate_class_combo no-ops if unchanged, so
                                          * the class isn't reset on a spurious editingFinished (dropdown open). */
    int           last_entity_count = -1;/* the entity_count last seen -- a change (map load / add / delete)
                                          * auto-raises |1 so the list populates without a manual Refresh. */
    int           spawn_rebuild_frames = 0; /* QOL: a from-scratch timeline SPAWN places the entity on the game
                                          * thread (deferred) + its className resolves a beat later, so the
                                          * one-shot count-poll can rebuild before the new entity reads as a
                                          * timeline. Set >0 on a spawn -> the dispatch re-scans a few times over
                                          * ~1.5s so the new timeline self-appears in the Timelines list. */
    /* Camera-Origin sync (OG WIN+0x20/+0x21 stored vec3 + WIN+8/+0x41/+0x42 per-axis dirty flags). */
    float         cam_xyz[3]   = {0.0f, 0.0f, 0.0f};    /* cached camera-origin vec3 (change-detect + Lock source) */
    bool          cam_dirty[3] = {false, false, false}; /* per-axis: the user committed an edit -> push to the editor */
    /* the Save-to-Decl click snapshot (FUN_180017d00 captured these from the 4 editable widgets). */
    std::string   save_decl_text;     /* OG WIN[0xc]  -- the QPlainTextEdit decl source */
    std::string   save_classname;     /* OG WIN[0x10] -- entity_classname_edit */
    std::string   save_inherit;       /* OG WIN[0x14] -- entity_inherit_edit */
    std::string   save_displayname;   /* OG WIN[0x18] -- entity_displayname_lineedit */
    /* the Prefabs state: the resolved create path (WIN+0x188), the serialized selection body (WIN+0x1a8),
     * and the loaded prefab file body (WIN[0x2d]/+0x168). */
    std::string   prefab_create_path; /* OG WIN[0x31]/+0x188: the create target file path */
    std::string   prefab_loaded_body; /* OG WIN[0x2d]/+0x168: the fread'd prefab json (the |0x20 paste source) */
};

/* ---- the setupUi port (FUN_18000cb6c) ----------------------------------------------------------- */
/* Build the EXACT 6-tab widget tree on `window`, caching every widget ptr into `win->ui[]`. 1482x944,
 * title "SnapHak Studio" (via retranslate), setCurrentIndex(1), connectSlotsByName. Installs a stub
 * ShController whose on_<objectName>_<signal> slots set/clear the WIN[0] flag bits (the real bodies
 * land later). Returns the central widget (already set on the window). */
void sh_setupUi(QMainWindow *window, ShWinController *win);

/* ---- the per-frame flag-word dispatch (FUN_180014e7c) -------------------------------------------- */
/* Reads win->flagword, routes each set bit to its consumer, clears the bit. Called from the think-loop
 * when the editor is ready. The data tabs fill the |1/|2/|8/|0x20/|0x40 bodies (the data tabs); the |0x80
 * (timeline) bit stays a C3b stub. ALSO runs the per-frame Entity-State read-sync (the OG repopulates the
 * tab widgets from the live selected entity every frame). */
void sh_dispatch_flagword(ShWinController *win);

/* ---- the 4 DATA-tab handlers (sh_tabs.cpp) --------------------------------------- */
/* ENTITIES tab: rebuild the entity list from the live editor (port FUN_1800147e8 in a count loop -- skip
 * NULL_/HideBuiltin id<=0x37 when checked/dual-add Timelines), applying the filter text. Clears + repopulates
 * widget_entity_list + timeline_list. Called by the |1 (rebuild) + |8 (refilter) flag consumers. */
void sh_rebuild_entity_list(ShWinController *win);

/* ENTITIES tab right-click ctx-menu (port FUN_180017384): Copy-ID (QClipboard), Delete (iface +0x130),
 * Push-to-stack-0 (the SnapStack psel-equivalent). `id` = the right-clicked entity id; `global_pos` = the
 * menu anchor. */
void sh_entity_context_menu(ShWinController *win, int id, const std::vector<int> &selected_ids,
                            const class QPoint &global_pos);

/* ENTITY-STATE tab: capture the 4 editable widgets into the Save snapshot + raise |2 (port FUN_180017d00).
 * Wired to button_save_state_to_decl.clicked. */
void sh_entity_state_save_clicked(ShWinController *win);

/* ENTITY-STATE tab: repopulate the class QComboBox with the engine-valid classes for `inherit` (the
 * linked dropdown). Driven by the inherit combo's commit (activated / editingFinished) + the read-sync. */
void sh_repopulate_class_combo(ShWinController *win, const class QString &inherit);

/* PREFABS tab: create (QInputDialog -> resolve path -> addItem -> raise |0x40; port FUN_180013c50) +
 * double-click load (fread the file -> raise |0x20; port FUN_180017538). */
void sh_prefab_create_clicked(ShWinController *win);
void sh_prefab_item_double_clicked(ShWinController *win, const class QString &stem);

/* PREFABS tab: re-scan %USERPROFILE%/snaphak/prefabs and repopulate the list (the OG ctor FUN_1800141a0
 * directory_iterator). Called once on init + after a create. */
void sh_prefab_list_populate(ShWinController *win);

/* TIMELINES tab double-click (port FUN_180017444): stash the selected timeline id into the timeline editor
 * (routes to tab 5, C3b). `id` = the double-clicked timeline entity id. */
void sh_timeline_item_double_clicked(ShWinController *win, int id);

/* ENTITIES tab double-click: open the Entity-State panel populated with that entity's state (sync off).
 * One-shot read of `id` into the Entity-State widgets (sets displayed_id + forces a repopulate). */
void sh_entity_state_load(ShWinController *win, int id);

#endif /* SH_CONTROLLER_H */
