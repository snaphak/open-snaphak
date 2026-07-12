/* snapstack.cpp -- the SnapStack STORES + the 9 STORE-op handlers + the 20-subcommand registrar.
 *
 * Clean-room, FAITHFUL port of the reference implementation (the proven
 * proven mechanism). The stores (StackStore/GroupStore) are PURE; the op handlers reach the editor through
 * the backend-owned interface vtable's engine-touch slots (selection read/write, hovered, toast, class/
 * inherit read). Toast strings + the filtcls "had inherit" mislabel are reproduced VERBATIM from the OG
 * handler text. Zero OG SnapHak bytes.
 */
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#ifndef NOMINMAX
#define NOMINMAX        /* keep std::min/max usable alongside windows.h (Qt-safe) */
#endif
#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <cmath>
#include <cstring>
#include <functional>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include "snaphak_iface.h"
#include "snapstack.h"
#include "mkcmd_template.inc"   /* SH_MKCMD_PREFAB_TEMPLATE -- the byte-exact embedded prefab template */

/* the byte-exact idSnapEntityPrefab template (prefix + __SNAPHAK_MKCMD_COMMANDTEXT__ placeholder + suffix),
 * embedded from the prefab template (mkcmd_template.inc) so the frontend needs no file at
 * runtime. mkcmd splices the assembled commandText at the placeholder. */
static std::string sh_load_mkcmd_template() { return std::string(SH_MKCMD_PREFAB_TEMPLATE); }

/* ============================================================ the STACK-OF-STACKS ================== */

void StackStore::ensure(int index)
{
    if (index < 0) return;                       /* the handler clamps the index >= 0 already */
    while ((int)stacks_.size() <= index) stacks_.push_back(std::vector<int>());
}

std::vector<int> &StackStore::get(int index)
{
    if (index < 0) index = 0;
    ensure(index);
    return stacks_[index];
}

int StackStore::push(int index, const std::vector<int> &ids)
{
    std::vector<int> &stack = get(index);
    /* dedup-on-push (psel's body): an id already present is not re-added. */
    int pushed = 0;
    for (size_t i = 0; i < ids.size(); i++) {
        int eid = ids[i];
        bool seen = false;
        for (size_t j = 0; j < stack.size(); j++) if (stack[j] == eid) { seen = true; break; }
        if (seen) continue;
        stack.push_back(eid);
        pushed++;
    }
    return pushed;
}

void StackStore::clear(int index)
{
    /* cstk (0x2208): in-range only, no auto-grow (the OG range guard). */
    if (index >= 0 && index < (int)stacks_.size()) stacks_[index].clear();
}

std::vector<int> StackStore::move_out(int index)
{
    /* FUN_180001d84: hand back stack[N]'s ids + leave the stack EMPTY. Out-of-range -> [] + no grow. */
    if (index < 0 || index >= (int)stacks_.size()) return std::vector<int>();
    std::vector<int> moved = stacks_[index];
    stacks_[index].clear();
    return moved;
}

/* ============================================================ the named GROUPS ===================== */

std::vector<int> &GroupStore::get(const std::string &name)
{
    /* lookup-or-insert (OG FUN_180003e9c): a miss materializes an empty group. */
    return groups_[name];
}

void GroupStore::set(const std::string &name, const std::vector<int> &ids)
{
    groups_[name] = ids;
}

bool GroupStore::has(const std::string &name) const
{
    return groups_.find(name) != groups_.end();
}

bool is_valid_group_name(const std::string &name)
{
    /* pop2g (0x2998 isalpha): first char must be a letter; empty fails. */
    return !name.empty() && (std::isalpha((unsigned char)name[0]) != 0);
}

int parse_stack_index(const char *arg)
{
    /* C atoi clamped >= 0; absent/blank -> 0 (snapstack.parse_stack_index). */
    if (arg == nullptr) return StackStore::DEFAULT_STACK;
    /* skip leading whitespace */
    const char *p = arg;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return StackStore::DEFAULT_STACK;
    int v = atoi(p);
    return v < 0 ? 0 : v;
}

/* ============================================================ the shared stores ==================== */
/* OG holds these as module globals (DAT_180031800 stack-of-stacks + DAT_180031830 group nil-node); the
 * clone holds them as file-static singletons in the frontend (the same single-instance lifetime). The op
 * handlers + the registered subcommands all share THESE. */
static StackStore g_stacks;
static GroupStore g_groups;

/* ============================================================ engine-touch helpers ================ */
/* Thin SEH-free wrappers over the interface vtable's engine-touch slots (the backend SEH-guards each body).
 * Every one null-checks the slot (a partial backend build leaves a slot NULL) -> a clean degrade. */

static std::vector<int> iface_get_selection(sh_iface *iface)
{
    std::vector<int> ids;
    if (!iface || !iface->vtbl || !iface->vtbl->get_selection) return ids;
    int buf[4096];
    int n = iface->vtbl->get_selection(iface, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    for (int i = 0; i < n; i++) ids.push_back(buf[i]);
    return ids;
}
static void iface_clear_selection(sh_iface *iface)
{
    if (iface && iface->vtbl && iface->vtbl->clear_selection) iface->vtbl->clear_selection(iface);
}
static void iface_add_to_selection(sh_iface *iface, int id)
{
    if (iface && iface->vtbl && iface->vtbl->add_to_selection) iface->vtbl->add_to_selection(iface, id);
}
static int iface_hovered_id(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->hovered_id) return -1;
    return iface->vtbl->hovered_id(iface);
}
static bool iface_is_valid_id(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->is_valid_id) return false;
    return iface->vtbl->is_valid_id(iface, id) != 0;
}
static void iface_toast(sh_iface *iface, const char *title, const char *text)
{
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, title, text);
}
static std::string iface_classname(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_classname_copy) return std::string();
    char buf[256];
    buf[0] = '\0';
    const char *s = iface->vtbl->get_classname_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
static std::string iface_inherit(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_inherit_copy) return std::string();
    char buf[256];
    buf[0] = '\0';
    const char *s = iface->vtbl->get_inherit_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}

/* --- the class/inherit-CHANGE engine touches (bscls/bsin/bsincls): the SAME four DATA-tab setter slots the
 * Save-to-Decl |2 consumer uses (sh_tabs.cpp apply_entity_state). Each null-checks its slot -> a clean
 * degrade on a partial backend build; the backend SEH-guards every body (iface_engine.c slot_set_class /
 * slot_get_declsource / slot_rebuild_declsource), so the heavy main-thread engine touch can never crash the
 * frontend. NO QJson/serialize round-trip -- class-change is the DIRECT setter-slot path (+0x78/+0x80/+0x30/
 * +0x40), exactly like the OG handlers FUN_180001244/FUN_1800012dc + the Save-to-Decl route. */
static void iface_set_classname(sh_iface *iface, int id, const std::string &v)   /* +0x78 */
{
    if (iface && iface->vtbl && iface->vtbl->set_classname) iface->vtbl->set_classname(iface, id, v.c_str());
}
static void iface_set_inherit(sh_iface *iface, int id, const std::string &v)     /* +0x80 */
{
    if (iface && iface->vtbl && iface->vtbl->set_inherit) iface->vtbl->set_inherit(iface, id, v.c_str());
}
/* +0x30 GET the live entity's canonical decl-source text (the `r` the OG handlers pass to the rebuild). The
 * decl-source can be large (snapVar bodies); size the buffer generously, like sh_tabs.cpp iface_declsource. */
static std::string iface_declsource_text(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_declsource_copy) return std::string();
    static char buf[64 * 1024];
    buf[0] = '\0';
    const char *s = iface->vtbl->get_declsource_copy(iface, id, buf, (int)sizeof(buf));
    return std::string(s ? s : "");
}
/* +0x40 DeclSourceRebuild(defsub, src, 1): re-emit the canonical class/inherit header + the edit body from
 * the (just-updated) defsub fields. THIS is the heavy re-resolve cascade (FINITE -- truth class-inherit-
 * change-freeze.md). The backend SEH-guards the engine call. */
static void iface_rebuild_declsource(sh_iface *iface, int id, const std::string &src)   /* +0x40 */
{
    if (iface && iface->vtbl && iface->vtbl->rebuild_set_declsource)
        iface->vtbl->rebuild_set_declsource(iface, id, src.c_str());
}

/* +0x268 ATOMIC class+inherit set: one FINAL-pair check then BOTH idStr fields, guard-bypassed.
 * Returns 1 = applied, 0 = rejected (fatal combo -- the caller MUST skip the rebuild), -1 = slot absent (old
 * backend -> the caller falls back to the legacy two guarded single-field sets). */
static int iface_apply_class_inherit(sh_iface *iface, int id, const std::string &cls, const std::string &inh) /* +0x268 */
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_class_inherit) return -1;
    return iface->vtbl->apply_class_inherit(iface, id, cls.c_str(), inh.c_str());
}

/* the id-STRING the bse/accl/acctargets/mkcmd ops use as the leaf value (the +0x18 resolve; the reference implementation
 * entityIdString -- the entity NAME, decimal-id fallback). The backend slot writes the resolved string. */
static std::string iface_id_string(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->id_to_string) {
        char d[24]; _snprintf_s(d, sizeof d, _TRUNCATE, "%d", id); return std::string(d);
    }
    char buf[256];
    buf[0] = '\0';
    const char *s = iface->vtbl->id_to_string(iface, id, buf, (int)sizeof(buf));
    if (s && s[0]) return std::string(s);
    char d[24]; _snprintf_s(d, sizeof d, _TRUNCATE, "%d", id); return std::string(d);
}

/* ============================================================ the heavy apply bridge ========
 * The 8 APPLY-ops (bss/bsi/bsf/bsb/bse/accl/acctargets/mkcmd) reach the engine through THREE heavy slots
 * the backend owns (apply_engine.c). The frontend does ONLY the JSON patch in between -- a native port
 * of the reference implementation's doApplyBssOne (serialize -> patch leaf -> schedule deserialize+commit). The serialize +
 * the deserialize/commit are engine work behind the vtable; the patch is here (QJson + a RAW-TOKEN splice
 * for the float leaf -- NEVER QJson re-serialize, which drops the engine-required .0). */

#define SH_APPLY_JSON_CAP   (256 * 1024)   /* the full-entity JSON can be large (snapVar arrays) */

/* +0xc8 serialize entity id -> the FULL idSnapEntity JSON. Returns "" on a binding gap / no map. */
static std::string iface_serialize_entity(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->serialize_entity) return std::string();
    std::string out;
    out.resize(SH_APPLY_JSON_CAP);
    int n = iface->vtbl->serialize_entity(iface, id, &out[0], SH_APPLY_JSON_CAP);
    if (n <= 0) return std::string();
    out.resize((size_t)n);
    return out;
}

/* +0xd0 SCHEDULE a batch of apply-items at the engine command-exec point (FIX B). Returns true if scheduled. */
static bool iface_schedule_apply(sh_iface *iface, const std::vector<sh_apply_item> &items, const char *op)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_edit || items.empty()) return false;
    return iface->vtbl->apply_edit(iface, items.data(), (int)items.size(), op) != 0;
}

/* +0x290 SYNCHRONOUS inline apply (OG-faithful): commit the batch NOW on this (UI/think-loop) thread, exactly
 * like OG's acctargets handler commits +0xd0 inline -- NOT deferred to the main thread. Returns the applied
 * count, or -1 if the slot is absent (an older backend -> the caller should fall back to schedule). */
static int iface_apply_sync(sh_iface *iface, const std::vector<sh_apply_item> &items, const char *op)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_sync) return -1;   /* slot absent -> caller fallback */
    if (items.empty()) return 0;
    return iface->vtbl->apply_sync(iface, items.data(), (int)items.size(), op);
}

/* Apply a batch the OG-FAITHFUL way: SYNCHRONOUS inline (+0x290) when the backend has it, else the deferred
 * +0xd0 schedule (older backend only). ALL the decl-edit SnapStack ops (accl/acctargets/bss/bsi/bsf/bsb/bse)
 * go through this so the commit runs INLINE on the UI/think-loop thread -- serialize + commit atomic on one
 * thread -> the committed decl-source block has a SINGLE clean owner (OG's behavior). The deferred +0xd0 split
 * them across threads/frames and double-owned the block -> the play->teardown double-free ("Memory corruption
 * before block"). Returns true if applied (or scheduled on the fallback). */
static bool iface_apply(sh_iface *iface, const std::vector<sh_apply_item> &items, const char *op)
{
    if (items.empty()) return false;
    int applied = iface_apply_sync(iface, items, op);   /* +0x290 sync -> applied count (>=0), or -1 absent */
    if (applied >= 0) return true;                       /* sync ran; backend toasts the applied/total count */
    return iface_schedule_apply(iface, items, op);       /* old backend without +0x290 -> deferred fallback */
}

/* the work-item argv is argv[0]=subcommand, argv[1..]=its args; this fetches argv[n] safely. */
static const char *arg_at(int argc, const char **argv, int n)
{
    return (argv && n >= 0 && n < argc) ? argv[n] : nullptr;
}

/* fwd-decl: the OG FUN_180001fa0-faithful operand resolver (group->copy / numeric index->move_out/consume),
 * defined with the apply bridge below; h_popsel (a store-op above it) also resolves through it. */
static std::vector<int> resolve_operand_consume(int argc, const char **argv);

/* ============================================================ the 9 STORE-op handlers ============== */
/* Each is a sh_cmd_handler: void(void* ctx, int argc, const char** argv). ctx = the interface (bound at
 * register time). The handler runs on the MAIN (UI) thread (the +0x1a0 drain). argv[0] = the subcommand
 * name; argv[1] = the stack-index arg (where applicable). Toast strings are VERBATIM (the OG handler text). */

/* psel [stack] (0x2108): read live selection -> push onto stack[N] (dedup) -> CLEAR selection -> toast
 * "pushed %d entities onto stack %d." */
static void h_psel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    std::vector<int> sel = iface_get_selection(iface);
    /* robustness + diagnostic: drop ids that are no longer valid (a stale selection entry) before
     * pushing -- same defense as do_acc -- and report selection/pushed/now counts so a "nothing got pushed"
     * is diagnosable at a glance: selection=0 => the +0x150 read returned nothing (re-select / a read issue);
     * pushed<selection => duplicates already on the stack (clear it with cstk). */
    std::vector<int> valid;
    valid.reserve(sel.size());
    int dropped = 0;
    for (size_t i = 0; i < sel.size(); i++) {
        if (iface_is_valid_id(iface, sel[i])) valid.push_back(sel[i]); else dropped++;
    }
    int pushed   = g_stacks.push(index, valid);
    int stackNow = (int)g_stacks.get(index).size();
    iface_clear_selection(iface);                       /* +0x148 CLEAR (psel clears after pushing) */
    char text[176];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "psel: selection=%d%s -> pushed %d onto stack %d (now %d).",
                (int)sel.size(), (dropped ? " (stale dropped)" : ""), pushed, index, stackNow);
    iface_toast(iface, "SnapStack", text);
}

/* popsel [stack] (0x3a14): "POP selection" -- ADD each stored id back to the editor selection (+0x138), then
 * CONSUME the operand. OG resolves via FUN_180001fa0, so a numeric stack is MOVED OUT (emptied) / a named
 * group is COPIED (preserved). (The clone previously used a non-consuming g_stacks.get() copy -- an RE miss;
 * the decompile move_out's the numeric stack.) No toast (the OG body just adds). */
static void h_popsel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* numeric: consume; group: copy */
    for (size_t i = 0; i < ids.size(); i++) iface_add_to_selection(iface, ids[i]);
}

/* phov [stack] (0x20b4): push the HOVERED id (+0x198) onto stack[N], dedup. <0 -> push 0. No toast. */
static void h_phov(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    int hovered = iface_hovered_id(iface);
    if (hovered >= 0) {
        std::vector<int> one;
        one.push_back(hovered);
        g_stacks.push(index, one);
    }
    /* hovered < 0 -> push nothing (the "push 0" no-op). No toast. */
}

/* cstk [stack] (0x2208): empty stack[N] in place. No engine call, no toast. */
static void h_cstk(void *ctx, int argc, const char **argv)
{
    (void)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    g_stacks.clear(index);
}

/* pr <stack> <lo> <hi> (0x2c9c): push every VALID id in [lo..hi] -> stack[N], dedup; toast
 * "Pushed %d entities out of %d in range %d-%d" (out-of = SPAN = hi-lo, the OG iVar5-iVar4). */
static void h_pr(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *lo_s = arg_at(argc, argv, 2);
    const char *hi_s = arg_at(argc, argv, 3);
    int lo = lo_s ? atoi(lo_s) : 0;
    int hi = hi_s ? atoi(hi_s) : 0;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }        /* normalize a reversed range */
    int span = hi - lo;                                  /* DIRECT 0x2c9c: span, NOT inclusive count */
    std::vector<int> in_range;
    for (int id = lo; id <= hi; id++)
        if (iface_is_valid_id(iface, id)) in_range.push_back(id);
    int pushed = g_stacks.push(index, in_range);
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "Pushed %d entities out of %d in range %d-%d", pushed, span, lo, hi);
    iface_toast(iface, "Pushed entities", text);
}

/* pg <stack> <group> (0x2b54): push the named group's ids -> stack[N], dedup; toast
 * "Pushed %d entities from group %s". A missing group auto-creates empty (push 0). */
static void h_pg(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *name_s = arg_at(argc, argv, 2);
    std::string name = name_s ? name_s : "";
    std::vector<int> ids = g_groups.get(name);
    int pushed = g_stacks.push(index, ids);
    char text[256];
    _snprintf_s(text, sizeof text, _TRUNCATE, "Pushed %d entities from group %s", pushed, name.c_str());
    iface_toast(iface, "Pushed entities", text);
}

/* pop2g <stack> <group> (0x2998): MOVE stack[N] -> named group (swap). The name must start with a letter;
 * else toast "<name> is not a valid group name because it needs to start with a letter" (title
 * "Invalid groupname"). */
static void h_pop2g(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *name_s = arg_at(argc, argv, 2);
    std::string name = name_s ? name_s : "";
    if (!is_valid_group_name(name)) {
        char text[256];
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "%s is not a valid group name because it needs to start with a letter", name.c_str());
        iface_toast(iface, "Invalid groupname", text);
        return;
    }
    std::vector<int> moved = g_stacks.move_out(index);   /* empties stack N, returns its old ids */
    g_groups.set(name, moved);                           /* the group becomes those ids (move-into swap) */
}

/* filtinh / filtcls shared body (0x3c70 / 0x3c78): KEEP only stack[N] ids whose inherit (filtinh) or
 * classname (filtcls) == match; re-push the survivors; toast the count. FIX: OG mislabeled the
 * filtcls toast "had inherit %s" (it reused filtinh's string); corrected to "had class %s" for filtcls. */
static void do_filt(sh_iface *iface, int index, const std::string &match, bool by_class)
{
    std::vector<int> cur = g_stacks.move_out(index);     /* resolve + clear the stack, then re-push keepers */
    std::vector<int> kept;
    for (size_t i = 0; i < cur.size(); i++) {
        int id = cur[i];
        std::string field = by_class ? iface_classname(iface, id) : iface_inherit(iface, id);
        if (field == match) kept.push_back(id);
    }
    g_stacks.push(index, kept);
    char text[256];
    /* FIX: label by the actual filter field -- filtcls "had class", filtinh "had inherit"
     * (OG mislabeled both as "had inherit"). */
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "%d entities on stack %d had %s %s", (int)kept.size(), index,
                by_class ? "class" : "inherit", match.c_str());
    iface_toast(iface, "Filter", text);
}

static void h_filtinh(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *match_s = arg_at(argc, argv, 2);
    do_filt(iface, index, match_s ? match_s : "", /*by_class=*/false);
}

static void h_filtcls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *match_s = arg_at(argc, argv, 2);
    do_filt(iface, index, match_s ? match_s : "", /*by_class=*/true);
}

/* ============================================================ the 8-pass apply chain ====== */
/* The 8 engine-apply ops are the native port of the reference implementation's proven full-entity round-trip. Each
 * handler runs on the +0x1a0 work-queue drain (the UI thread). The HEAVY engine work (serialize entity ->
 * JSON, deserialize patched JSON -> commit) is behind the backend's three heavy vtable slots; the FRONTEND
 * does only the JSON PATCH between them (QJson for structure + a RAW-TOKEN splice for the bsf float leaf).
 *
 * Threading (FIX B, the reference implementation): the structured-deserialize AVs if run mid-frame (a stale reflection-handler),
 * so the apply does NOT run inline on the drain. The frontend builds the patched text + SCHEDULEs it via
 * the +0xd0 slot; the backend BufferCommandTexts clone_bss_apply and the engine drains the heavy apply on
 * the DOOM main thread (the decl-safe exec point). The result toasts from the backend. */

/* --- C atoi / atof (the reference implementation _c_atoi / _c_atof; the reference implementation cAtoi / cAtof) --- */
static long sh_c_atoi(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if (!(*s >= '0' && *s <= '9')) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}
static double sh_c_atof(const char *s) { return s ? atof(s) : 0.0; }   /* atof = leading-token, else 0.0 */

/* --- renderEngineFloat: the ENGINE-FORMAT float token. The engine's text form is the shortest
 * round-trip decimal: keep `.0` on whole floats (2.0 not 2) + switch to scientific notation at
 * exp < -4 or exp >= 16. We build it from the shortest round-trip digits printf gives (%.17g
 * over-prints; we trim to the shortest that round-trips), then format exactly to that rule -- the
 * shortest round-trip repr with C-style exponents. --- */
static std::string sh_shortest_digits(double f, int &out_exp, bool &out_neg)
{
    /* find the shortest %.Ne that round-trips to f, then split mantissa digits + decimal exponent E. */
    out_neg = std::signbit(f);
    double af = std::fabs(f);
    char buf[64];
    int prec = 0;
    for (prec = 0; prec <= 17; prec++) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%.*e", prec, af);
        double rt = atof(buf);
        if (rt == af) break;
    }
    /* buf = "d.dddde[+-]XX" (or "de[+-]XX" at prec 0). Split. */
    const char *e = strchr(buf, 'e');
    std::string mant(buf, e ? (size_t)(e - buf) : strlen(buf));
    out_exp = e ? atoi(e + 1) : 0;
    /* strip the '.' -> the pure significant-digit string. */
    std::string digits;
    for (size_t i = 0; i < mant.size(); i++) if (mant[i] != '.') digits.push_back(mant[i]);
    /* drop trailing zeros (the shortest form has none, but %.0e can leave a bare digit -- keep >=1). */
    while (digits.size() > 1 && digits.back() == '0') { digits.pop_back(); }
    return digits;
}
static std::string render_engine_float(double f)
{
    if (f == 0.0) return std::signbit(f) ? std::string("-0.0") : std::string("0.0");
    if (std::isnan(f)) return std::string("NaN");
    if (std::isinf(f)) return f > 0 ? std::string("Infinity") : std::string("-Infinity");
    int E; bool neg;
    std::string digits = sh_shortest_digits(f, E, neg);   /* digits has no point; E = decimal exponent */
    std::string body;
    if (E < -4 || E >= 16) {
        /* SCIENTIFIC (the exp<-4 / exp>=16 threshold): mant = d[.ddd], exponent e+NN / e-NN (>=2 digits). */
        std::string mant = (digits.size() > 1) ? (digits.substr(0, 1) + "." + digits.substr(1)) : digits;
        int ae = E < 0 ? -E : E;
        char ebuf[16];
        _snprintf_s(ebuf, sizeof ebuf, _TRUNCATE, "%c%02d", E < 0 ? '-' : '+', ae);
        body = mant + "e" + ebuf;
    } else if (E >= 0) {
        /* FIXED, integer part has E+1 digits; always keep a fractional part. */
        if ((int)digits.size() <= E + 1) {
            body = digits + std::string((size_t)(E + 1 - (int)digits.size()), '0') + ".0";
        } else {
            body = digits.substr(0, (size_t)(E + 1)) + "." + digits.substr((size_t)(E + 1));
        }
    } else {
        /* FIXED, value < 1 (E in [-4,-1]): "0." + leading zeros + digits. */
        body = "0." + std::string((size_t)(-E - 1), '0') + digits;
    }
    std::string s = (neg ? "-" : "") + body;
    /* C-style exponent strip (format-spec rule 5): e+07 -> e7, e-07 -> e-7 (drop '+' + leading zeros). */
    size_t ep = s.find('e');
    if (ep != std::string::npos) {
        std::string head = s.substr(0, ep + 1);   /* incl. the 'e' */
        std::string tail = s.substr(ep + 1);
        bool em = false; size_t k = 0;
        if (k < tail.size() && (tail[k] == '+' || tail[k] == '-')) { em = (tail[k] == '-'); k++; }
        while (k < tail.size() - 1 && tail[k] == '0') k++;   /* drop leading zeros (keep >=1 digit) */
        s = head + (em ? "-" : "") + tail.substr(k);
    }
    return s;
}

/* --- the per-op leaf-value ENCODING (the reference implementation encodeBulkSetLeafJson; the reference implementation bulkset_leaf_json):
 * bss -> a JSON string literal; bsi -> a bare int (cAtoi); bsf -> renderEngineFloat(fround(cAtof));
 * bsb -> "true" iff value=="true" else "false". Returns the RAW JSON leaf TOKEN (spliced VERBATIM). --- */
static std::string encode_bulkset_leaf_json(const std::string &op, const std::string &value)
{
    if (op == "bsi") {
        char b[24]; _snprintf_s(b, sizeof b, _TRUNCATE, "%ld", sh_c_atoi(value.c_str())); return std::string(b);
    }
    if (op == "bsf") {
        float f32 = (float)sh_c_atof(value.c_str());          /* Math.fround: narrow to float32 */
        return render_engine_float((double)f32);              /* engine-format token (raw splice) */
    }
    if (op == "bsb") {
        return (value == "true") ? std::string("true") : std::string("false");
    }
    /* bss / default: a JSON string literal (QJson-escaped, compact). Emit a 1-element array [<literal>]
     * and strip the surrounding brackets -> the bare escaped string literal token. */
    QByteArray esc = QJsonDocument(QJsonArray{ QString::fromStdString(value) }).toJson(QJsonDocument::Compact);
    QByteArray inner = esc.mid(1, esc.size() - 2).trimmed();   /* ["x"] -> "x" */
    return std::string(inner.constData(), (size_t)inner.size());
}

/* The unique sentinel the reference implementation uses: set the leaf to it, stringify, then string-replace it with the raw
 * value token VERBATIM (preserves the float `2.0` that a QJson re-serialize would drop). */
static const char *SH_RAW_LEAF = "__RAW_LEAF__";

/* Patch one entityDef.state.edit.<propPath> SCALAR leaf into the full-entity JSON, returning the modified
 * compact JSON (or "" on a parse/shape failure). the reference implementation patchFullJsonEdit: ensure entityDef.state.edit,
 * walk the '.'-split propPath creating intermediates, set the leaf to the sentinel, compact-emit, then
 * string-replace the sentinel literal with the raw `leaf_token` VERBATIM. */
static std::string patch_full_json_edit(const std::string &full_json, const std::string &prop_path,
                                        const std::string &leaf_token)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(full_json.data(), (int)full_json.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::string();
    QJsonObject root = doc.object();
    if (!root.contains("entityDef") || !root.value("entityDef").isObject()) return std::string();
    QJsonObject ed = root.value("entityDef").toObject();
    QJsonObject state = ed.value("state").isObject() ? ed.value("state").toObject() : QJsonObject();
    QJsonObject edit  = state.value("edit").isObject() ? state.value("edit").toObject() : QJsonObject();

    /* walk the '.'-split propPath, creating intermediate objects; set the LEAF to the sentinel. */
    QStringList segs = QString::fromStdString(prop_path).split('.');
    if (segs.isEmpty()) return std::string();
    /* rebuild nested objects bottom-up: collect each level, set, re-insert. We mutate via a chain copy. */
    std::function<QJsonObject(QJsonObject, int)> setNested =
        [&](QJsonObject cur, int idx) -> QJsonObject {
            const QString key = segs[idx];
            if (idx == segs.size() - 1) {
                cur.insert(key, QJsonValue(QString::fromUtf8(SH_RAW_LEAF)));
            } else {
                QJsonObject child = cur.value(key).isObject() ? cur.value(key).toObject() : QJsonObject();
                cur.insert(key, setNested(child, idx + 1));
            }
            return cur;
        };
    edit = setNested(edit, 0);
    state.insert("edit", edit);
    ed.insert("state", state);
    root.insert("entityDef", ed);

    QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Compact);
    std::string s(out.constData(), (size_t)out.size());
    /* the sentinel was emitted as the JSON string literal "__RAW_LEAF__" -> replace it with the raw token. */
    std::string needle = std::string("\"") + SH_RAW_LEAF + "\"";
    size_t pos = s.find(needle);
    if (pos == std::string::npos) return std::string();
    s.replace(pos, needle.size(), leaf_token);
    return s;
}

/* Patch a num/item[] LIST into entityDef.state.edit.<propPath> on the full-entity JSON (accl/acctargets).
 * the reference implementation patchFullJsonRefList: the engine list object is {"item[0]":s0,...,"num":N}. The members are
 * id-STRINGS (JSON string literals), so a plain QJson re-emit is safe here (no float). Returns "" on shape
 * failure. */
static std::string patch_full_json_reflist(const std::string &full_json, const std::string &prop_path,
                                           const std::vector<std::string> &id_strings)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(full_json.data(), (int)full_json.size()), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::string();
    QJsonObject root = doc.object();
    if (!root.contains("entityDef") || !root.value("entityDef").isObject()) return std::string();
    QJsonObject ed = root.value("entityDef").toObject();
    QJsonObject state = ed.value("state").isObject() ? ed.value("state").toObject() : QJsonObject();
    QJsonObject edit  = state.value("edit").isObject() ? state.value("edit").toObject() : QJsonObject();

    /* ACCUMULATE-WITH-DEDUP onto the receiver's EXISTING list: acctargets/accl serialize the receiver's
     * CURRENT state (+0xc8) and ADD to its existing entityDef.state.edit.<path> num/item[] list (FUN_180001a58
     * keeps the node if it is already a list; FUN_18000137c read-num/append). We KEEP the existing items and
     * append only the new id-strings that are NOT already present -> re-targeting an entity you already
     * targeted is a no-op, never a duplicate. (DEDUP added 2026-06-26 on tester feedback: the previous
     * accumulate appended blindly, so target-X-then-X-again gave [X,X] -> the listener fired twice. The OG
     * observable behavior is no duplicate targets.) An absent / non-list leaf starts fresh at num=0. */
    QStringList segs = QString::fromStdString(prop_path).split('.');
    if (segs.isEmpty()) return std::string();
    std::function<QJsonObject(QJsonObject, int)> setNested =
        [&](QJsonObject cur, int idx) -> QJsonObject {
            const QString key = segs[idx];
            if (idx == segs.size() - 1) {
                QJsonObject existing = cur.value(key).toObject();     /* {} if absent / not an object */
                int base = 0;
                QJsonObject list;                                     /* fresh unless the leaf IS a num/item[] list */
                std::set<std::string> present;                        /* existing target id-strings -> dedup */
                if (existing.value("num").isDouble()) {               /* already a list -> preserve + append NEW only */
                    base = existing.value("num").toInt(0);
                    if (base < 0) base = 0;
                    list = existing;                                  /* keep item[0..base-1] */
                    for (int k = 0; k < base; k++) {
                        QJsonValue v = existing.value(QString("item[%1]").arg(k));
                        if (v.isString()) present.insert(v.toString().toStdString());
                    }
                }
                int next = base;
                for (size_t i = 0; i < id_strings.size(); i++) {
                    if (present.count(id_strings[i])) continue;       /* already a target -> skip (no duplicate) */
                    list.insert(QString("item[%1]").arg(next), QString::fromStdString(id_strings[i]));
                    present.insert(id_strings[i]);
                    next++;
                }
                list.insert("num", next);
                cur.insert(key, list);
            } else {
                QJsonObject child = cur.value(key).isObject() ? cur.value(key).toObject() : QJsonObject();
                cur.insert(key, setNested(child, idx + 1));
            }
            return cur;
        };
    edit = setNested(edit, 0);
    state.insert("edit", edit);
    ed.insert("state", state);
    root.insert("entityDef", ed);
    QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return std::string(out.constData(), (size_t)out.size());
}

/* --- DECL-SOURCE targets splice (the crash-free acctargets/accl apply path) ---------------------------
 * Splice a `<path> = { item[0]="ref0"; ...; num=N; }` LIST into the edit{} block of a DECL-SOURCE text
 * (decl syntax: '=' assignments, ';' terminators, tab indent -- NOT JSON). acctargets/accl now apply the
 * SAME way the manual Entity-State Save + the class/inherit handlers do: read the live decl source via
 * +0x30, edit it as text, commit via the SYNCHRONOUS +0x40 DeclSourceRebuild. This REPLACES the deferred
 * entity-JSON -> temp-def-deserialize (ae_apply_one), whose reload-teardown DOUBLE-FREES the committed
 * source block (the acctargets play->save->reload crash; live-diagnosed 2026-07-12 -- OG's +0xd0 commit is
 * byte-identical to ours, so the divergence was the deferral/temp-def, not the commit content). Accumulate
 * with dedup onto any existing block. Single-level path (acctargets = "targets"). Returns "" on a shape
 * failure (the caller then leaves the receiver untouched). */
static size_t decl_match_brace(const std::string &s, size_t open)   /* index of the '}' matching s[open]=='{' */
{
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        else if (s[i] == '}') { if (--depth == 0) return i; }
    }
    return std::string::npos;
}
static std::string splice_decl_reflist(const std::string &decl, const std::string &path,
                                       const std::vector<std::string> &ids)
{
    /* locate the top-level `edit { ... }` block (entityDef.state.edit -- where the lists live). */
    size_t ek = decl.find("edit");
    if (ek == std::string::npos) return std::string();
    size_t eo = decl.find('{', ek);
    if (eo == std::string::npos) return std::string();
    size_t ec = decl_match_brace(decl, eo);
    if (ec == std::string::npos) return std::string();

    /* find an existing `<path> = { ... }` block INSIDE edit{} (token-boundary match). */
    size_t blkOpen = std::string::npos, blkClose = std::string::npos;
    for (size_t scan = eo + 1; scan < ec; ) {
        size_t f = decl.find(path, scan);
        if (f == std::string::npos || f >= ec) break;
        char b = (f > 0) ? decl[f - 1] : ' ';
        char a = (f + path.size() < decl.size()) ? decl[f + path.size()] : ' ';
        int btok = (b==' '||b=='\t'||b=='\n'||b=='\r'||b=='{');
        int atok = (a==' '||a=='\t'||a=='=');
        if (btok && atok) {
            size_t eq = decl.find('=', f + path.size());
            size_t br = (eq != std::string::npos) ? decl.find_first_not_of(" \t", eq + 1) : std::string::npos;
            if (br != std::string::npos && br < ec && decl[br] == '{') {
                blkOpen  = br;
                blkClose = decl_match_brace(decl, br);
                break;
            }
        }
        scan = f + path.size();
    }

    /* gather existing refs (dedup) from the block, then append the new ids (dedup). */
    std::vector<std::string> refs;
    std::set<std::string> present;
    if (blkOpen != std::string::npos && blkClose != std::string::npos) {
        for (size_t p = blkOpen + 1; p < blkClose; ) {
            size_t it = decl.find("item[", p);
            if (it == std::string::npos || it >= blkClose) break;
            size_t q1 = decl.find('"', it);
            if (q1 == std::string::npos || q1 >= blkClose) break;
            size_t q2 = decl.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= blkClose) break;
            std::string ref = decl.substr(q1 + 1, q2 - q1 - 1);
            if (present.insert(ref).second) refs.push_back(ref);
            p = q2 + 1;
        }
    }
    for (size_t i = 0; i < ids.size(); i++)
        if (present.insert(ids[i]).second) refs.push_back(ids[i]);
    if (refs.empty()) return std::string();

    /* build the block body: item[K] = "ref"; ... num = N; (decl syntax, tab-indented). */
    std::string body;
    for (size_t k = 0; k < refs.size(); k++)
        body += "\t\titem[" + std::to_string(k) + "] = \"" + refs[k] + "\";\n";
    body += "\t\tnum = " + std::to_string(refs.size()) + ";\n";

    if (blkOpen != std::string::npos)                        /* REPLACE the existing block's interior */
        return decl.substr(0, blkOpen + 1) + "\n" + body + "\t" + decl.substr(blkClose);
    /* INSERT a fresh `<path> = { ... }` just before edit{}'s closing brace. */
    std::string blk = "\t" + path + " = {\n" + body + "\t}\n";
    return decl.substr(0, ec) + blk + decl.substr(ec);
}

/* Resolve a handler's operand arg (argv[1]) -> the operand ids, reproducing OG FUN_180001fa0 (the resolver
 * shared by bss/bsi/bsf/bsb, bsin/bscls/bsincls, mkcmd and popsel). The OG contract: numeric stacks are
 * ONE-SHOT SCRATCH -- every apply/use op DRAINS the stack -- while named GROUPS are the persistent, reusable
 * COPY source:
 *   - a GROUP NAME (first char a letter AND an existing group) -> COPY the group (left intact, reusable).
 *   - else a STACK INDEX -> MOVE OUT (CONSUME) stack[N], leaving it EMPTY.
 * (The clone previously returned g_stacks.get() -- a non-consuming COPY -- for EVERY op, so a numeric stack
 * was never drained by use; dedup-on-push then blocked re-adding the same entities, the receiver/targets
 * piled up, and the stack became permanently unusable -- the tester's "must use a brand new stack / pushed 0
 * on the second go / push-to-stack-0 stops working / eventually all stacks stop" cluster.) */
static std::vector<int> resolve_operand_consume(int argc, const char **argv)
{
    const char *arg = arg_at(argc, argv, 1);
    if (arg && std::isalpha((unsigned char)arg[0]) && g_groups.has(std::string(arg)))
        return g_groups.get(std::string(arg));           /* GROUP: copy (preserved) */
    return g_stacks.move_out(parse_stack_index(arg));    /* STACK INDEX: consume (emptied) */
}

/* --- bss/bsi/bsf/bsb shared body: per id serialize -> patch the typed leaf -> collect; schedule the batch.
 * argv: [0]=op [1]=stack [2]=propPath [3]=value. The per-op difference is ONLY the leaf encoding. --- */
static void do_bulkset(sh_iface *iface, const char *op, int argc, const char **argv)
{
    const char *prop = arg_at(argc, argv, 2);
    const char *val  = arg_at(argc, argv, 3);
    if (!prop || !val) { iface_toast(iface, "SnapStack", "usage: <op> <stack> <path> <value>"); return; }
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "no entities on the stack"); return; }

    std::string leaf = encode_bulkset_leaf_json(op, std::string(val));
    /* one pass: serialize+patch each id, keeping (id, patched-text) together. ptexts owns the strings the
     * item.text pointers reference; reserve it to ids.size() so no reallocation invalidates a pointer. The
     * backend deep-copies in slot_schedule_apply, so ptexts/items may go out of scope after the schedule. */
    std::vector<std::string> ptexts;
    std::vector<sh_apply_item> items;
    ptexts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        std::string full = iface_serialize_entity(iface, ids[i]);
        if (full.empty()) continue;
        std::string patched = patch_full_json_edit(full, std::string(prop), leaf);
        if (patched.empty()) continue;
        ptexts.push_back(patched);
        sh_apply_item it; it.kind = 0; it.id = ids[i]; it.text = ptexts.back().c_str();
        items.push_back(it);
    }
    if (items.empty()) { iface_toast(iface, "SnapStack", "serialize/patch produced no apply"); return; }
    if (!iface_apply(iface, items, op)) {   /* +0x290 SYNCHRONOUS inline (OG-faithful); deferred fallback */
        char t[96]; _snprintf_s(t, sizeof t, _TRUNCATE, "%s: apply failed (editor down?)", op);
        iface_toast(iface, "SnapStack", t);
    }
}

static void h_bss(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bss", argc, argv); }
static void h_bsi(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsi", argc, argv); }
static void h_bsf(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsf", argc, argv); }

/* bsb (0x2de8): bulk-set BOOL. FAITHFUL: the OG carries a leftover MessageBoxA("FUCK","fuckedy") on its
 * re-resolve mismatch path. We reproduce it (flagged for parity) -- gated behind the same condition
 * (a serialize/patch failure on an id that previously resolved), so it fires only on the OG's mismatch case
 * and never in the success path. Reproduced faithfully (a deliberate OG quirk); the later-fix pass strips it. */
static void h_bsb(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *prop = arg_at(argc, argv, 2);
    const char *val  = arg_at(argc, argv, 3);
    if (!prop || !val) { iface_toast(iface, "SnapStack", "usage: bsb <stack> <path> true|false"); return; }
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "no entities on the stack"); return; }

    std::string leaf = encode_bulkset_leaf_json("bsb", std::string(val));
    std::vector<std::string> ptexts;
    std::vector<sh_apply_item> items;
    ptexts.reserve(ids.size());
    bool re_resolve_mismatch = false;
    for (size_t i = 0; i < ids.size(); i++) {
        std::string full = iface_serialize_entity(iface, ids[i]);
        if (full.empty()) { re_resolve_mismatch = true; continue; }   /* an id that won't re-resolve */
        std::string patched = patch_full_json_edit(full, std::string(prop), leaf);
        if (patched.empty()) { re_resolve_mismatch = true; continue; }
        ptexts.push_back(patched);
        sh_apply_item it; it.kind = 0; it.id = ids[i]; it.text = ptexts.back().c_str();
        items.push_back(it);
    }
    /* FIX: OG fired a debug MessageBoxA("FUCK","fuckedy") here (chrispy crud). The re-resolve mismatch
     * IS a real signal (a property/value that didn't round-trip), so surface it -- but via a clean non-modal
     * toast, not an intrusive debug box. */
    if (re_resolve_mismatch) {
        iface_toast(iface, "SnapStack", "bsb: some entities skipped (property/value re-resolve mismatch)");
    }
    if (!items.empty() && !iface_apply(iface, items, "bsb"))   /* +0x290 SYNCHRONOUS inline; deferred fallback */
        iface_toast(iface, "SnapStack", "bsb: apply failed (editor down?)");
    else if (items.empty())
        iface_toast(iface, "SnapStack", "bsb: serialize/patch produced no apply");
}

/* bse (0x2720): pop LAST id -> its id-STRING; for EACH remaining id set state.edit.<userPath> = that
 * id-string (a JSON string leaf -- delegates to the SAME bss apply, value = the popped id-string). >=2 ids. */
static void h_bse(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *prop = arg_at(argc, argv, 2);
    if (!prop) { iface_toast(iface, "SnapStack", "usage: bse <stack> <path>"); return; }
    /* bse/accl/acctargets are STACK-ONLY (no group operand) + CONSUME the whole stack: OG pops the last id
     * (end -= 4) then FUN_180001d84 move_out's the remainder -> stack left EMPTY. Peek the size first (OG
     * guards before touching the store), then drain via move_out. */
    int index = parse_stack_index(arg_at(argc, argv, 1));
    if ((int)g_stacks.get(index).size() < 2) { iface_toast(iface, "SnapStack", "bse needs >= 2 ids on the stack"); return; }
    std::vector<int> ids = g_stacks.move_out(index);     /* CONSUME the stack (OG-faithful) */
    int popped = ids.back();
    std::vector<int> remaining(ids.begin(), ids.end() - 1);
    std::string poppedStr = iface_id_string(iface, popped);
    std::string leaf = encode_bulkset_leaf_json("bss", poppedStr);   /* the popped id-string as a JSON string */

    std::vector<std::string> ptexts;
    std::vector<sh_apply_item> items;
    ptexts.reserve(remaining.size());
    for (size_t i = 0; i < remaining.size(); i++) {
        std::string full = iface_serialize_entity(iface, remaining[i]);
        if (full.empty()) continue;
        std::string patched = patch_full_json_edit(full, std::string(prop), leaf);
        if (patched.empty()) continue;
        ptexts.push_back(patched);
        sh_apply_item it; it.kind = 0; it.id = remaining[i]; it.text = ptexts.back().c_str();
        items.push_back(it);
    }
    if (items.empty()) { iface_toast(iface, "SnapStack", "bse: produced no apply"); return; }
    if (!iface_apply(iface, items, "bse"))   /* +0x290 SYNCHRONOUS inline (OG-faithful); deferred fallback */
        iface_toast(iface, "SnapStack", "bse: apply failed (editor down?)");
}

/* accl/acctargets shared body (0x2498 / 0x228c): pop LAST id (=RECEIVER); build a num/item[] LIST of ALL
 * remaining ids' id-strings at state.edit.<path>; apply on the POPPED id. acctargets HARDCODES the path
 * "targets" (entityDef.state.edit.targets). >=2 ids. */
static void do_acc(sh_iface *iface, const char *op, int argc, const char **argv, bool hardcoded_targets)
{
    const char *userPath = arg_at(argc, argv, 2);
    if (!hardcoded_targets && !userPath) { iface_toast(iface, "SnapStack", "usage: accl <stack> <path>"); return; }
    /* STACK-ONLY + CONSUME the whole stack (OG pops the last id then move_out's the rest -> stack emptied).
     * Peek size first, then drain via move_out -- the receiver (last) is the apply target, the rest the list. */
    int index = parse_stack_index(arg_at(argc, argv, 1));
    if ((int)g_stacks.get(index).size() < 2) { char t[64]; _snprintf_s(t, sizeof t, _TRUNCATE, "%s needs >= 2 ids", op);
        iface_toast(iface, "SnapStack", t); return; }
    std::vector<int> ids = g_stacks.move_out(index);     /* CONSUME the stack (OG-faithful) */
    int popped = ids.back();
    std::vector<int> remaining(ids.begin(), ids.end() - 1);
    std::string path = hardcoded_targets ? std::string("targets") : std::string(userPath);

    /* FIX (exceeds OG): drop stack ids that are no longer VALID before listing them. The entity-stack
     * is NOT pruned when an entity is deleted, so a stale/deleted id (e.g. a barrel the user just removed)
     * would otherwise resolve to a dangling id-string -- a bare decimal once even its path is gone -- and get
     * written as a junk target (the user's "3 entities, some make no sense" report: a live remove plus two
     * deleted barrels). Unlike the bss/bse serialize path, iface_id_string NEVER fails, so there is no implicit
     * skip; validate explicitly via the +0x28 is-valid slot (same check the read-sync uses to detect deletes). */
    std::vector<std::string> idStrings;
    idStrings.reserve(remaining.size());
    int skipped = 0;
    for (size_t i = 0; i < remaining.size(); i++) {
        if (!iface_is_valid_id(iface, remaining[i])) { skipped++; continue; }   /* stale/deleted -> not a target */
        idStrings.push_back(iface_id_string(iface, remaining[i]));
    }
    if (idStrings.empty()) {
        char t[112]; _snprintf_s(t, sizeof t, _TRUNCATE,
            "%s: no VALID targets on the stack (all %d were stale/deleted -- clear the stack with cstk)",
            op, (int)remaining.size());
        iface_toast(iface, "SnapStack", t); return;
    }
    if (skipped > 0) {
        char t[112]; _snprintf_s(t, sizeof t, _TRUNCATE,
            "%s: skipped %d stale/deleted id(s); listed %d valid target(s)", op, skipped, (int)idStrings.size());
        iface_toast(iface, "SnapStack", t);
    }

    /* APPLY the OG-FAITHFUL way: the exact temp-def round-trip OG uses (serialize entity JSON -> patch the
     * num/item[] list -> deserialize onto a temp def -> DeclSourceRebuild + IdStrAssign = ae_apply_one), but
     * committed SYNCHRONOUSLY INLINE on THIS (UI/think-loop) thread via +0x290 -- exactly like OG's acctargets
     * handler (FUN_18000228c) commits its +0xd0 (FUN_180004b80) inline. This is the REAL fix (2026-07-12):
     * OG's commit is byte-identical to ours and OG runs it on this same UI thread, so the round-trip was never
     * wrong -- the bug was our DEFERRAL (serialize on UI thread, commit a frame later on the DOOM main thread),
     * which double-owned the committed decl-source block -> the play->teardown double-free. Committing inline
     * gives the block one clean owner. (An OG-diverging decl-source splice via +0x40 also works -- kept as
     * splice_decl_reflist for reference -- but the inline commit keeps us faithful to OG and fixes bss/bse/
     * timeline the same way.) iface_apply_sync returns -1 only on an OLD backend without +0x290 -> deferred
     * fallback. See scratchpad og_baseline_entity63.md + [[snapstack-architecture-mismatch]]. */
    std::string full = iface_serialize_entity(iface, popped);      /* +0xc8 serialize (UI thread, works) */
    if (full.empty()) { iface_toast(iface, "SnapStack", "accl: receiver serialize failed"); return; }
    std::string patched = patch_full_json_reflist(full, path, idStrings);
    if (patched.empty()) { iface_toast(iface, "SnapStack", "accl: list patch failed"); return; }

    sh_apply_item it; it.kind = 0; it.id = popped; it.text = patched.c_str();
    std::vector<sh_apply_item> items(1, it);
    if (!iface_apply(iface, items, op))   /* +0x290 SYNCHRONOUS inline commit (OG-faithful); deferred fallback */
        iface_toast(iface, "SnapStack", "accl: apply failed (editor down?)");
}
static void h_accl(void *ctx, int argc, const char **argv)       { do_acc((sh_iface *)ctx, "accl", argc, argv, /*hardcoded=*/false); }
static void h_acctargets(void *ctx, int argc, const char **argv) { do_acc((sh_iface *)ctx, "acctargets", argc, argv, /*hardcoded=*/true); }

/* mkcmd (0x3744): synthesize an idSnapEntityPrefab command-entity into the editor+0x209a8 paste slot. Per
 * id: "ai_ScriptCmdEnt " + template.replace('$', idString), joined with ';' (trailing ';'). The whole
 * commandText is spliced into the byte-exact prefab template at the placeholder; the prefab text is
 * scheduled (kind=1) so the backend deserializes it as "idSnapEntityPrefab" into editor+0x209a8. The
 * READ-BACK (read_prefab) verifies the +0x209a8 offset on this build. argv: [0]=mkcmd [1]=stack [2]=template
 * (optional; default "$"). */
static void h_mkcmd(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "mkcmd: no entities on the stack"); return; }
    const char *tmpl_arg = arg_at(argc, argv, 2);
    std::string tmpl = (tmpl_arg && tmpl_arg[0]) ? std::string(tmpl_arg) : std::string("$");

    /* build the commandText macro: per id, "ai_ScriptCmdEnt " + tmpl with every '$' -> idString; join ';'. */
    std::string commandText;
    for (size_t i = 0; i < ids.size(); i++) {
        std::string idStr = iface_id_string(iface, ids[i]);
        std::string part = "ai_ScriptCmdEnt ";
        for (size_t c = 0; c < tmpl.size(); c++) {
            if (tmpl[c] == '$') part += idStr; else part.push_back(tmpl[c]);
        }
        commandText += part;
        commandText += ';';   /* OG appends ';' after EACH part -> trailing ';' */
    }

    /* splice commandText into the byte-exact prefab template at the placeholder. The template is loaded from
     * the committed the prefab template (prefix @0x1ca30 + suffix @0x1cb50). */
    std::string prefab = sh_load_mkcmd_template();
    const std::string ph = "__SNAPHAK_MKCMD_COMMANDTEXT__";
    size_t pos = prefab.find(ph);
    if (pos == std::string::npos) { iface_toast(iface, "SnapStack", "mkcmd: template missing placeholder"); return; }
    prefab.replace(pos, ph.size(), commandText);

    sh_apply_item it; it.kind = 1; it.id = 0; it.text = prefab.c_str();   /* kind 1 = mkcmd prefab paste */
    std::vector<sh_apply_item> items(1, it);
    if (!iface_schedule_apply(iface, items, "mkcmd"))
        iface_toast(iface, "SnapStack", "mkcmd: schedule failed (editor down?)");
}

/* ============================================================ class/inherit-CHANGE handlers (un-deferred)
 * bscls / bsin / bsincls: the in-place className/inherit SETTERS over the stack. UN-DEFERRED 2026-06-22 --
 * the engine's in-place class-change re-resolve cascade is FINITE (it COMPLETES; heavy/slow on large maps,
 * not a hang -- truth class-inherit-change-freeze.md, OVERTURNED). Each is a faithful port of the OG
 * snaphakui.dll handlers via the ALREADY-RE'd setter slots (+0x78 set class / +0x80 set inherit / +0x30 get
 * decl-source / +0x40 DeclSourceRebuild), matching the bss/bse stack-iteration + interface-slot convention:
 *
 *   FUN_180001244(id, newClassNameStr)   [the className path -- LIGHT, no parent re-resolve]:
 *       +0x78 set className   ->  r = +0x30 get decl-source  ->  +0x40 DeclSourceRebuild(r)   [ALWAYS rebuilds]
 *   FUN_1800012dc(id, newInheritStr, flag) [the inherit path -- HEAVY, re-resolves the new parent decl]:
 *       +0x80 set inherit  ->  if (flag) { r = +0x30 get decl-source ; +0x40 DeclSourceRebuild(r) }
 *
 *   bscls   = FUN_180001244(id, newClassName)                                  [set class + 1 rebuild]
 *   bsin    = FUN_1800012dc(id, newInherit, 0x01)                              [set inherit + 1 rebuild]
 *   bsincls = FUN_1800012dc(id, newInherit, 0x00)  THEN  FUN_180001244(id, newClassName)
 *             [set inherit, SUPPRESS the intermediate rebuild; the className setter does the ONE rebuild that
 *              re-emits BOTH new fields from the just-updated defsub -- one cascade carrying both changes]
 *
 * The VALUE arg is the new className/inherit string (`sh bscls <stack> <newClassName>` etc.). The setter
 * slots no-op on an empty string (OG-faithful), so a missing value cleanly does nothing. NO QJson/serialize
 * round-trip (that is the bss/bse APPLY family's path) -- this is the direct setter-slot path, exactly like
 * the OG handlers + the Save-to-Decl |2 consumer (sh_tabs.cpp apply_entity_state). Each engine touch is
 * SEH-guarded in the backend slot body, so the heavy main-thread cascade can never crash the frontend. */

/* FUN_180001244(id, cls): set className (+0x78), then re-emit decl-source (+0x30 -> +0x40). ALWAYS rebuilds. */
static void do_set_classname_one(sh_iface *iface, int id, const std::string &cls)
{
    iface_set_classname(iface, id, cls);                          /* +0x78 set className */
    std::string r = iface_declsource_text(iface, id);            /* r = +0x30 get decl-source text */
    iface_rebuild_declsource(iface, id, r);                       /* +0x40 DeclSourceRebuild -- its re-parse REVERTS +0x60 (last-wins from the appended old src) */
    iface_set_classname(iface, id, cls);                          /* RE-ASSERT (+0x78) -- the explicit class wins over the rebuild's revert (declsource-rebuild-trace) */
}

/* FUN_1800012dc(id, inh, flag): set inherit (+0x80); if flag != 0, re-emit decl-source (+0x30 -> +0x40). */
static void do_set_inherit_one(sh_iface *iface, int id, const std::string &inh, bool rebuild)
{
    iface_set_inherit(iface, id, inh);                            /* +0x80 set inherit */
    if (rebuild) {
        std::string r = iface_declsource_text(iface, id);        /* r = +0x30 get decl-source text */
        iface_rebuild_declsource(iface, id, r);                  /* +0x40 DeclSourceRebuild -- its re-parse REVERTS +0x58 (last-wins) */
        iface_set_inherit(iface, id, inh);                       /* RE-ASSERT (+0x80) -- the explicit inherit wins over the rebuild's revert (declsource-rebuild-trace) */
    }
}

/* bscls <stack> <newClassName>: per stack id, set className + rebuild (the light path -- no parent re-resolve). */
static void h_bscls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *cls = arg_at(argc, argv, 2);
    if (!cls || !cls[0]) { iface_toast(iface, "SnapStack", "usage: bscls <stack> <className>"); return; }
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "no entities on the stack"); return; }
    std::string clsStr(cls);
    int n = 0;
    for (size_t i = 0; i < ids.size(); i++) { do_set_classname_one(iface, ids[i], clsStr); n++; }
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE, "bscls: set className=%s on %d entities", clsStr.c_str(), n);
    iface_toast(iface, "SnapStack", text);
}

/* bsin <stack> <newInherit>: per stack id, set inherit + rebuild (the heavy path -- re-resolves the new parent). */
static void h_bsin(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *inh = arg_at(argc, argv, 2);
    if (!inh || !inh[0]) { iface_toast(iface, "SnapStack", "usage: bsin <stack> <inherit>"); return; }
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "no entities on the stack"); return; }
    std::string inhStr(inh);
    int n = 0;
    for (size_t i = 0; i < ids.size(); i++) { do_set_inherit_one(iface, ids[i], inhStr, /*rebuild=*/true); n++; }
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE, "bsin: set inherit=%s on %d entities", inhStr.c_str(), n);
    iface_toast(iface, "SnapStack", text);
}

/* bsincls <stack> <newInherit> <newClassName>: per stack id, set inherit (SUPPRESS its rebuild) THEN set
 * className (the className setter does the ONE rebuild carrying BOTH new fields). argv: [2]=inherit [3]=class. */
static void h_bsincls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *inh = arg_at(argc, argv, 2);
    const char *cls = arg_at(argc, argv, 3);
    if (!inh || !inh[0] || !cls || !cls[0]) {
        iface_toast(iface, "SnapStack", "usage: bsincls <stack> <inherit> <className>"); return;
    }
    std::vector<int> ids = resolve_operand_consume(argc, argv);   /* CONSUME the stack (OG drains on use) */
    if (ids.empty()) { iface_toast(iface, "SnapStack", "no entities on the stack"); return; }
    std::string inhStr(inh), clsStr(cls);
    int n = 0;
    for (size_t i = 0; i < ids.size(); i++) {
        /* atomic FINAL-pair set (+0x268) -> the ONE rebuild. Fixes the cross-family morph the legacy
         * two-call sequence rejected at its invalid intermediate (the per-slot guard saw new-class vs old-
         * inherit). r==0 = the FINAL pair is the fatal combo -> leave unchanged + SKIP the rebuild. */
        int r = iface_apply_class_inherit(iface, ids[i], clsStr, inhStr);   /* +0x268 atomic (pass 1) */
        if (r == 1) {
            std::string rsrc = iface_declsource_text(iface, ids[i]);        /* +0x30 get decl-source */
            iface_rebuild_declsource(iface, ids[i], rsrc);                  /* +0x40 rebuild -- its re-parse REVERTS defsub+0x60/+0x58 (DeclSourceRebuild re-emits a NEW header then appends the OLD src; the re-parse's keyed class/inherit lookup is LAST-WINS so the appended old class= overrides). declsource-rebuild-trace. */
            iface_apply_class_inherit(iface, ids[i], clsStr, inhStr);       /* RE-ASSERT (pass 2, the OG Save-to-Decl/+0xd0 pattern) -- the explicit pair WINS over the rebuild's revert; this is what makes the morph stick in defsub+0x60/+0x58 (the persisted form). */
        } else if (r == -1) {                                              /* slot absent (old backend) -> legacy */
            do_set_inherit_one(iface, ids[i], inhStr, /*rebuild=*/false);
            do_set_classname_one(iface, ids[i], clsStr);
        }
        n++;
    }
    char text[200];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "bsincls: set inherit=%s className=%s on %d entities", inhStr.c_str(), clsStr.c_str(), n);
    iface_toast(iface, "SnapStack", text);
}

/* ============================================================ the registrar (FUN_180003c80) ======== */
/* Register all 20 SnapStack subcommands on the interface (+0x188). Order mirrors the OG registrar's 20
 * calls. The 9 store-ops carry REAL handlers; the 8 apply-ops carry the 8-pass apply chain; bsin/bscls/bsincls carry
 * the REAL class/inherit-change setters (un-deferred -- the OG handler order via the +0x78/+0x80/+0x30/+0x40
 * slots). Each handler's ctx = the interface (so it reaches the engine-touch slots + the
 * shared stores). The cmd-map ends with the full 20 (OG-faithful completeness). */
struct sh_subcommand { const char *name; sh_cmd_handler handler; };

static const sh_subcommand SNAPSTACK_COMMANDS[] = {
    /* --- the 9 REAL store-ops --- */
    { "psel",       h_psel },
    { "popsel",     h_popsel },
    { "phov",       h_phov },
    { "cstk",       h_cstk },
    { "pr",         h_pr },
    { "pg",         h_pg },
    { "pop2g",      h_pop2g },
    { "filtinh",    h_filtinh },
    { "filtcls",    h_filtcls },     /* reuses the "had inherit" toast (OG mislabel, faithful) */
    /* --- the 8 apply ops (the full-entity 8-pass round-trip; serialize+patch here, deserialize+
     *     commit at the engine command-exec point via the backend's clone_bss_apply) --- */
    { "bss",        h_bss },
    { "bsi",        h_bsi },
    { "bsf",        h_bsf },
    { "bsb",        h_bsb },
    { "bse",        h_bse },
    { "accl",       h_accl },
    { "acctargets", h_acctargets },
    { "mkcmd",      h_mkcmd },
    /* --- the 3 class/inherit-CHANGE setters (un-deferred 2026-06-22; OG FUN_180001244/FUN_1800012dc port
     *     via the +0x78/+0x80/+0x30/+0x40 setter slots; the cascade is FINITE, not a hang) --- */
    { "bsin",       h_bsin },
    { "bscls",      h_bscls },
    { "bsincls",    h_bsincls },
};
static const int SNAPSTACK_COMMAND_COUNT =
    (int)(sizeof(SNAPSTACK_COMMANDS) / sizeof(SNAPSTACK_COMMANDS[0]));   /* == 20 */

void sh_register_snapstack_commands(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->register_cmd) return;
    for (int i = 0; i < SNAPSTACK_COMMAND_COUNT; i++)
        iface->vtbl->register_cmd(iface, SNAPSTACK_COMMANDS[i].name,
                                  SNAPSTACK_COMMANDS[i].handler, /*ctx=*/iface);
}

/* the Entities ctx-menu "Push to stack 0" reaches the SHARED store (g_stacks) the ops mutate. */
void sh_snapstack_push_one(int index, int id)
{
    if (index < 0) index = 0;
    std::vector<int> one;
    one.push_back(id);
    g_stacks.push(index, one);
}

/* batch form: push the whole LIST SELECTION onto stack `index` (dedup), faithful to OG FUN_180018154 which
 * loops QListWidget::selectedItems() pushing each onto stack 0 -- so "Push to stack 0" stacks the SELECTION,
 * not just the right-clicked row. */
void sh_snapstack_push_ids(int index, const std::vector<int> &ids)
{
    if (index < 0) index = 0;
    if (!ids.empty()) g_stacks.push(index, ids);
}
