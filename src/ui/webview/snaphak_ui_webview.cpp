/* snaphak_ui_webview.cpp -- PROOF-OF-CONCEPT frontend: a Qt-free snaphakui.dll that hosts the
 * "SnapHak Studio" UI as HTML in a Microsoft Edge WebView2 control instead of a Qt widget tree.
 *
 * Drop-in for the Qt snaphakui.dll: exports snaphak_ui_init (ord 10), writes the loop-state to
 * arg-block[0], caches the backend interface (arg-block[3]) and drains its work-queue (+0x1a0) at
 * ~30 Hz on this thread, keeps the 9 sl_* exports (../sl_exports.cpp). Zero DOOM/OG bytes.
 *
 * ITERATION 4 -- deeper Entities tab.
 *   - multi-select (JS), context menu: Copy ID (clipboard), Delete (+0x130 selection_guard),
 *     Push to stack 0 (honest stub -- SnapStack ops not ported to this UI yet).
 *   - auto-refresh: a cheap content signature over the walk; the list is re-emitted only when it
 *     changes (add/delete/rename/reclass/hide), so no needless re-renders.
 *   - synchronize with editor: when the checkbox is on, poll get_selection (+0x150); a single changed
 *     editor selection re-points the panel (mirrors the Qt sync, sh_tabs.cpp ~820).
 *   - state auto-refresh: the displayed entity's state is re-read on a signature and pushed as
 *     {auto:true}; the HTML applies it only when the edit panel is clean (never clobbers unsaved edits).
 *   - save is deferred + applied under the loop mutex (Qt apply_entity_state order); delete likewise.
 */
#include <windows.h>
#include <shlobj.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>
#include <utility>

#include "WebView2.h"
#include "snaphak_iface.h"
#include "mockup_html.h"
#include "../sh_entity_desc.h" /* GENERATED: OUR RE-extracted Inherit/Classname descriptions (same table sh_tabs.cpp uses) */
#include "../sh_event_catalog.h" /* GENERATED: OUR event-def catalog, 1611 events (same table sh_timeline.cpp uses) */
#include "../sh_entity_asset_lists.h" /* GENERATED: OUR per-entity-class model/anim asset lists (same table sh_timeline.cpp uses) */

using namespace Microsoft::WRL;

struct ShLoopState { CRITICAL_SECTION mtx; uint64_t flags; };

static ShLoopState *g_loop  = nullptr;
static sh_iface    *g_iface = nullptr;

static HWND                     g_hwnd         = nullptr;
static ICoreWebView2Controller *g_controller   = nullptr;
static ICoreWebView2           *g_webview      = nullptr;
static bool                     g_webview_ready = false;
static std::wstring             g_html;
static std::string              g_version = "dev";

static bool          g_sync_on       = false;   /* "Synchronize with editor" checkbox */
static int           g_displayed_eid = -1;      /* entity the state panel is showing */
static int           g_last_editor_sel = -1;    /* reverse-apply guard */
static uint64_t      g_last_list_sig  = 0;
static uint64_t      g_last_state_sig = 0;
static uint64_t      g_last_sel_sig   = 0;       /* forward-sync: last editor-selection signature */

static volatile bool g_pending_save = false;
static int           g_save_eid     = -1;
static std::string   g_save_decl, g_save_class, g_save_inherit, g_save_dname;
static int           g_save_result  = 0;

static volatile bool g_pending_delete = false;
static std::vector<int> g_delete_eids;

static volatile bool g_pending_select = false;   /* list -> editor selection push ("Select in editor") */
static std::vector<int> g_select_eids;
static volatile bool g_pending_deselect = false;  /* explicit "Deselect" button -- clear_selection escape hatch */
static char g_enumbuf[262144];                   /* packed-string scratch for enum_inherits / enum_valid_classes */

static volatile bool g_cam_lock = false;         /* Camera Origin "Lock Position" */
static float         g_cam_xyz[3] = {0.0f, 0.0f, 0.0f};
static volatile bool g_cam_write_once = false;   /* a committed field edit -> write the vec3 once */

static volatile bool g_pending_create_prefab = false;
static std::string   g_create_prefab_name;
static int           g_create_result = 0;        /* 1 ok; 0 empty editor selection; 2 not hovering an entity in selection; -1 resolve/serialize/write failure */
static int           g_last_selcount = -1;       /* last broadcast editor-selection count (Create-button gating) */

static volatile bool g_pending_delete_prefab = false;
static std::string   g_delete_prefab_name, g_delete_prefab_folder;   /* folder="" -> root (prefabs/) */
static int           g_delete_result = 0;        /* 1 ok; 0 DeleteFile failed (missing/locked); -1 resolve failed */

static volatile bool g_pending_rename_prefab = false;
static std::string   g_rename_prefab_old, g_rename_prefab_new, g_rename_prefab_folder;
static int           g_rename_result = 0;        /* 1 ok; 0 MoveFile failed (dest exists/missing/locked); -1 resolve failed */

static volatile bool g_pending_load_prefab = false;
static std::string   g_load_prefab_name, g_load_prefab_folder;
static int           g_load_result = 0;          /* 1 staged ok (now press Ctrl+V in the 3D view to place it);
                                                   * 0/-1 resolve/read/schedule failure (see log). Deliberately
                                                   * stage-only -- see backend-changes.md for why we don't try to
                                                   * automate the paste keystroke ourselves. */

/* Folders: one real level of subdirectories under %USERPROFILE%\snaphak\prefabs\ (no nested-within-nested).
 * folder="" always means the root prefabs\ dir. The folder/file IS the truth -- no separate manifest. */
static volatile bool g_pending_create_folder = false;
static std::string   g_create_folder_name;
static int           g_create_folder_result = 0;   /* 1 ok; 0 already exists / empty name; -1 CreateDirectory failed */

static volatile bool g_pending_rename_folder = false;
static std::string   g_rename_folder_old, g_rename_folder_new;
static int           g_rename_folder_result = 0;   /* 1 ok; 0 MoveFile failed (dest exists/locked); -1 resolve failed */

static volatile bool g_pending_delete_folder = false;
static std::string   g_delete_folder_name;
static int           g_delete_folder_result = 0;   /* 1 ok (removed, any contents moved to root); 0 RemoveDirectory failed
                                                     * (a name collision at root left a file behind); -1 resolve failed */

static volatile bool g_pending_move_prefab = false;
static std::string   g_move_prefab_name, g_move_prefab_from, g_move_prefab_to;
static int           g_move_prefab_result = 0;      /* 1 ok; 0 MoveFile failed (dest name collision); -1 resolve failed */

/* Timelines Stage 2: OPEN a timeline -> serialize the entity (+0xc8) and ship its raw JSON to the page, which
 * JSON.parses it and walks entityDef.state.edit.componentTimeLine/encounterComponent (same shape the Qt
 * tl_collect_from_decl parses). Serialize is an engine touch -> done in the think-loop drain under the loop
 * mutex, like the prefab ops. */
static volatile bool g_pending_open_timeline = false;
static int           g_open_timeline_eid = -1;
static int           g_tl_json_len = 0;             /* bytes serialized into g_tl_json this drain (0 = failed) */

/* Timelines Stage 3: resolve one entity's class (for the asset dropdowns) -- same deferred-to-drain shape as
 * opening a timeline, but a separate eid/len pair (see poc_serialize_entity_into's buffer-separation note). */
static volatile bool g_pending_resolve_entity = false;
static int           g_resolve_entity_eid = -1;
static int           g_resolve_json_len = 0;

/* Timelines Stage 5 (Save): the page ships the FULL patched entity JSON (fresh-reserialized by the page
 * itself via a plain openTimeline round-trip, then patched client-side -- see mockup.html's doSaveTimeline)
 * -- committed via apply_edit kind=0 (the SAME "deserialize patched_text -> commit" path the prefab
 * kind=1/mkcmd flow already uses in this file, just id-targeted instead of paste-targeted). */
static volatile bool g_pending_save_timeline = false;
static int           g_save_timeline_eid = -1;
static std::string   g_save_timeline_json;          /* UTF-8; the page's JSON.stringify output, already fully patched */
static int           g_save_timeline_result = 0;     /* 1 ok, 0 apply_edit refused/failed */

#define POC_MAX_ENTS 8192
#define POC_ID_CAP   384
#define POC_NAME_CAP 192
struct PocEnt { int eid; char id[POC_ID_CAP]; char name[POC_NAME_CAP]; int hidden; };
static PocEnt *g_ents = nullptr;

/* Timelines list: dual-added from the entity walk (OG quirk, sh_tabs.cpp populate_one_entity). Filled by
 * poc_rescan_timelines, which runs ONLY when the cheap entities signature changed (same cadence as the OG's
 * sh_rebuild_entity_list) -- never a fixed timer, and never touches classname on every poll. */
#define POC_MAX_TLS 2048
struct PocTl { int eid; char id[POC_ID_CAP]; char name[POC_NAME_CAP]; };
static PocTl *g_tls = nullptr;
static int    g_tl_count = 0;

/* ------------------------------------------------------------------ tiny file log ------------------ */
static void poc_log(const char *msg)
{
    CreateDirectoryA("snaphak_logs", nullptr);
    FILE *f = nullptr;
    if (fopen_s(&f, "snaphak_logs\\webview_poc.log", "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
        fclose(f);
    }
}
static void poc_logf(const char *fmt, unsigned long a) { char l[256]; _snprintf_s(l, sizeof l, _TRUNCATE, fmt, a); poc_log(l); }

/* ------------------------------------------------------------------ hashing (change signatures) ---- */
static uint64_t hstr(uint64_t h, const char *s) { while (*s) { h = (h ^ (unsigned char)*s) * 1099511628211ull; s++; } return h; }
static uint64_t hint(uint64_t h, int v) { for (int i = 0; i < 4; i++) { h = (h ^ (unsigned char)(v & 0xff)) * 1099511628211ull; v >>= 8; } return h; }

/* ------------------------------------------------------------------ string / JSON helpers ---------- */
static std::wstring poc_json_w(const char *utf8)
{
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w;
    if (wl > 0) { w.resize(wl - 1); if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wl); }
    std::wstring o; o.reserve(w.size() + 8);
    for (wchar_t c : w) {
        switch (c) {
        case L'\\': o += L"\\\\"; break; case L'"': o += L"\\\""; break;
        case L'\n': o += L"\\n"; break;  case L'\r': o += L"\\r"; break; case L'\t': o += L"\\t"; break;
        default: if (c < 0x20) { wchar_t b[8]; _snwprintf_s(b, _countof(b), _TRUNCATE, L"\\u%04x", (unsigned)c); o += b; } else o += c;
        }
    }
    return o;
}
static std::string w_to_utf8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s; s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
/* Path-safety gate (contributor-reported): resolve_prefab_path (+0xc0, backend) is a plain string concat
 * with no rejection of its own -- a name containing ".." or a path separator would resolve outside the
 * prefabs\ tree before ever reaching fopen/DeleteFileA/MoveFileA/CreateDirectoryA/RemoveDirectoryA. Only
 * the JS side guarded this before. Applied to every raw prefab/folder name component the UI supplies,
 * BEFORE it is concatenated into a prefix/filename and handed to resolve_prefab_path. */
static bool poc_valid_name(const std::string &n)
{
    if (n.empty() || n.size() > 200) return false;
    if (n.find("..") != std::string::npos) return false;
    if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos) return false;
    if (n.find(':') != std::string::npos) return false;
    return true;
}
static bool json_get_wstr(const std::wstring &j, const wchar_t *key, std::wstring &out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    if (p >= j.size() || j[p] != L'"') return false; p++;
    out.clear();
    while (p < j.size()) {
        wchar_t c = j[p++];
        if (c == L'"') break;
        if (c == L'\\' && p < j.size()) {
            wchar_t e = j[p++];
            switch (e) {
            case L'"': out += L'"'; break; case L'\\': out += L'\\'; break; case L'/': out += L'/'; break;
            case L'n': out += L'\n'; break; case L'r': out += L'\r'; break; case L't': out += L'\t'; break;
            case L'b': out += L'\b'; break; case L'f': out += L'\f'; break;
            case L'u': if (p + 4 <= j.size()) { wchar_t h[5] = {j[p],j[p+1],j[p+2],j[p+3],0}; out += (wchar_t)wcstoul(h, nullptr, 16); p += 4; } break;
            default: out += e; break;
            }
        } else out += c;
    }
    return true;
}
static bool json_get_int(const std::wstring &j, const wchar_t *key, int *out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    bool neg = false; if (p < j.size() && j[p] == L'-') { neg = true; p++; }
    if (p >= j.size() || j[p] < L'0' || j[p] > L'9') return false;
    long v = 0; while (p < j.size() && j[p] >= L'0' && j[p] <= L'9') { v = v * 10 + (j[p] - L'0'); p++; }
    *out = neg ? -(int)v : (int)v;
    return true;
}
static void json_get_intarray(const std::wstring &j, const wchar_t *key, std::vector<int> &out)
{
    out.clear();
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return;
    p += needle.size();
    while (p < j.size() && j[p] != L'[') p++;
    if (p >= j.size()) return; p++;
    while (p < j.size() && j[p] != L']') {
        while (p < j.size() && (j[p] == L' ' || j[p] == L',' || j[p] == L'\t')) p++;
        if (p >= j.size() || j[p] == L']') break;
        bool neg = false; if (j[p] == L'-') { neg = true; p++; }
        if (p >= j.size() || j[p] < L'0' || j[p] > L'9') break;
        long v = 0; while (p < j.size() && j[p] >= L'0' && j[p] <= L'9') { v = v * 10 + (j[p] - L'0'); p++; }
        out.push_back(neg ? -(int)v : (int)v);
    }
}

static bool json_get_double(const std::wstring &j, const wchar_t *key, double *out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    wchar_t *end = nullptr;
    double v = wcstod(j.c_str() + p, &end);
    if (end == j.c_str() + p) return false;
    *out = v;
    return true;
}

static void poc_read_version()
{
    char *la = nullptr; size_t n = 0;
    if (_dupenv_s(&la, &n, "LOCALAPPDATA") != 0 || !la) return;
    std::string path = std::string(la) + "\\open-snaphak\\install.json";
    free(la);
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
    std::string data; char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, r);
    fclose(f);
    size_t k = data.find("\"version\""); if (k == std::string::npos) return;
    k = data.find(':', k); if (k == std::string::npos) return;
    k = data.find('"', k);  if (k == std::string::npos) return; k++;
    size_t e = data.find('"', k); if (e == std::string::npos) return;
    g_version = data.substr(k, e - k);
}

/* ------------------------------------------------------------------ guarded engine reads/writes ---- */
static int poc_editor_ready()
{
    int r = 0;
    __try {
        if (g_iface && g_iface->vtbl) {
            if (g_iface->vtbl->editor_ready_poll) r = g_iface->vtbl->editor_ready_poll(g_iface) ? 1 : 0;
            else if (g_iface->vtbl->entity_count)  r = g_iface->vtbl->entity_count(g_iface) > 0 ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
static int poc_collect(int *out_ready)
{
    int n = 0; *out_ready = 0;
    __try {
        if (!g_iface || !g_iface->vtbl) return 0;
        if (g_iface->vtbl->editor_ready_poll) *out_ready = g_iface->vtbl->editor_ready_poll(g_iface) ? 1 : 0;
        int count = g_iface->vtbl->entity_count ? g_iface->vtbl->entity_count(g_iface) : 0;
        for (int id = 0; id < count && n < POC_MAX_ENTS; id++) {
            if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) continue;
            char idbuf[POC_ID_CAP]; idbuf[0] = 0; const char *s = idbuf;
            if (g_iface->vtbl->id_to_string) { const char *r = g_iface->vtbl->id_to_string(g_iface, id, idbuf, sizeof idbuf); if (r) s = r; }
            if (!s || s[0] == 0) continue;
            if (s[0]=='N' && s[1]=='U' && s[2]=='L' && s[3]=='L' && s[4]=='_') continue;
            char nmbuf[POC_NAME_CAP]; nmbuf[0] = 0; const char *nm = nmbuf;
            if (g_iface->vtbl->get_displayname) { const char *r = g_iface->vtbl->get_displayname(g_iface, id, nmbuf, sizeof nmbuf); if (r) nm = r; }
            int hidden = 0;
            if (g_iface->vtbl->id_dev_layer_hidden) hidden = g_iface->vtbl->id_dev_layer_hidden(g_iface, id) ? 1 : 0;
            g_ents[n].eid = id;
            strncpy_s(g_ents[n].id, POC_ID_CAP, s, _TRUNCATE);
            strncpy_s(g_ents[n].name, POC_NAME_CAP, nm ? nm : "", _TRUNCATE);
            g_ents[n].hidden = hidden;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return n;
}
/* Rescan g_ents[0..n) for Timelines and refill g_tls/g_tl_count. Called ONLY when the cheap entities-list
 * signature just changed (mirrors the OG sh_rebuild_entity_list cadence -- NOT a fixed timer). Reads the
 * classname of EVERY entity, exactly like the OG populate_one_entity (sh_tabs.cpp), and dual-adds any
 * idTarget_Timeline / idEncounterManager into the Timelines list (the OG quirk -- both classes, so encounter
 * managers show too). The classname read was live-debug-proven a clean pure-memory pointer walk
 * (ent->+0x158->+0x60), so it's cheap + safe; the per-call SEH guard keeps one bad entity from aborting the
 * rescan. (An earlier version pre-filtered on the id-string containing "unknown"/"placeholder_target" to save
 * calls, but that skipped idEncounterManager -- whose inherit is neither -- so it's dropped in favor of the
 * faithful "read every entity's class" behavior.) */
static void poc_rescan_timelines(int n)
{
    int tn = 0;
    if (!g_tls || !g_iface || !g_iface->vtbl || !g_iface->vtbl->get_classname_copy) { g_tl_count = 0; return; }
    for (int i = 0; i < n && tn < POC_MAX_TLS; i++) {
        if (g_ents[i].hidden) continue;   /* dev-layer hidden -> excluded from BOTH lists, matching the OG quirk */
        char clsbuf[128]; clsbuf[0] = 0;
        const char *c = NULL;
        __try {
            c = g_iface->vtbl->get_classname_copy(g_iface, g_ents[i].eid, clsbuf, sizeof clsbuf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            c = NULL;
            poc_logf("poc_rescan_timelines: get_classname_copy FAULTED for id=%lu (skipped)", (unsigned long)g_ents[i].eid);
        }
        if (c && (strcmp(c, "idTarget_Timeline") == 0 || strcmp(c, "idEncounterManager") == 0)) {
            g_tls[tn].eid = g_ents[i].eid;
            strncpy_s(g_tls[tn].id, POC_ID_CAP, g_ents[i].id, _TRUNCATE);
            strncpy_s(g_tls[tn].name, POC_NAME_CAP, g_ents[i].name, _TRUNCATE);
            tn++;
        }
    }
    g_tl_count = tn;
}
static bool poc_collect_state(int id, char *decl, int dcap, char *cls, int ccap, char *inh, int icap, char *dnm, int ncap)
{
    bool ok = false; decl[0] = cls[0] = inh[0] = dnm[0] = 0;
    __try {
        if (!g_iface || !g_iface->vtbl) return false;
        if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) return false;
        if (g_iface->vtbl->get_declsource_copy) { const char *r = g_iface->vtbl->get_declsource_copy(g_iface, id, decl, dcap); if (r && r != decl) strncpy_s(decl, dcap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_classname_copy)  { const char *r = g_iface->vtbl->get_classname_copy(g_iface, id, cls, ccap);  if (r && r != cls)  strncpy_s(cls,  ccap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_inherit_copy)    { const char *r = g_iface->vtbl->get_inherit_copy(g_iface, id, inh, icap);    if (r && r != inh)  strncpy_s(inh,  icap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_displayname)     { const char *r = g_iface->vtbl->get_displayname(g_iface, id, dnm, ncap);     if (r && r != dnm)  strncpy_s(dnm,  ncap, r, _TRUNCATE); }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}
static void poc_post_json(const wchar_t *json);   /* fwd */

/* Camera Origin: write the stored vec3 to the editor (+0x00). Used every frame while Lock is on, and once
 * per committed field edit. SEH-guarded. */
static void poc_cam_write()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->set_editor_vec3)
            g_iface->vtbl->set_editor_vec3(g_iface, g_cam_xyz);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* Camera Origin: read the live vec3 (+0x08); if it moved, cache + push it to the fields. SEH-guarded. */
static void poc_cam_read_send()
{
    float cam[3] = {0.0f, 0.0f, 0.0f};
    int ok = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->get_editor_vec3) { g_iface->vtbl->get_editor_vec3(g_iface, cam); ok = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) return;
    if (fabsf(cam[0]-g_cam_xyz[0]) < 1e-4f && fabsf(cam[1]-g_cam_xyz[1]) < 1e-4f && fabsf(cam[2]-g_cam_xyz[2]) < 1e-4f) return;
    g_cam_xyz[0] = cam[0]; g_cam_xyz[1] = cam[1]; g_cam_xyz[2] = cam[2];
    wchar_t m[192];
    _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"camera\",\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}", cam[0], cam[1], cam[2]);
    poc_post_json(m);
}

static int poc_get_selection(int *out, int max)
{
    int n = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->get_selection) {
            n = g_iface->vtbl->get_selection(g_iface, out, max);
            if (n < 0) n = 0;
            if (n > max) n = max;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}
static void poc_apply_save()
{
    g_save_result = -2;
    __try {
        if (!g_iface || !g_iface->vtbl) { g_save_result = -2; return; }
        int id = g_save_eid;
        if (id < 0) { g_save_result = -2; return; }
        if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) { g_save_result = -2; return; }
        int r = -1;
        if (g_iface->vtbl->apply_class_inherit) r = g_iface->vtbl->apply_class_inherit(g_iface, id, g_save_class.c_str(), g_save_inherit.c_str());
        if (r == -1) {
            if (g_iface->vtbl->set_classname) g_iface->vtbl->set_classname(g_iface, id, g_save_class.c_str());
            if (g_iface->vtbl->set_inherit)   g_iface->vtbl->set_inherit(g_iface, id, g_save_inherit.c_str());
        }
        /* BUGFIX (contributor-reported): displayname used to be written HERE, unconditionally -- so a REJECTED
         * class+inherit pair (r==0) still silently landed the displayname, a partial apply on a "failed" save.
         * Moved inside the r!=0 branch below so a refusal is fully atomic: nothing lands until the pair is
         * accepted. */
        if (r != 0) {
            if (g_iface->vtbl->rebuild_set_declsource) g_iface->vtbl->rebuild_set_declsource(g_iface, id, g_save_decl.c_str());
            int r2 = -1;
            if (g_iface->vtbl->apply_class_inherit) r2 = g_iface->vtbl->apply_class_inherit(g_iface, id, g_save_class.c_str(), g_save_inherit.c_str());
            if (r2 == -1) {
                if (g_iface->vtbl->set_classname) g_iface->vtbl->set_classname(g_iface, id, g_save_class.c_str());
                if (g_iface->vtbl->set_inherit)   g_iface->vtbl->set_inherit(g_iface, id, g_save_inherit.c_str());
            }
            if (g_iface->vtbl->set_entity_0x170) g_iface->vtbl->set_entity_0x170(g_iface, id, g_save_dname.c_str());
            g_save_result = 1;
        } else g_save_result = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_save_result = -2; }
}
static void poc_apply_deletes()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->selection_guard)
            for (size_t i = 0; i < g_delete_eids.size(); i++) {
                int id = g_delete_eids[i];
                if (id >= 0) g_iface->vtbl->selection_guard(g_iface, id);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* Timelines Stage 2: serialize an entity (+0xc8) into a caller-supplied buffer. SEH-guarded, no engine
 * exceptions escape. Returns bytes written (0 = failed / no slot / SEH). Shared by the timeline-open path
 * (g_tl_json below) and the entity-inherit-resolution path (Stage 3 asset dropdowns, g_resolve_json) -- two
 * SEPARATE buffers, not one shared one, so a resolve request mid-timeline-open can't clobber the open's data
 * if both land in the same think-loop drain. */
static int poc_serialize_entity_into(int id, char *buf, int cap)
{
    int n = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->serialize_entity && id >= 0)
            n = g_iface->vtbl->serialize_entity(g_iface, id, buf, cap - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    buf[n] = 0;
    return n;
}
/* Timelines Stage 2: serialize the timeline entity itself. Timeline entities are far smaller than prefabs,
 * but a 1 MB buffer is comfortably safe for a heavily-authored one. */
static char g_tl_json[1024 * 1024];
static int poc_serialize_entity_raw(int id) { return poc_serialize_entity_into(id, g_tl_json, (int)sizeof g_tl_json); }
/* Post {kind:"timelineData", eid, ok, json:"<the serialized entity JSON, escaped>"}. The page JSON.parses
 * `json` and walks entityDef.state.edit.componentTimeLine / encounterComponent itself (the entity JSON is
 * valid JSON -- the Qt side parses the identical bytes with QJsonDocument). */
static void poc_emit_timeline_data(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"timelineData\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += poc_json_w(g_tl_json); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* Timelines Stage 3 (per-entity asset dropdowns): resolve the CLASS (entityDef.inherit) of the "Runs on"
 * entity of an event-tab -- NOT the timeline entity itself. Reuses the same serialize_entity call, into its
 * OWN buffer (see poc_serialize_entity_into's comment). The page JSON.parses the result and reads .entityDef
 * .inherit itself (matches tl_entity_inherit_slug's "serialize + read one field" approach, but ships the
 * whole doc rather than adding a second raw-string field-scanner in C++ -- one parsing path, not two). */
static char g_resolve_json[1024 * 1024];
static int poc_serialize_entity_resolve(int id) { return poc_serialize_entity_into(id, g_resolve_json, (int)sizeof g_resolve_json); }
static void poc_emit_entity_inherit(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"entityInherit\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += poc_json_w(g_resolve_json); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* "Select in editor": drive the 3D editor selection from the list -- clear, then add each. (+0x148/+0x138)
 * BRACKETED LOGGING: a hang (not a crash -- the backend slots are SEH-guarded) leaves the last line on
 * disk (each poc_log flushes), so the log pinpoints exactly which call/ id froze the UI thread. */
static void poc_apply_select_in_editor()
{
    poc_logf("select-in-editor: apply start ids=%lu", (unsigned long)g_select_eids.size());
    __try {
        if (g_iface && g_iface->vtbl) {
            if (g_iface->vtbl->clear_selection) { poc_log("select-in-editor: clear"); g_iface->vtbl->clear_selection(g_iface); }
            if (g_iface->vtbl->add_to_selection)
                for (size_t i = 0; i < g_select_eids.size(); i++) {
                    int id = g_select_eids[i];
                    if (id < 0) continue;
                    if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) { poc_logf("select-in-editor: skip invalid id=%lu", (unsigned long)id); continue; }
                    poc_logf("select-in-editor: add id=%lu", (unsigned long)id);
                    g_iface->vtbl->add_to_selection(g_iface, id);
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { poc_log("select-in-editor: SEH in apply"); }
    poc_log("select-in-editor: apply done");
}
/* explicit Deselect: clear_selection only, no re-add. A reliable escape hatch since a native click on
 * empty space doesn't clear a selection that was set via add_to_selection (confirmed: native click/drag
 * selection deselects fine on its own -- only our externally-driven selection gets stuck). */
static void poc_apply_deselect()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) g_iface->vtbl->clear_selection(g_iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) { poc_log("deselect: SEH in apply"); }
}
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- this leaf has only
 * PODs in scope, so the SEH guard around the engine call is safe here. */
static int poc_serialize_selection_raw(char *buf, int cap)
{
    int n = 0;
    __try { n = g_iface->vtbl->serialize_selection(g_iface, buf, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}
/* Create-from-selection: resolve the file path (+0xc0), serialize the CURRENT editor selection (+0xb0,
 * same slot the Qt sh_prefab_create_clicked uses), fwrite it. g_create_result: 1 ok, 0 nothing was
 * selected (serialize returned empty), 2 not hovering an entity in the selection, -1 resolve/serialize/
 * write failure. Real prefabs on disk run up to ~370 KB (Sync Entities for Demons.json), so the scratch
 * buffer is generously sized at 4 MB.
 *
 * The hover check (+0x198) is a real, CONFIRMED (2026-07-06) engine requirement, not a UI nicety: the
 * engine's own PrefabPopulate refuses to run without a hovered entity in the selection (it prints
 * "Failed to create prefab: not hovering entity in selection." itself). Checking it here up front, before
 * ever touching serialize_selection, gives an accurate result code instead of the generic "nothing
 * selected" for what is actually a distinct failure. */
static void poc_apply_create_prefab()
{
    g_create_result = -1;
    { char l[300]; _snprintf_s(l, sizeof l, _TRUNCATE, "create-prefab: START name='%s'", g_create_prefab_name.c_str()); poc_log(l); }
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path || !g_iface->vtbl->serialize_selection || g_create_prefab_name.empty()) {
        poc_log("create-prefab: ABORT (iface/slot/name missing)");
        return;
    }
    if (g_iface->vtbl->hovered_id && g_iface->vtbl->hovered_id(g_iface) < 0) {
        poc_log("create-prefab: ABORT (not hovering an entity in the selection)");
        g_create_result = 2;
        return;
    }
    if (!poc_valid_name(g_create_prefab_name)) {
        poc_log("create-prefab: ABORT (name rejected -- '..' or path separator)");
        return;
    }
    char path[1024]; path[0] = '\0';
    std::string fname = g_create_prefab_name + ".json";
    if (!g_iface->vtbl->resolve_prefab_path(g_iface, "prefabs/", fname.c_str(), path, (int)sizeof path) || !path[0]) {
        poc_log("create-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    { char l[1200]; _snprintf_s(l, sizeof l, _TRUNCATE, "create-prefab: path='%s' -- about to serialize_selection (+0xb0)", path); poc_log(l); }
    static char buf[4 * 1024 * 1024];
    int n = poc_serialize_selection_raw(buf, (int)sizeof buf);
    poc_logf("create-prefab: serialize_selection returned n=%lu", (unsigned long)n);
    if (n <= 0) { g_create_result = 0; return; }
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) { poc_log("create-prefab: ABORT (fopen failed)"); return; }
    fwrite(buf, 1, (size_t)n, fp);
    fclose(fp);
    poc_log("create-prefab: WROTE file ok");
    g_create_result = 1;
}
/* folder="" -> the root prefabs\ dir; else prefabs\<folder>\ (one real level, no nesting). Shared by every
 * prefab/folder file op below so they all agree on where a folder actually lives on disk. */
static bool poc_prefab_dir(const std::string &folder, char *out, int cap)
{
    if (cap) out[0] = '\0';
    if (!folder.empty() && !poc_valid_name(folder)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    return g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), "", out, cap) && out[0] != '\0';
}
static bool poc_prefab_file_path(const std::string &folder, const std::string &name, char *out, int cap)
{
    if (cap) out[0] = '\0';
    if (!folder.empty() && !poc_valid_name(folder)) return false;
    if (!poc_valid_name(name)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    std::string fname = name + ".json";
    return g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), fname.c_str(), out, cap) && out[0] != '\0';
}
static void poc_strip_trailing_sep(char *s)
{
    size_t n = strlen(s);
    if (n && (s[n - 1] == '\\' || s[n - 1] == '/')) s[n - 1] = '\0';
}
/* list the *.json stems directly inside a resolved directory (non-recursive), sorted. */
static void poc_list_json_dir(const std::string &dirPath, std::vector<std::string> &names)
{
    std::string pattern = dirPath + "*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fn = fd.cFileName;
        if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".json") == 0) names.push_back(fn.substr(0, fn.size() - 5));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
}

/* Delete a prefab file: pure Win32 (no engine) -- resolve the path (+0xc0 is SHGetFolderPathA, no engine
 * touch) then DeleteFileA. result: 1 deleted, 0 DeleteFile failed (missing/locked), -1 resolve failed. */
static void poc_apply_delete_prefab()
{
    g_delete_result = -1;
    if (g_delete_prefab_name.empty()) return;
    char path[1024];
    if (!poc_prefab_file_path(g_delete_prefab_folder, g_delete_prefab_name, path, (int)sizeof path)) return;
    g_delete_result = DeleteFileA(path) ? 1 : 0;
}
/* Rename a prefab file WITHIN its current folder: MoveFileA old->new. MoveFileA refuses to overwrite an
 * existing destination (returns 0), so a name collision is a safe no-op the UI reports; the JS also
 * pre-checks. result: 1 ok, 0 MoveFile failed (dest exists / source missing / locked), -1 resolve failed. */
static void poc_apply_rename_prefab()
{
    g_rename_result = -1;
    if (g_rename_prefab_old.empty() || g_rename_prefab_new.empty()) return;
    char oldp[1024], newp[1024];
    if (!poc_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_old, oldp, (int)sizeof oldp)) return;
    if (!poc_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_new, newp, (int)sizeof newp)) return;
    g_rename_result = MoveFileA(oldp, newp) ? 1 : 0;
}
/* Load/Place: read the prefab file's raw JSON off disk, clear the current editor selection FIRST (so
 * nothing else is selected once the user pastes it), then schedule a kind=1 (mkcmd) apply item to stage
 * it into editor+0x209a8. Deliberately stage-only -- we tried automating the follow-up paste keystroke
 * (direct PasteInstantiate call, then a synthesized Ctrl+V) and both had real side effects (a crash, then
 * a spurious ESC-menu popup from the OS-level focus switch); see backend-changes.md. The user presses
 * Ctrl+V themselves in the 3D view to actually place it, matching the original SnapHak's own workflow
 * (confirmed via decompiling the original snaphakui.dll/XINPUT1_3.dll -- neither of them automates this
 * step either). result: 1 staged, 0/-1 resolve/read/schedule failure (see backend log). */
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- these leaves have
 * only PODs in scope, so the SEH guards around the engine calls are safe here. */
static void poc_clear_selection_seh()
{
    if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) {
        __try { g_iface->vtbl->clear_selection(g_iface); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static int poc_apply_edit_seh(const sh_apply_item *it, int count, const char *op)
{
    __try { return g_iface->vtbl->apply_edit(g_iface, it, count, op) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void poc_apply_load_prefab()
{
    g_load_result = -1;
    if (g_load_prefab_name.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) { poc_log("load-prefab: ABORT (iface/slot missing)"); return; }
    char path[1024];
    if (!poc_prefab_file_path(g_load_prefab_folder, g_load_prefab_name, path, (int)sizeof path)) {
        poc_log("load-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) { poc_log("load-prefab: ABORT (fopen failed)"); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::string body;
    if (sz > 0) { body.resize((size_t)sz); size_t got = fread(&body[0], 1, (size_t)sz, fp); body.resize(got); }
    fclose(fp);
    if (body.empty()) { poc_log("load-prefab: ABORT (empty file)"); return; }

    poc_clear_selection_seh();

    sh_apply_item it; it.kind = 1; it.id = 0; it.text = body.c_str();
    g_load_result = poc_apply_edit_seh(&it, 1, "load-prefab");
    char l[300]; _snprintf_s(l, sizeof l, _TRUNCATE, "load-prefab: name='%s' staged=%d", g_load_prefab_name.c_str(), g_load_result);
    poc_log(l);
}
/* Timelines Stage 5 (Save): kind=0 (deserialize the FULL patched entity JSON -> temp def -> commit
 * class/inherit/source on `id`) -- the SAME apply_edit path as kind=1 above, just id-targeted instead of
 * paste-targeted. The page already did the hard part (fresh-reserialize + JSON-patch the componentTimeLine/
 * encounterComponent field, matching Qt's tl_patch_component); this is purely "hand the opaque blob to the
 * engine and report whether it took." */
static void poc_apply_save_timeline()
{
    g_save_timeline_result = 0;
    if (g_save_timeline_eid < 0 || g_save_timeline_json.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) { poc_log("save-timeline: ABORT (iface/slot missing)"); return; }
    sh_apply_item it; it.kind = 0; it.id = g_save_timeline_eid; it.text = g_save_timeline_json.c_str();
    g_save_timeline_result = poc_apply_edit_seh(&it, 1, "save-timeline");
    char l[128]; _snprintf_s(l, sizeof l, _TRUNCATE, "save-timeline: eid=%d ok=%d", g_save_timeline_eid, g_save_timeline_result);
    poc_log(l);
}
/* Move a prefab from one folder to another (drag-and-drop target): MoveFileA across the two resolved
 * paths. Refuses to overwrite an existing same-named file at the destination (JS pre-checks too). */
static void poc_apply_move_prefab()
{
    g_move_prefab_result = -1;
    if (g_move_prefab_name.empty()) return;
    char oldp[1024], newp[1024];
    if (!poc_prefab_file_path(g_move_prefab_from, g_move_prefab_name, oldp, (int)sizeof oldp)) return;
    if (!poc_prefab_file_path(g_move_prefab_to, g_move_prefab_name, newp, (int)sizeof newp)) return;
    g_move_prefab_result = MoveFileA(oldp, newp) ? 1 : 0;
}
/* Create a real subdirectory under prefabs\. result: 1 created, 0 empty name / already exists, -1 failed. */
static void poc_apply_create_folder()
{
    g_create_folder_result = -1;
    if (g_create_folder_name.empty()) { g_create_folder_result = 0; return; }
    char dir[1024];
    if (!poc_prefab_dir(g_create_folder_name, dir, (int)sizeof dir)) return;
    poc_strip_trailing_sep(dir);
    if (CreateDirectoryA(dir, nullptr)) { g_create_folder_result = 1; return; }
    g_create_folder_result = (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}
/* Rename a folder: MoveFileA also renames directories. result: 1 ok, 0 dest exists/locked, -1 resolve failed. */
static void poc_apply_rename_folder()
{
    g_rename_folder_result = -1;
    if (g_rename_folder_old.empty() || g_rename_folder_new.empty()) return;
    char oldd[1024], newd[1024];
    if (!poc_prefab_dir(g_rename_folder_old, oldd, (int)sizeof oldd)) return;
    if (!poc_prefab_dir(g_rename_folder_new, newd, (int)sizeof newd)) return;
    poc_strip_trailing_sep(oldd); poc_strip_trailing_sep(newd);
    g_rename_folder_result = MoveFileA(oldd, newd) ? 1 : 0;
}
/* Delete a folder: move any remaining prefabs back to root first (a same-named file already at root is
 * left behind rather than silently overwritten -- that leaves the folder non-empty, so RemoveDirectory then
 * fails and reports it honestly instead of losing data), then RemoveDirectoryA. */
static void poc_apply_delete_folder()
{
    g_delete_folder_result = -1;
    if (g_delete_folder_name.empty()) return;
    char dir[1024], rootDir[1024];
    if (!poc_prefab_dir(g_delete_folder_name, dir, (int)sizeof dir)) return;
    if (!poc_prefab_dir("", rootDir, (int)sizeof rootDir)) return;
    std::vector<std::string> items;
    poc_list_json_dir(dir, items);
    for (size_t i = 0; i < items.size(); i++) {
        std::string src = std::string(dir) + items[i] + ".json";
        std::string dst = std::string(rootDir) + items[i] + ".json";
        MoveFileA(src.c_str(), dst.c_str());
    }
    poc_strip_trailing_sep(dir);
    g_delete_folder_result = RemoveDirectoryA(dir) ? 1 : 0;
}
/* Run enum_inherits (+0x278) or enum_valid_classes (+0x270) into buf; returns 1 + *pcount packed strings
 * (consecutive NUL-terminated, double-NUL end). SEH-guarded, no C++ objects. */
static int poc_run_enum(int classes, const char *inherit, char *buf, int cap, int *pcount)
{
    int r = 0; *pcount = 0; if (cap >= 2) { buf[0] = 0; buf[1] = 0; }
    __try {
        if (g_iface && g_iface->vtbl) {
            if (classes) { if (g_iface->vtbl->enum_valid_classes) r = g_iface->vtbl->enum_valid_classes(g_iface, inherit ? inherit : "", buf, cap, pcount); }
            else         { if (g_iface->vtbl->enum_inherits)      r = g_iface->vtbl->enum_inherits(g_iface, buf, cap, pcount); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; *pcount = 0; }
    return r;
}

/* ------------------------------------------------------------------ messages to the HTML ----------- */
static uint64_t poc_list_sig(int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h = hint(h, g_ents[i].eid); h = hstr(h, g_ents[i].id); h = hstr(h, g_ents[i].name); h = hint(h, g_ents[i].hidden); }
    return h ^ (uint64_t)n;
}
static void poc_emit_list(int n, int ready)
{
    if (!g_webview || !g_ents) return;
    std::wstring json; json.reserve((size_t)(n + g_tl_count) * 96 + 96);
    json += L"{\"kind\":\"list\",\"version\":\""; json += poc_json_w(g_version.c_str());
    json += L"\",\"editorReady\":"; json += ready ? L"true" : L"false";
    json += L",\"count\":"; json += std::to_wstring(n);
    json += L",\"entities\":[";
    for (int i = 0; i < n; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_ents[i].eid);
        json += L",\"id\":\""; json += poc_json_w(g_ents[i].id);
        json += L"\",\"name\":\""; json += poc_json_w(g_ents[i].name);
        json += L"\",\"hidden\":"; json += g_ents[i].hidden ? L"true" : L"false";
        json += L"}";
    }
    json += L"],\"timelines\":[";
    for (int i = 0; i < g_tl_count; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_tls[i].eid);
        json += L",\"id\":\""; json += poc_json_w(g_tls[i].id);
        json += L"\",\"name\":\""; json += poc_json_w(g_tls[i].name);
        json += L"\"}";   /* close the "name" string BEFORE the object brace (the missing \" was the empty-list bug) */
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void poc_send_list()
{
    int ready = 0; int n = poc_collect(&ready);
    uint64_t sig = poc_list_sig(n);
    /* the one get_classname_copy call site (the Timeline rescan) rides the SAME "did the cheap entities
     * signature change" gate as the OG's sh_rebuild_entity_list -- not a fixed timer. */
    if (sig != g_last_list_sig) poc_rescan_timelines(n);
    g_last_list_sig = sig;
    poc_emit_list(n, ready);
}
static void poc_send_state(int id, bool autoflag)
{
    if (!g_webview) return;
    static char decl[65536]; char cls[512], inh[512], dnm[512];
    bool ok = poc_collect_state(id, decl, sizeof decl, cls, sizeof cls, inh, sizeof inh, dnm, sizeof dnm);
    uint64_t sig = hstr(hstr(hstr(hstr(1469598103934665603ull, decl), cls), inh), dnm) ^ (uint64_t)id;
    if (autoflag && sig == g_last_state_sig) return;   /* nothing changed -> skip the auto push */
    g_last_state_sig = sig;
    std::wstring json = L"{\"kind\":\"state\",\"auto\":"; json += autoflag ? L"true" : L"false";
    json += L",\"eid\":"; json += std::to_wstring(id);
    json += L",\"ok\":"; json += ok ? L"true" : L"false";
    json += L",\"decl\":\"";        json += poc_json_w(decl); json += L"\"";
    json += L",\"classname\":\"";   json += poc_json_w(cls);  json += L"\"";
    json += L",\"inherit\":\"";     json += poc_json_w(inh);  json += L"\"";
    json += L",\"displayname\":\""; json += poc_json_w(dnm);  json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Build + send the valid-values list for a datalist: {kind:"inherits"|"classes", inherit?, items:[...]}. */
static void poc_send_enum(int classes, const char *inherit)
{
    if (!g_webview) return;
    int count = 0;
    poc_run_enum(classes, inherit, g_enumbuf, (int)sizeof g_enumbuf, &count);
    if (count < 0) count = 0;
    std::wstring json = classes ? L"{\"kind\":\"classes\",\"inherit\":\"" : L"{\"kind\":\"inherits\"";
    if (classes) { json += poc_json_w(inherit ? inherit : ""); json += L"\""; }
    json += L",\"items\":[";
    const char *p = g_enumbuf;
    const char *end = g_enumbuf + sizeof g_enumbuf;
    int emitted = 0;
    for (int i = 0; i < count && p < end; i++) {
        size_t len = strnlen(p, (size_t)(end - p));
        if (len > 0) {
            if (emitted) json += L",";
            std::string s(p, len);
            json += L"\""; json += poc_json_w(s.c_str()); json += L"\"";
            emitted++;
        }
        p += len + 1;
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Run enum_decls_of_resclass (+0x110) into buf. SEH-guarded, no C++ objects -- split out from
 * poc_send_arg_resclass below because MSVC (C2712) forbids __try in a function that also has a C++ object
 * (std::wstring/std::string) requiring unwind cooperation, same reason poc_run_enum is split from
 * poc_send_enum. */
static int poc_run_arg_resclass(const char *resClass, char *buf, int cap, int *pcount)
{
    int r = 0; *pcount = 0; if (cap >= 2) { buf[0] = 0; buf[1] = 0; }
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->enum_decls_of_resclass && resClass && resClass[0])
            r = g_iface->vtbl->enum_decls_of_resclass(g_iface, resClass, buf, cap, pcount);
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; *pcount = 0; }
    return r;
}
/* Timelines Stage 3 (DECL + ENUM arg widgets): the valid-values list for ONE decl resource-class or ONE
 * engine enum type, via the SAME shared +0x110 slot the Qt clone's tl_iface_enum_decls uses for both --
 * decl args pass the reduced short-name ("sound"), enum args pass the raw catalog type verbatim
 * ("fxCondition_t"). A shifted-build offset or unknown resClass degrades to an empty list (the combo then
 * falls back to a plain editable box, faithful to the OG/Qt behavior). */
static void poc_send_arg_resclass(const char *resClass)
{
    if (!g_webview) return;
    int count = 0;
    poc_run_arg_resclass(resClass, g_enumbuf, (int)sizeof g_enumbuf, &count);
    if (count < 0) count = 0;
    std::wstring json = L"{\"kind\":\"argResclass\",\"resClass\":\""; json += poc_json_w(resClass ? resClass : ""); json += L"\",\"items\":[";
    const char *p = g_enumbuf;
    const char *end = g_enumbuf + sizeof g_enumbuf;
    int emitted = 0;
    for (int i = 0; i < count && p < end; i++) {
        size_t len = strnlen(p, (size_t)(end - p));
        if (len > 0) {
            if (emitted) json += L",";
            std::string s(p, len);
            json += L"\""; json += poc_json_w(s.c_str()); json += L"\"";
            emitted++;
        }
        p += len + 1;
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* CLONE EXTENSION: the Entities-tab Inherit/Classname description panel -- same source table + name->desc
 * lookup as the Qt clone's es_lookup_desc (sh_tabs.cpp), reused here so both frontends read one canonical
 * generated table (sh_entity_desc.h) instead of duplicating the text. */
static const ShEntDesc *poc_lookup_desc(const std::string &name)
{
    static std::map<std::string, const ShEntDesc *> *idx = nullptr;
    if (!idx) {
        idx = new std::map<std::string, const ShEntDesc *>();
        for (int i = 0; i < SH_ENTITY_DESCS_N; i++) (*idx)[SH_ENTITY_DESCS[i].name] = &SH_ENTITY_DESCS[i];
    }
    if (name.empty()) return nullptr;
    auto it = idx->find(name);
    return it == idx->end() ? nullptr : it->second;
}
static void poc_json_desc_obj(std::wstring &json, const ShEntDesc *d)
{
    if (!d) { json += L"null"; return; }
    json += L"{\"summary\":\"";    json += poc_json_w(d->summary);    json += L"\"";
    json += L",\"confidence\":\""; json += poc_json_w(d->confidence); json += L"\"";
    json += L",\"source\":\"";     json += poc_json_w(d->source);     json += L"\"}";
}
static void poc_send_desc(const char *inherit, const char *classname)
{
    if (!g_webview) return;
    std::wstring json = L"{\"kind\":\"desc\",\"inherit\":";
    poc_json_desc_obj(json, poc_lookup_desc(inherit ? inherit : ""));
    json += L",\"class\":";
    poc_json_desc_obj(json, poc_lookup_desc(classname ? classname : ""));
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Timelines Stage 3: the full event-def catalog, sent ONCE per session (like enumInherits' full list) --
 * the page caches it and filters client-side, same "first N shown, type to narrow" Combo() convention as
 * Inherit/Classname. Each event carries its arg SCHEMA (name+type per position, matching sh_timeline.cpp's
 * ShEvtArg) -- not just the bare event name -- so the page can label each argument ("<name> (<type>)",
 * same convention as the Qt clone's tl_arg_label) and know how many args a freshly-picked event expects,
 * instead of only being able to describe args that already have a stored value. Static data
 * (sh_event_catalog.h), no engine touch. */
static void poc_send_events()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_EVENT_CATALOG_N * 96 + 32);
    json = L"{\"kind\":\"events\",\"items\":[";
    for (int i = 0; i < SH_EVENT_CATALOG_N; i++) {
        if (i) json += L",";
        const ShEvtDef &d = SH_EVENT_CATALOG[i];
        json += L"{\"name\":\""; json += poc_json_w(d.name); json += L"\",\"args\":[";
        for (int a = 0; a < d.argc; a++) {
            if (a) json += L",";
            const ShEvtArg &arg = d.args[a];
            json += L"{\"name\":\""; json += poc_json_w(arg.name ? arg.name : ""); json += L"\"";
            json += L",\"type\":\""; json += poc_json_w(arg.type ? arg.type : ""); json += L"\"}";
        }
        json += L"]}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void poc_json_asset_items(std::wstring &json, const ShAssetItem *items, int n)
{
    json += L"[";
    for (int i = 0; i < n; i++) {
        if (i) json += L",";
        json += L"{\"display\":\""; json += poc_json_w(items[i].display ? items[i].display : ""); json += L"\"";
        json += L",\"value\":\"";   json += poc_json_w(items[i].value   ? items[i].value   : ""); json += L"\"}";
    }
    json += L"]";
}
/* Timelines Stage 3 (per-entity asset dropdowns, "exceed-the-OG" -- the original Qt UI never had this,
 * it just made you type a raw model index or anim path): the full per-entity-class asset table, sent ONCE
 * per session, same "ship the canonical generated table, cache client-side" pattern as poc_send_events.
 * Only 45 entity classes carry asset lists (sh_entity_asset_lists.h), far smaller than the event catalog.
 * Static data, no engine touch -- resolving WHICH class applies to a given "Runs on" entity is the separate
 * poc_emit_entity_inherit round-trip above. */
static void poc_send_entity_assets()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_ENTITY_ASSETS_N * 512 + 32);
    json = L"{\"kind\":\"entityAssets\",\"items\":[";
    for (int i = 0; i < SH_ENTITY_ASSETS_N; i++) {
        if (i) json += L",";
        const ShEntityAssets &ea = SH_ENTITY_ASSETS[i];
        json += L"{\"slug\":\""; json += poc_json_w(ea.slug ? ea.slug : ""); json += L"\"";
        json += L",\"models\":";    poc_json_asset_items(json, ea.models,    ea.nModels);
        json += L",\"animWeb\":";   poc_json_asset_items(json, ea.animWeb,   ea.nAnimWeb);
        json += L",\"md6Anim\":";   poc_json_asset_items(json, ea.md6Anim,   ea.nMd6Anim);
        json += L",\"animAlias\":"; poc_json_asset_items(json, ea.animAlias, ea.nAnimAlias);
        json += L",\"tagName\":";   poc_json_asset_items(json, ea.tagName,   ea.nTagName);
        json += L"}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void poc_post_json(const wchar_t *json) { if (g_webview) g_webview->PostWebMessageAsJson(json); }

/* cheap targeted scan of a prefab JSON body (no full JSON parser, same "find key -> read quoted value"
 * approach as json_get_wstr): the entity count is the number of exact `"idSnapEntity"` tokens (each entity's
 * own "~type"; the prefab's OWN root "~type" is "idSnapEntityPrefab" -- a different exact token, so this
 * can't double-count it). The per-class tally scans every `"className"` key (only entityDef objects have
 * one) and groups by value. */
static void poc_tally_prefab(const std::string &body, int *entityCount, std::vector<std::pair<std::string, int>> &tally)
{
    *entityCount = 0;
    size_t pos = 0;
    while ((pos = body.find("\"idSnapEntity\"", pos)) != std::string::npos) { (*entityCount)++; pos += 14; }

    std::map<std::string, int> counts;
    const std::string key = "\"className\"";
    pos = 0;
    while ((pos = body.find(key, pos)) != std::string::npos) {
        size_t p = pos + key.size();
        size_t colon = body.find(':', p);
        if (colon == std::string::npos) break;
        size_t q1 = body.find('"', colon + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        counts[body.substr(q1 + 1, q2 - q1 - 1)]++;
        pos = q2 + 1;
    }
    for (auto &kv : counts) tally.push_back(kv);
}
/* read a single prefab file (resolved via +0xc0) and push its entity count + per-class tally so the
 * Prefabs tab detail pane can show real numbers instead of the old static mockup values. folder="" -> root. */
static void poc_send_prefab_detail(const std::string &name, const std::string &folder)
{
    if (!g_webview) return;
    int entityCount = 0;
    std::vector<std::pair<std::string, int>> tally;
    bool ok = false;
    if (!name.empty()) {
        char path[1024];
        if (poc_prefab_file_path(folder, name, path, (int)sizeof path)) {
            FILE *fp = nullptr;
            if (fopen_s(&fp, path, "rb") == 0 && fp) {
                fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                if (sz > 0) {
                    std::string body; body.resize((size_t)sz);
                    size_t got = fread(&body[0], 1, (size_t)sz, fp);
                    body.resize(got);
                    poc_tally_prefab(body, &entityCount, tally);
                    ok = true;
                }
                fclose(fp);
            }
        }
    }
    std::wstring json = L"{\"kind\":\"prefabDetail\",\"name\":\""; json += poc_json_w(name.c_str());
    json += L"\",\"ok\":"; json += ok ? L"true" : L"false";
    json += L",\"count\":"; json += std::to_wstring(entityCount);
    json += L",\"types\":[";
    for (size_t i = 0; i < tally.size(); i++) {
        if (i) json += L",";
        json += L"{\"className\":\""; json += poc_json_w(tally[i].first.c_str());
        json += L"\",\"count\":"; json += std::to_wstring(tally[i].second); json += L"}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* enumerate %USERPROFILE%\snaphak\prefabs\ (resolved via the +0xc0 interface slot, same path the OG/Qt
 * Prefabs tab lists -- see sh_tabs.cpp sh_prefab_list_populate): the root *.json files, plus one real level
 * of subdirectories (each a "folder"), each listing its own *.json files. No nested-within-nested. Empty
 * (or missing dir) sends empty arrays so the UI can show its empty state. */
static void poc_send_prefabs()
{
    if (!g_webview) return;
    std::vector<std::string> rootNames;
    std::vector<std::pair<std::string, std::vector<std::string>>> folders;
    char rootDir[1024];
    if (poc_prefab_dir("", rootDir, (int)sizeof rootDir)) {
        poc_list_json_dir(rootDir, rootNames);

        std::vector<std::string> subdirs;
        std::string pattern = std::string(rootDir) + "*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string dn = fd.cFileName;
                if (dn == "." || dn == "..") continue;
                subdirs.push_back(dn);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (size_t i = 0; i < subdirs.size(); i++) {
            std::vector<std::string> items;
            poc_list_json_dir(std::string(rootDir) + subdirs[i] + "\\", items);
            folders.push_back(std::make_pair(subdirs[i], items));
        }
    }
    std::wstring json = L"{\"kind\":\"prefabs\",\"root\":[";
    for (size_t i = 0; i < rootNames.size(); i++) {
        if (i) json += L",";
        json += L"\""; json += poc_json_w(rootNames[i].c_str()); json += L"\"";
    }
    json += L"],\"folders\":[";
    for (size_t i = 0; i < folders.size(); i++) {
        if (i) json += L",";
        json += L"{\"name\":\""; json += poc_json_w(folders[i].first.c_str()); json += L"\",\"items\":[";
        const std::vector<std::string> &items = folders[i].second;
        for (size_t j = 0; j < items.size(); j++) {
            if (j) json += L",";
            json += L"\""; json += poc_json_w(items[j].c_str()); json += L"\"";
        }
        json += L"]}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* ------------------------------------------------------------------ window / WebView2 -------------- */
static LRESULT CALLBACK PocWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: if (g_controller) { RECT rc; GetClientRect(hwnd, &rc); g_controller->put_Bounds(rc); } return 0;
    case WM_CLOSE: return 0;   /* inert: don't let the user close the UI while in the editor (would need a map reload) */
    case WM_NCCALCSIZE:
        /* frameless: consume the non-client area so the client (WebView2) fills the window and the native
         * title bar is gone. WS_THICKFRAME stays so Aero Snap + maximize work; the default maximize sizing
         * already respects the taskbar. When MAXIMIZED, Windows adds the resize-frame overhang beyond the
         * work area, so inset the client by that frame or the top/bottom clip off-screen. */
        if (wp) {
            if (IsZoomed(hwnd)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top  += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);   /* MUST be W: ANSI DefWindowProcA truncates the wide caption to "S" */
}
static void poc_create_window()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = PocWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"SnapHakStudioWebView";
    wc.style = CS_NOCLOSE;   /* remove the native close (X) button -- can't close the UI from the editor */
    RegisterClassExW(&wc);
    /* default size big enough to show the Entities list + state editor (or the Prefabs folder tree + card)
     * side by side with no clipping on a typical 1080p+ display, without needing a manual resize on first
     * launch. Fits comfortably within 1920x1080 with room for the taskbar. */
    g_hwnd = CreateWindowExW(0, L"SnapHakStudioWebView", L"Snapmap+",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    /* force a frame recalculation now so WM_NCCALCSIZE strips the title bar BEFORE the window is first
     * shown -- otherwise the native frame lingers until the first resize/move. */
    if (g_hwnd)
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    poc_logf("window created (hwnd=%lu)", (unsigned long)(uintptr_t)g_hwnd);
}
static HRESULT on_message(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR jp = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&jp)) && jp) {
        std::wstring json(jp); CoTaskMemFree(jp);
        std::wstring cmd;
        if (json_get_wstr(json, L"cmd", cmd)) {
            if (cmd == L"refresh") {
                poc_send_list();
            } else if (cmd == L"listPrefabs") {
                poc_send_prefabs();
            } else if (cmd == L"selectPrefab") {
                std::wstring nm, fo; json_get_wstr(json, L"name", nm); json_get_wstr(json, L"folder", fo);
                poc_send_prefab_detail(w_to_utf8(nm), w_to_utf8(fo));
            } else if (cmd == L"createPrefab") {
                std::wstring nm; json_get_wstr(json, L"name", nm);
                g_create_prefab_name = w_to_utf8(nm);
                g_pending_create_prefab = true;
            } else if (cmd == L"deletePrefab") {
                std::wstring nm, fo; json_get_wstr(json, L"name", nm); json_get_wstr(json, L"folder", fo);
                g_delete_prefab_name = w_to_utf8(nm); g_delete_prefab_folder = w_to_utf8(fo);
                g_pending_delete_prefab = true;
            } else if (cmd == L"loadPrefab") {
                std::wstring nm, fo; json_get_wstr(json, L"name", nm); json_get_wstr(json, L"folder", fo);
                g_load_prefab_name = w_to_utf8(nm); g_load_prefab_folder = w_to_utf8(fo);
                g_pending_load_prefab = true;
            } else if (cmd == L"renamePrefab") {
                std::wstring o, nn, fo; json_get_wstr(json, L"oldName", o); json_get_wstr(json, L"newName", nn); json_get_wstr(json, L"folder", fo);
                g_rename_prefab_old = w_to_utf8(o); g_rename_prefab_new = w_to_utf8(nn); g_rename_prefab_folder = w_to_utf8(fo);
                g_pending_rename_prefab = true;
            } else if (cmd == L"createFolder") {
                std::wstring nm; json_get_wstr(json, L"name", nm);
                g_create_folder_name = w_to_utf8(nm);
                g_pending_create_folder = true;
            } else if (cmd == L"renameFolder") {
                std::wstring o, nn; json_get_wstr(json, L"oldName", o); json_get_wstr(json, L"newName", nn);
                g_rename_folder_old = w_to_utf8(o); g_rename_folder_new = w_to_utf8(nn);
                g_pending_rename_folder = true;
            } else if (cmd == L"deleteFolder") {
                std::wstring nm; json_get_wstr(json, L"name", nm);
                g_delete_folder_name = w_to_utf8(nm);
                g_pending_delete_folder = true;
            } else if (cmd == L"movePrefabToFolder") {
                std::wstring nm, fromf, tof; json_get_wstr(json, L"name", nm); json_get_wstr(json, L"fromFolder", fromf); json_get_wstr(json, L"toFolder", tof);
                g_move_prefab_name = w_to_utf8(nm); g_move_prefab_from = w_to_utf8(fromf); g_move_prefab_to = w_to_utf8(tof);
                g_pending_move_prefab = true;
            } else if (cmd == L"select") {
                int eid = -1;
                if (json_get_int(json, L"eid", &eid)) { g_displayed_eid = eid; poc_send_state(eid, false); }
            } else if (cmd == L"openTimeline") {
                int eid = -1;
                if (json_get_int(json, L"eid", &eid) && eid >= 0) { g_open_timeline_eid = eid; g_pending_open_timeline = true; }
            } else if (cmd == L"resolveEntityInherit") {
                int eid = -1;
                if (json_get_int(json, L"eid", &eid) && eid >= 0) { g_resolve_entity_eid = eid; g_pending_resolve_entity = true; }
            } else if (cmd == L"saveTimeline") {
                int eid = -1; std::wstring j;
                if (json_get_int(json, L"eid", &eid) && eid >= 0 && json_get_wstr(json, L"json", j) && !j.empty()) {
                    g_save_timeline_eid = eid; g_save_timeline_json = w_to_utf8(j); g_pending_save_timeline = true;
                }
            } else if (cmd == L"setSync") {
                int on = 0; json_get_int(json, L"on", &on); g_sync_on = (on != 0); g_last_editor_sel = -1; g_last_sel_sig = 0;
            } else if (cmd == L"delete") {
                json_get_intarray(json, L"eids", g_delete_eids);
                if (!g_delete_eids.empty()) g_pending_delete = true;
            } else if (cmd == L"selectInEditor") {
                json_get_intarray(json, L"eids", g_select_eids);
                g_pending_select = true;   /* applied under the loop mutex */
            } else if (cmd == L"deselect") {
                g_pending_deselect = true;
            } else if (cmd == L"enumInherits") {
                poc_send_enum(0, nullptr);
            } else if (cmd == L"enumClasses") {
                std::wstring inh; json_get_wstr(json, L"inherit", inh);
                std::string i8 = w_to_utf8(inh);
                poc_send_enum(1, i8.c_str());
            } else if (cmd == L"enumEvents") {
                poc_send_events();
            } else if (cmd == L"enumEntityAssets") {
                poc_send_entity_assets();
            } else if (cmd == L"enumArgResclass") {
                std::wstring rc; json_get_wstr(json, L"resClass", rc);
                std::string rc8 = w_to_utf8(rc);
                poc_send_arg_resclass(rc8.c_str());
            } else if (cmd == L"lookupDesc") {
                std::wstring inh, cls; json_get_wstr(json, L"inherit", inh); json_get_wstr(json, L"classname", cls);
                std::string i8 = w_to_utf8(inh), c8 = w_to_utf8(cls);
                poc_send_desc(i8.c_str(), c8.c_str());
            } else if (cmd == L"camLock") {
                int on = 0; json_get_int(json, L"on", &on);
                g_cam_lock = (on != 0);
                if (g_cam_lock) {   /* freeze to the current field values */
                    double x, y, z;
                    if (json_get_double(json, L"x", &x)) g_cam_xyz[0] = (float)x;
                    if (json_get_double(json, L"y", &y)) g_cam_xyz[1] = (float)y;
                    if (json_get_double(json, L"z", &z)) g_cam_xyz[2] = (float)z;
                }
            } else if (cmd == L"camSet") {
                double x, y, z;
                if (json_get_double(json, L"x", &x)) g_cam_xyz[0] = (float)x;
                if (json_get_double(json, L"y", &y)) g_cam_xyz[1] = (float)y;
                if (json_get_double(json, L"z", &z)) g_cam_xyz[2] = (float)z;
                g_cam_write_once = true;
            } else if (cmd == L"winMin") {
                ShowWindow(g_hwnd, SW_MINIMIZE);
            } else if (cmd == L"winMax") {
                ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            } else if (cmd == L"winDrag") {
                ReleaseCapture();
                SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);   /* start the native move loop */
            } else if (cmd == L"winResize") {
                std::wstring dir; json_get_wstr(json, L"dir", dir);
                WPARAM ht = 0;
                if      (dir == L"l")  ht = HTLEFT;      else if (dir == L"r")  ht = HTRIGHT;
                else if (dir == L"t")  ht = HTTOP;       else if (dir == L"b")  ht = HTBOTTOM;
                else if (dir == L"tl") ht = HTTOPLEFT;   else if (dir == L"tr") ht = HTTOPRIGHT;
                else if (dir == L"bl") ht = HTBOTTOMLEFT;else if (dir == L"br") ht = HTBOTTOMRIGHT;
                if (ht) { ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, ht, 0); }
            } else if (cmd == L"pushStack") {
                std::vector<int> ids; json_get_intarray(json, L"eids", ids);
                wchar_t m[160];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                    L"{\"kind\":\"info\",\"text\":\"Push to stack 0: %d selected -- SnapStack ops not ported to this UI yet.\"}", (int)ids.size());
                poc_post_json(m);
            } else if (cmd == L"save") {
                int eid = -1; json_get_int(json, L"eid", &eid);
                std::wstring decl, cls, inh, dnm;
                json_get_wstr(json, L"decl", decl); json_get_wstr(json, L"classname", cls);
                json_get_wstr(json, L"inherit", inh); json_get_wstr(json, L"displayname", dnm);
                g_save_eid = eid; g_save_decl = w_to_utf8(decl); g_save_class = w_to_utf8(cls);
                g_save_inherit = w_to_utf8(inh); g_save_dname = w_to_utf8(dnm);
                g_pending_save = true;
            }
        }
    }
    return S_OK;
}
static HRESULT on_nav_completed(ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) { poc_send_list(); return S_OK; }
static HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller *controller)
{
    if (FAILED(result) || !controller) { poc_logf("controller creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    g_controller = controller; g_controller->AddRef();
    g_controller->get_CoreWebView2(&g_webview);
    if (!g_webview) { poc_log("get_CoreWebView2 null"); return E_FAIL; }
    { /* disable WebView2's default (browser) right-click menu app-wide; our own menus are HTML. */
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->Release();
        }
    }
    RECT rc; GetClientRect(g_hwnd, &rc); g_controller->put_Bounds(rc);
    EventRegistrationToken tok;
    g_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(on_message).Get(), &tok);
    g_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(on_nav_completed).Get(), &tok);
    g_webview->NavigateToString(g_html.c_str());
    g_controller->put_IsVisible(TRUE);
    g_webview_ready = true;
    poc_log("controller ready: navigated to embedded HTML");
    return S_OK;
}
static HRESULT on_environment_created(HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env) { poc_logf("environment creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    return env->CreateCoreWebView2Controller(g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(on_controller_created).Get());
}
static void poc_start_webview()
{
    wchar_t local[MAX_PATH] = {}; std::wstring udf = L".";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        udf = std::wstring(local) + L"\\open-snaphak\\webview2"; SHCreateDirectoryExW(nullptr, udf.c_str(), nullptr);
    }
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(on_environment_created).Get());
    if (FAILED(hr)) poc_logf("Create env FAILED hr=0x%08lx", (unsigned long)hr);
}

/* ------------------------------------------------------------------ the 30 Hz think-loop ----------- */
static void poc_think_loop()
{
    bool was_visible = false;
    unsigned frame = 0;
    for (;;) {
        frame++;
        bool did_save = false, did_delete = false, did_create_prefab = false;
        bool did_delete_prefab = false, did_rename_prefab = false, did_load_prefab = false;
        bool did_create_folder = false, did_rename_folder = false, did_delete_folder = false, did_move_prefab = false;
        bool did_open_timeline = false, did_resolve_entity = false, did_save_timeline = false;
        EnterCriticalSection(&g_loop->mtx);
        if (g_iface && g_iface->vtbl && g_iface->vtbl->drain_work_queue) g_iface->vtbl->drain_work_queue(g_iface);
        if (g_pending_save)   { poc_apply_save();    g_pending_save = false;   did_save = true; }
        if (g_pending_delete) { poc_apply_deletes(); g_delete_eids.clear(); g_pending_delete = false; did_delete = true; }
        if (g_pending_select) {
            poc_apply_select_in_editor();
            /* keep forward-sync (editor->list) quiet about the selection WE just pushed (avoid a ping-pong). */
            g_last_editor_sel = (g_select_eids.size() == 1) ? g_select_eids[0] : -1;
            if (g_select_eids.size() == 1) g_displayed_eid = g_select_eids[0];
            g_select_eids.clear(); g_pending_select = false;
        }
        if (g_pending_deselect) {
            poc_apply_deselect();
            g_last_editor_sel = -1; g_last_sel_sig = 0;
            g_pending_deselect = false;
        }
        if (g_pending_create_prefab) {
            poc_apply_create_prefab();
            g_pending_create_prefab = false;
            did_create_prefab = true;
        }
        if (g_pending_delete_prefab) { poc_apply_delete_prefab(); g_pending_delete_prefab = false; did_delete_prefab = true; }
        if (g_pending_rename_prefab) { poc_apply_rename_prefab(); g_pending_rename_prefab = false; did_rename_prefab = true; }
        if (g_pending_load_prefab)   { poc_apply_load_prefab();   g_pending_load_prefab = false;   did_load_prefab = true; }
        if (g_pending_create_folder) { poc_apply_create_folder(); g_pending_create_folder = false; did_create_folder = true; }
        if (g_pending_rename_folder) { poc_apply_rename_folder(); g_pending_rename_folder = false; did_rename_folder = true; }
        if (g_pending_delete_folder) { poc_apply_delete_folder(); g_pending_delete_folder = false; did_delete_folder = true; }
        if (g_pending_move_prefab)   { poc_apply_move_prefab();   g_pending_move_prefab = false;   did_move_prefab = true; }
        if (g_pending_open_timeline) { g_tl_json_len = poc_serialize_entity_raw(g_open_timeline_eid); g_pending_open_timeline = false; did_open_timeline = true; }
        if (g_pending_resolve_entity) { g_resolve_json_len = poc_serialize_entity_resolve(g_resolve_entity_eid); g_pending_resolve_entity = false; did_resolve_entity = true; }
        if (g_pending_save_timeline) { poc_apply_save_timeline(); g_pending_save_timeline = false; did_save_timeline = true; }
        /* Camera Origin: hold the locked origin every frame (or flush one committed edit). */
        if (g_cam_lock || g_cam_write_once) { poc_cam_write(); g_cam_write_once = false; }
        LeaveCriticalSection(&g_loop->mtx);

        if (did_save) {
            wchar_t m[80]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"saveResult\",\"result\":%d}", g_save_result);
            poc_post_json(m);
            poc_send_list();
            if (g_save_eid >= 0) poc_send_state(g_save_eid, false);
        }
        if (did_delete) poc_send_list();
        if (did_create_prefab) {
            std::wstring m = L"{\"kind\":\"createPrefabResult\",\"result\":"; m += std::to_wstring(g_create_result);
            m += L",\"name\":\""; m += poc_json_w(g_create_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_create_result == 1) poc_send_prefabs();
        }
        if (did_delete_prefab) {
            std::wstring m = L"{\"kind\":\"deletePrefabResult\",\"result\":"; m += std::to_wstring(g_delete_result);
            m += L",\"name\":\""; m += poc_json_w(g_delete_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_delete_result == 1) poc_send_prefabs();
        }
        if (did_rename_prefab) {
            std::wstring m = L"{\"kind\":\"renamePrefabResult\",\"result\":"; m += std::to_wstring(g_rename_result);
            m += L",\"oldName\":\""; m += poc_json_w(g_rename_prefab_old.c_str());
            m += L"\",\"newName\":\""; m += poc_json_w(g_rename_prefab_new.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_rename_result == 1) poc_send_prefabs();
        }
        if (did_load_prefab) {
            std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":"; m += std::to_wstring(g_load_result);
            m += L",\"name\":\""; m += poc_json_w(g_load_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
        }
        if (did_open_timeline) poc_emit_timeline_data(g_open_timeline_eid, g_tl_json_len);
        if (did_resolve_entity) poc_emit_entity_inherit(g_resolve_entity_eid, g_resolve_json_len);
        if (did_save_timeline) {
            std::wstring m = L"{\"kind\":\"saveTimelineResult\",\"eid\":"; m += std::to_wstring(g_save_timeline_eid);
            m += L",\"result\":"; m += std::to_wstring(g_save_timeline_result); m += L"}";
            poc_post_json(m.c_str());
        }
        if (did_create_folder) {
            std::wstring m = L"{\"kind\":\"createFolderResult\",\"result\":"; m += std::to_wstring(g_create_folder_result);
            m += L",\"name\":\""; m += poc_json_w(g_create_folder_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_create_folder_result == 1) poc_send_prefabs();
        }
        if (did_rename_folder) {
            std::wstring m = L"{\"kind\":\"renameFolderResult\",\"result\":"; m += std::to_wstring(g_rename_folder_result);
            m += L",\"oldName\":\""; m += poc_json_w(g_rename_folder_old.c_str());
            m += L"\",\"newName\":\""; m += poc_json_w(g_rename_folder_new.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_rename_folder_result == 1) poc_send_prefabs();
        }
        if (did_delete_folder) {
            std::wstring m = L"{\"kind\":\"deleteFolderResult\",\"result\":"; m += std::to_wstring(g_delete_folder_result);
            m += L",\"name\":\""; m += poc_json_w(g_delete_folder_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_delete_folder_result == 1) poc_send_prefabs();
        }
        if (did_move_prefab) {
            std::wstring m = L"{\"kind\":\"movePrefabResult\",\"result\":"; m += std::to_wstring(g_move_prefab_result);
            m += L",\"name\":\""; m += poc_json_w(g_move_prefab_name.c_str());
            m += L"\",\"toFolder\":\""; m += poc_json_w(g_move_prefab_to.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_move_prefab_result == 1) poc_send_prefabs();
        }

        if (g_webview_ready) {
            bool ready = poc_editor_ready() != 0;
            if (ready && !was_visible) { ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd); poc_send_list(); was_visible = true; }
            else if (!ready && was_visible) { ShowWindow(g_hwnd, SW_HIDE); was_visible = false; }

            /* periodic auto tasks (~ every 10 frames = ~330 ms): list change poll, editor-selection sync,
             * displayed-state change poll. All emit only on an actual change. */
            if (was_visible && (frame % 10 == 0)) {
                int rdy = 0; int n = poc_collect(&rdy);
                uint64_t sig = poc_list_sig(n);
                if (sig != g_last_list_sig) {
                    poc_rescan_timelines(n);   /* the one classname call site -- change-gated, not a fixed timer */
                    g_last_list_sig = sig;
                    poc_emit_list(n, rdy);
                }

                /* live editor-selection COUNT, independent of "Follow editor selection" -- the Prefabs tab's
                 * "Create from selection (N)" button needs this regardless of sync mode. POC_MAX_ENTS (not
                 * a fresh 64-cap) since a selection can't exceed the total entity list, already capped there. */
                static int selids[POC_MAX_ENTS];
                int sn = poc_get_selection(selids, POC_MAX_ENTS);
                if (sn != g_last_selcount) {
                    g_last_selcount = sn;
                    wchar_t m[64]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"selCount\",\"count\":%d}", sn);
                    poc_post_json(m);
                }
                if (g_sync_on) {
                    /* mirror the WHOLE editor selection (any N) into the list, only when it changes. */
                    for (int a = 1; a < sn; a++) { int v = selids[a]; int b = a - 1; while (b >= 0 && selids[b] > v) { selids[b+1] = selids[b]; b--; } selids[b+1] = v; }
                    uint64_t sig = 1469598103934665603ull;
                    for (int a = 0; a < sn; a++) sig = hint(sig, selids[a]);
                    sig ^= (uint64_t)sn;
                    if (sig != g_last_sel_sig) {
                        g_last_sel_sig = sig;
                        std::wstring m = L"{\"kind\":\"editorSelect\",\"eids\":[";
                        for (int a = 0; a < sn; a++) { if (a) m += L","; m += std::to_wstring(selids[a]); }
                        m += L"]}";
                        poc_post_json(m.c_str());
                        if (sn == 1) g_displayed_eid = selids[0];
                    }
                }
                if (g_displayed_eid >= 0) poc_send_state(g_displayed_eid, true);
                if (!g_cam_lock) poc_cam_read_send();   /* live camera readout (unless locked) */
            }
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(0x21);
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI snaphak_ui_init(LPVOID param_1)
{
    sh_ui_argblock *args = reinterpret_cast<sh_ui_argblock *>(param_1);
    g_loop = new ShLoopState();
    InitializeCriticalSection(&g_loop->mtx); g_loop->flags = 0;
    if (args && args->out_slot) *reinterpret_cast<void **>(args->out_slot) = g_loop;
    g_iface = args ? args->iface : nullptr;
    g_ents = (PocEnt *)malloc(sizeof(PocEnt) * POC_MAX_ENTS);
    g_tls  = (PocTl  *)malloc(sizeof(PocTl)  * POC_MAX_TLS);
    if (!g_ents || !g_tls) {
        poc_log("snaphak_ui_init: FATAL -- malloc failed for g_ents/g_tls, aborting init");
        free(g_ents); g_ents = nullptr;
        free(g_tls);  g_tls  = nullptr;
        return 1;
    }
    poc_read_version();

    poc_log("=== snaphak_ui_init (WebView2 POC, entities-deep) entered ===");
    poc_log(g_iface ? "interface handed over: yes" : "interface handed over: NO (null)");
    { char v[128]; _snprintf_s(v, sizeof v, _TRUNCATE, "installed version: %s", g_version.c_str()); poc_log(v); }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int n = MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, nullptr, 0);
    g_html.resize(n > 0 ? n - 1 : 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, &g_html[0], n);

    poc_create_window();
    poc_start_webview();
    poc_think_loop();
    return 0;
}
