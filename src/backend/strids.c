/* strids.c -- see strids.h. The custom #str_ string injector (port of OG FUN_18000FF10 +
 * FUN_1800102e0).
 *
 * On the first top-level engine idLangDict sort, we load %USERPROFILE%\snaphak\strings\strids.json,
 * parse its flat {id: text} object, and for each row append a 32-byte idLangDict entry to the live
 * string table (the same record + the same engine fns the engine's own lang loader uses), then let the
 * real sort run so our rows sort into place. Every later call (incl. the sort's own recursion) passes
 * straight through (one-shot latch + recursion guard) so we never duplicate rows.
 *
 * The 32-byte record (DIRECT, engine insert FUN_141a29980 + OG FUN_18000FF10):
 *   +0x00  u32  hash       = engine idStr::Hash("#str_<id>")  (FNV-1a, lowercased)
 *   +0x04  u32  pad
 *   +0x08  ptr  keyHandle  = engine idStr-pool intern of "#str_<id>"
 *   +0x10  ptr  valHandle  = engine idStr-pool intern of the value text
 *   +0x18  u32  valLen     = strlen(value)   (OG mirrors the value length here)
 *   +0x1c  u32  valLen2    = strlen(value)
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "strids.h"
#include "strids_baked.h"   /* the compiled-in canonical #str_ set (baked strids) */
#include "hook.h"
#include "backend_log.h"

/* StridsSortBody prologue steal window (DIRECT, disasm of 0x1a2b490):
 *   48 89 5C 24 18        mov [rsp+0x18],rbx        (5)
 *   55                    push rbp                  (1)
 *   56                    push rsi                  (1)
 *   57                    push rdi                  (1)
 *   48 8D AC 24 10 F8..   lea rbp,[rsp-0x7f0]       (8)  -- rsp-relative, position-independent
 * = 16 bytes of whole, register/rsp-only, position-independent instructions (no RIP-rel, no rel jmp). */
#define SORT_STOLEN 16

/* The engine sort body prototype: void sort(void* ctx, void* arr, uint32_t count, uint32_t radix).
 * From OG FUN_1800102e0: (base+0x1a2b490)(param_1, table[0], table_count, 0x20). */
typedef void (*sort_fn_t)(void *ctx, void *arr, uint32_t count, uint32_t radix);

/* The radix the engine always passes to the sort body. DIRECT: the sort WRAPPER (0x1a2b480) is just
 * `MOV R9D,0x20 ; JMP 0x1a2b490` -- it hard-sets the 4th arg (radix) to 0x20 and tail-jumps the body,
 * which radix-sorts the 32-bit hash key 8 bits at a time (param_4 bits, recursing param_4-8). */
#define SORT_RADIX 0x20

/* The engine fns the inject calls. All resolved by the signature scanner; never hardcoded RVAs. */
typedef int   (*insert_fn_t)(void *table_desc, void *record32);   /* idList<StridEntry>::Append */
typedef uint32_t (*hash_fn_t)(const char *s);                     /* idStr::Hash (FNV-1a, lowercased) */
typedef void  (*idstr_ctor_fn_t)(void *out_handle, const char *s);/* *out_handle = pool_intern(s) */

static sort_fn_t       g_sort_orig   = NULL;   /* trampoline -> the real engine sort body */
static void           *g_table_desc  = NULL;   /* the idLangDict table descriptor (.data global) */
static insert_fn_t     g_insert      = NULL;
static hash_fn_t       g_hash        = NULL;
static idstr_ctor_fn_t g_idstr_ctor  = NULL;

static volatile LONG g_injected      = 0;      /* one-shot latch (0 = not yet injected) */
static volatile LONG g_in_sort       = 0;      /* recursion guard (>0 = inside the sort already) */
static volatile LONG g_inject_count  = 0;      /* rows appended (observability) */

/* strids.json source. Default mirrors OG's path (%USERPROFILE%\snaphak\strings\strids.json). */
static char g_src_path[MAX_PATH] = {0};

static void default_source_path(char *out, size_t cap)
{
    char profile[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snaphak\\strings\\strids.json", profile);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snaphak\\strings\\strids.json");
}

/* ----------------------------------------------------------------- table-global LEA decode --------
 * The table descriptor is a .data global (can't be sig-scanned -- it's not code). The engine's
 * idLangDict::GetIndexForId (resolved as StridsTableLea) starts with a `LEA RCX,[rip+disp32]` to that
 * global. We scan forward from the resolved fn entry for the FIRST `48 8D 0D` (LEA RCX, rip-rel) and
 * decode its rip-relative displacement to recover the global build-portably. (The fn has a second
 * `48 8D 0D` later -- the error-path string -- so we take the first.) */
#define LEA_SCAN_WINDOW 0x40

static int safe_read_n(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *decode_table_global(const uint8_t *table_lea_fn)
{
    uint8_t b[LEA_SCAN_WINDOW];
    if (!safe_read_n(table_lea_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= LEA_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && b[i + 1] == 0x8D && b[i + 2] == 0x0D) {     /* LEA RCX,[rip+disp32] */
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = table_lea_fn + i + 7;
            return (void *)(rip_next + disp);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- strids.json parsing ----------
 * strids.json is a flat JSON object: { "id1" : "text1", "id2" : "text2", ... }. We do a minimal,
 * failure-tolerant scan for "string" : "string" pairs (the only shape the format uses), unescaping the
 * value's \" \\ \n \t the same way the engine lang loader does. Keys/values are bounded; a malformed
 * file degrades to "fewer / zero rows injected", never a crash. */

/* Read the whole source file into a fresh NUL-terminated heap buffer (caller HeapFrees), or NULL. */
static char *read_source_file(size_t *out_len)
{
    char path[MAX_PATH];
    if (g_src_path[0]) strncpy_s(path, sizeof path, g_src_path, _TRUNCATE);
    else default_source_path(path, sizeof path);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)(16 * 1024 * 1024)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    size_t got = 0;
    while (got < n) {
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, (DWORD)(n - got), &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got != n) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* Scan one JSON "string" starting at *p (which must point AT the opening quote). Copies the unescaped
 * content into out[0..cap) NUL-terminated, advances *p past the closing quote. Returns out length, or
 * -1 if no well-formed string is found. */
static int scan_json_string(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    if (*s != '"') return -1;
    s++;
    size_t o = 0;
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\' && *s) {
            char e = *s++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                default:  c = e;    break;   /* unknown escape -> literal (engine warns; we keep it) */
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    if (*s != '"') return -1;   /* unterminated */
    s++;
    out[o] = '\0';
    *p = s;
    return (int)o;
}

/* Dedup within one inject pass: a key must NEVER be appended twice -- duplicate keys corrupt the engine's
 * sorted-by-hash string dictionary (lookups collapse -> wrong/missing strings). FIRST-writer-wins: do_inject
 * injects the user's strids.json FIRST and the baked defaults second, so for a key the user defines their
 * value WINS and the baked duplicate is skipped (a user's explicit override beats our default, matching the
 * decl file-shadow). Case-insensitive (the engine lowercases the #str_ hash). */
#define STRIDS_DEDUP_CAP 512
static char g_injected_ids[STRIDS_DEDUP_CAP][96];
static int  g_injected_n;

static int already_injected(const char *id)
{
    for (int i = 0; i < g_injected_n; i++)
        if (_stricmp(g_injected_ids[i], id) == 0) return 1;
    return 0;
}

/* Append one #str_<id> -> text row to the live table via the engine fns. Skips a key already injected
 * this pass (baked-wins dedup). */
static void inject_row(const char *id, const char *text, size_t text_len)
{
    if (already_injected(id)) return;                       /* first-writer-wins dedup: never append twice */
    if (g_injected_n < STRIDS_DEDUP_CAP)
        strncpy_s(g_injected_ids[g_injected_n++], sizeof g_injected_ids[0], id, _TRUNCATE);

    char key[256];
    _snprintf_s(key, sizeof key, _TRUNCATE, "#str_%s", id);

    /* 32-byte record: { u32 hash; u32 pad; ptr keyHandle; ptr valHandle; u32 len; u32 len } */
    uint8_t rec[32];
    memset(rec, 0, sizeof rec);
    uint32_t h = g_hash(key);
    memcpy(rec + 0x00, &h, 4);
    g_idstr_ctor(rec + 0x08, key);    /* keyHandle = intern("#str_<id>") */
    g_idstr_ctor(rec + 0x10, text);   /* valHandle = intern(text)        */
    uint32_t vl = (uint32_t)text_len;
    memcpy(rec + 0x18, &vl, 4);
    memcpy(rec + 0x1c, &vl, 4);

    g_insert(g_table_desc, rec);      /* idList<StridEntry>::Append(tableDesc, &record) */
    InterlockedIncrement(&g_inject_count);
}

/* Load + inject all rows from strids.json. Returns the count injected. Failure-tolerant. */
static long do_inject(void)
{
    if (g_table_desc == NULL || g_insert == NULL || g_hash == NULL || g_idstr_ctor == NULL)
        return 0;

    g_injected_n = 0;   /* fresh dedup set for this inject pass */

    /* (1) USER rows FIRST -- the user's strings\strids.json is their EXPLICIT override layer, so it WINS
     * (same precedent as the decl file-shadow: a user's own file beats our baked default). A key the user
     * defines is injected + recorded here; the baked default for that key is then SKIPPED in (2). */
    size_t len = 0;
    char *buf = read_source_file(&len);
    if (buf != NULL) {
        const char *p = buf;
        char id[256], text[4096];
        /* Walk "key" : "value" pairs. Anything that isn't a well-formed quoted string is skipped over a
         * char at a time, so braces/commas/whitespace/comments don't trip the scan. */
        while (*p) {
            if (*p != '"') { p++; continue; }
            int idlen = scan_json_string(&p, id, sizeof id);
            if (idlen < 0) { p++; continue; }
            /* expect a ':' (skip ws) then the value string */
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p != ':') continue;     /* not a key:value pair -- resume scanning from here */
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p != '"') continue;
            int vlen = scan_json_string(&p, text, sizeof text);
            if (vlen < 0) continue;
            if (idlen > 0) inject_row(id, text, (size_t)vlen);
        }
        HeapFree(GetProcessHeap(), 0, buf);
    } else {
        backend_log("B1: strids -- no user strids.json (optional); baked defaults cover the shipped pack");
    }

    /* (2) BAKED defaults: fill every shipped key the user did NOT override (the dedup skips a key the json
     * already injected in (1)), so the "*Custom" tab + Timeline/Unknown strings ALWAYS resolve -- including
     * on a clean setup with no strids.json. */
    for (size_t bi = 0; bi < B1_STRIDS_BAKED_COUNT; bi++)
        inject_row(g_strids_baked[bi].id, g_strids_baked[bi].text, strlen(g_strids_baked[bi].text));

    return (long)InterlockedCompareExchange(&g_inject_count, 0, 0);
}

/* Re-run the engine radix sort over the WHOLE live table so the rows we just appended sort into place.
 * MANDATORY (not optional): the engine #str_ lookup BINARY-SEARCHES the table on the +0x00 hash key
 * (DIRECT, FUN_141a2aa90 -- the comparator behind both idLangDict::GetIndexForId variants 0x1a29c20/
 * 0x1a29cb0), so an unsorted appended row is invisible until the table is re-sorted ascending-by-hash.
 * We call the engine sort BODY through the trampoline (g_sort_orig) exactly as the engine loader
 * does (FUN_141a2a050: sortWrapper(&cmp, table[0], count) -> body(ctx, arr, count, 0x20)). The body's
 * ctx (param_1) is inert for the radix sort itself (only threaded into its recursion), so we pass the
 * table descriptor as a valid ctx pointer. Reads the live array/count from the descriptor (+0 array,
 * +8 count -- DIRECT, FUN_141a29980 idList layout). SEH-guarded: a bad read => skip the sort, no crash. */
static void resort_table(void)
{
    if (g_sort_orig == NULL || g_table_desc == NULL) return;
    __try {
        void    *live_arr = *(void **)g_table_desc;
        uint32_t live_cnt = *(uint32_t *)((uint8_t *)g_table_desc + 8);
        if (live_arr != NULL && live_cnt > 1)
            g_sort_orig(g_table_desc, live_arr, live_cnt, SORT_RADIX);
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* bad descriptor read -> leave the table as-is */ }
}

/* Inject our rows + re-sort the table, EXACTLY ONCE (one-shot latch). Shared by the install-time direct
 * path and the sort detour. Logs the "B1: strids injected N #str_ entries" marker. Returns the count
 * injected, or -1 if another caller already latched the inject (so the caller can stay quiet). */
static long inject_and_resort_once(void)
{
    if (InterlockedCompareExchange(&g_injected, 1, 0) != 0)
        return -1;   /* already injected by the other path -- do nothing, no double rows */

    long n = do_inject();
    resort_table();

    char line[96];
    _snprintf_s(line, sizeof line, _TRUNCATE, "B1: strids injected %ld #str_ entries", n);
    backend_log(line);
    return n;
}

/* The detour. Kept installed as a harmless safety net: if the engine ever runs another top-level sort
 * after our install-time inject, the one-shot latch makes this a pass-through (we already injected), so
 * it never double-injects and never interferes with the sort's recursive partition calls. (The inject
 * normally happens at install time now -- see sh_strids_install -- because the lang table sorts ONCE at
 * engine startup, BEFORE our deferred install, so this detour would otherwise never fire.) */
static void sh_sort_detour(void *ctx, void *arr, uint32_t count, uint32_t radix)
{
    if (g_sort_orig == NULL) return;   /* defensive: never happens once installed */

    LONG depth = InterlockedIncrement(&g_in_sort);
    if (depth == 1 && InterlockedCompareExchange(&g_injected, 1, 0) == 0) {
        /* First top-level sort and we won the latch (no install-time inject has run -- e.g. a sort that
         * somehow precedes it): inject now so THIS very sort orders our rows in. We DON'T call
         * resort_table() here (this call IS the sort); we just append, then re-read the grown
         * array/count below so the engine sort below covers the augmented table. */
        long n = do_inject();
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE, "B1: strids injected %ld #str_ entries", n);
        backend_log(line);
        /* The table grew -- re-read the live count from the descriptor so the original sort orders the
         * FULL augmented table, not the pre-inject count it was called with. The descriptor's count is
         * the dword at +8 (idList layout: +0 array ptr, +8 num, +0xc capacity -- DIRECT, FUN_141a29980). */
        __try {
            uint32_t live = *(uint32_t *)((uint8_t *)g_table_desc + 8);
            void    *live_arr = *(void **)g_table_desc;
            if (live_arr) arr = live_arr;
            if (live)     count = live;
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep the engine's args on any read fault */ }
    }

    g_sort_orig(ctx, arr, count, radix);
    InterlockedDecrement(&g_in_sort);
}

int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn)
{
    char line[256];

    if (sort_body_fn == NULL) {
        backend_log("B1: strids injector SKIPPED -- StridsSortBody not resolved");
        return 0;
    }
    if (!sort_status_ok) {
        backend_log("B1: strids injector SKIPPED -- StridsSortBody resolved via hook-tolerant fallback "
                    "(prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (table_lea_fn == NULL || insert_fn == NULL || hash_fn == NULL || idstr_ctor_fn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: strids injector SKIPPED -- missing engine fn (lea=%p insert=%p hash=%p ctor=%p)",
            table_lea_fn, insert_fn, hash_fn, idstr_ctor_fn);
        backend_log(line);
        return 0;
    }
    if (g_sort_orig != NULL) {
        backend_log("B1: strids injector already installed");
        return 1;
    }

    g_table_desc = decode_table_global((const uint8_t *)table_lea_fn);
    if (g_table_desc == NULL) {
        backend_log("B1: strids injector SKIPPED -- could not decode the table-global LEA "
                    "(StridsTableLea layout shifted?)");
        return 0;
    }
    g_insert     = (insert_fn_t)insert_fn;
    g_hash       = (hash_fn_t)hash_fn;
    g_idstr_ctor = (idstr_ctor_fn_t)idstr_ctor_fn;

    void *tramp = install_inline_hook(sort_body_fn, (void *)sh_sort_detour, SORT_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: strids injector FAIL -- install_inline_hook returned NULL");
        g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        return 0;
    }
    g_sort_orig = (sort_fn_t)tramp;

    if (!g_src_path[0]) default_source_path(g_src_path, sizeof g_src_path);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: strids injector installed at %p (trampoline %p, stolen %d); table=%p; source=%s",
        sort_body_fn, tramp, SORT_STOLEN, g_table_desc, g_src_path);
    backend_log(line);

    /* INJECT-ON-INSTALL. The engine's idLangDict sorts its lang table exactly ONCE at startup -- well
     * BEFORE this deferred install (~5s in, after the SteamStub decrypt) -- so the detour above would
     * never fire on its own (editor entry does not re-sort). Same late-install class as the resolver
     * defer. So we run the inject DIRECTLY here, immediately, exactly as the detour body would: append
     * the #str_ rows, then RE-SORT the table (mandatory -- the engine #str_ lookup binary-searches on
     * the +0x00 hash key, DIRECT FUN_141a2aa90, so unsorted appended rows are invisible). The one-shot
     * latch in inject_and_resort_once means a later real sort (should one ever occur) won't double it.
     * Emits the "B1: strids injected N #str_ entries" marker at install time. */
    inject_and_resort_once();
    return 1;
}

int sh_strids_set_source(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        default_source_path(g_src_path, sizeof g_src_path);
        return 1;
    }
    strncpy_s(g_src_path, sizeof g_src_path, path, _TRUNCATE);
    return g_src_path[0] != '\0';
}

unsigned long sh_strids_injected_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_inject_count, 0, 0);
}
