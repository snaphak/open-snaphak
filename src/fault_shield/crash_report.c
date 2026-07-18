/* crash_report.c -- see crash_report.h.
 *
 * SAFETY MODEL (this runs while the process may be dying):
 *  - record writes use CreateFileA(CREATE_NEW) + WriteFile with WRITE_THROUGH, static buffers, no
 *    CRT heap; a same-second collision just bumps a suffix; any failure is absorbed silently.
 *  - the first-chance fatal one-shot mirrors the diagnostic build's proven pattern: HEAP_CORRUPTION
 *    (0xC0000374) and __fastfail/STACK_BUFFER_OVERRUN (0xC0000409) trap to the kernel and never
 *    reach an unhandled-exception filter on x64, so the full capture happens right there, once --
 *    the process is dying anyway, so the loader-lock cost is acceptable. The stack walk is heap-free
 *    (RtlVirtualUnwind), so it survives a corrupt heap; the dump is best-effort after it.
 *  - the unhandled-exception filter chains to the REAL previous filter, guarded so it can never
 *    chain to itself, and re-asserts for ~30s after arm (the engine installs its own top-level
 *    filter during bring-up and would displace ours -- same displacement the diagnostic build
 *    documents).
 *  - dbghelp is bound at ARM time (LoadLibrary inside a crash is off-limits).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <dbghelp.h>
#include "crash_record_format.h"
#include "crash_report.h"
#include "fault_record.h"
#include "recovery.h"    /* shield_last_engine_msg */
#include "veh.h"         /* shield_capture_stack */
#include "engine_layout.h"

extern uint8_t *g_doom_base;

#define CRASH_MAX_RECORDS 8   /* per-session cap: a fault storm can't spam pending-*.json */

typedef BOOL (WINAPI *minidump_write_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                        PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);

static char g_crash_dir[MAX_PATH] = {0};   /* <game>\snaphak\crash */
static char g_dump_path[MAX_PATH] = {0};   /* <game>\snaphak\logs\snaphak_crash.dmp */
static char g_inst_version[48]    = {0};   /* installed version at arm time */
static minidump_write_t g_minidump_write = NULL;
static volatile LONG g_records_written = 0;
static volatile LONG g_fatal_oneshot   = 0;   /* first-chance 0xC0000374/0xC0000409 full capture, once */
static volatile LONG g_uef_entered     = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = NULL;

/* record-format scratch -- static, not stack (crash contexts may have little stack left). */
static char g_rec_buf[8192];
static char g_rec_stack[2048];
static char g_rec_text[HARVEST_MSG_MAX];

static void crash_dirs_from_module(void)
{
    HMODULE self = NULL;
    char path[MAX_PATH];
    char *slash;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&crash_dirs_from_module, &self) || !self) return;
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    /* <game>\snaphak\crash (one level at a time; idempotent) + the dump path under logs\ */
    _snprintf_s(g_crash_dir, MAX_PATH, _TRUNCATE, "%ssnaphak", path);
    CreateDirectoryA(g_crash_dir, NULL);
    _snprintf_s(g_dump_path, MAX_PATH, _TRUNCATE, "%ssnaphak\\logs", path);
    CreateDirectoryA(g_dump_path, NULL);
    _snprintf_s(g_crash_dir, MAX_PATH, _TRUNCATE, "%ssnaphak\\crash", path);
    CreateDirectoryA(g_crash_dir, NULL);
    _snprintf_s(g_dump_path, MAX_PATH, _TRUNCATE, "%ssnaphak\\logs\\snaphak_crash.dmp", path);
}

/* Read "version" from %LOCALAPPDATA%\open-snaphak\install.json (the installer's manifest -- the same
 * source the UI reads). Best-effort: absent/malformed -> "" (the record just omits it). */
static void crash_read_version(void)
{
    char la[MAX_PATH], path[MAX_PATH], data[4096];
    HANDLE h;
    DWORD got = 0;
    const char *k;
    char *p, *q;
    if (GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH) == 0) return;
    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\open-snaphak\\install.json", la);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, data, sizeof data - 1, &got, NULL);
    CloseHandle(h);
    data[got] = '\0';
    k = "\"version\"";
    p = strstr(data, k);
    if (!p) return;
    p = strchr(p + strlen(k), ':'); if (!p) return;
    p = strchr(p, '"');             if (!p) return;
    p++;
    q = strchr(p, '"');             if (!q) return;
    if (q - p >= (ptrdiff_t)sizeof g_inst_version) q = p + sizeof g_inst_version - 1;
    memcpy(g_inst_version, p, (size_t)(q - p));
    g_inst_version[q - p] = '\0';
}

void crash_report_file(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path)
{
    crash_record r;
    SYSTEMTIME st;
    char tbuf[32], fpath[MAX_PATH];
    int len, tryn;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (g_crash_dir[0] == '\0') return;
    if (InterlockedIncrement(&g_records_written) > CRASH_MAX_RECORDS) return;

    GetLocalTime(&st);
    _snprintf_s(tbuf, sizeof tbuf, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    r.kind = kind; r.code = code;
    r.rip_rva = (unsigned long long)rip_rva; r.fault_addr = (unsigned long long)fault_addr;
    r.module = module_name; r.stack = stack; r.engine_text = engine_text;
    r.dump = dump_path; r.version = g_inst_version; r.time = tbuf;
    len = crash_record_json(g_rec_buf, sizeof g_rec_buf, &r);
    if (len <= 0) return;

    /* pending-YYYYMMDD-HHMMSS[-n].json: CREATE_NEW so a same-second second fault bumps the suffix
     * instead of read-modify-writing anything at crash time. Lexicographic name order == time order,
     * which is what the UI's "show the latest" relies on. */
    for (tryn = 0; tryn < 4; tryn++) {
        if (tryn == 0)
            _snprintf_s(fpath, MAX_PATH, _TRUNCATE, "%s\\pending-%04d%02d%02d-%02d%02d%02d.json",
                        g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            _snprintf_s(fpath, MAX_PATH, _TRUNCATE, "%s\\pending-%04d%02d%02d-%02d%02d%02d-%d.json",
                        g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tryn);
        h = CreateFileA(fpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return;
    }
    if (h == INVALID_HANDLE_VALUE) return;
    {
        DWORD wrote;
        WriteFile(h, g_rec_buf, (DWORD)len, &wrote, NULL);
    }
    CloseHandle(h);
    {
        shield_fault f = { "crash-record", (int)code, "crash record written for the report dialog",
                           rip_rva, fault_addr };
        shield_emit(&f);
    }
}

const char *crash_report_write_dump(EXCEPTION_POINTERS *ep)
{
    HANDLE h;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    BOOL ok = FALSE;
    if (!g_minidump_write || g_dump_path[0] == '\0') return "";
    h = CreateFileA(g_dump_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return "";
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    __try {
        ok = g_minidump_write(GetCurrentProcess(), GetCurrentProcessId(), h,
                              (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo),
                              ep ? &mei : NULL, NULL, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    CloseHandle(h);
    return ok ? g_dump_path : "";
}

/* One full fatal capture: stack + engine text + dump + record. Shared by the first-chance one-shot
 * and the unhandled filter. SEH-guarded per step: a capture failure degrades, never re-faults. */
static void crash_capture_fatal(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *rip  = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t fa = (ep->ExceptionRecord->NumberParameters >= 2)
                     ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    uintptr_t rva = 0;
    const char *mod = "?";
    char modbuf[80];
    const char *dump;

    /* module + RVA of the faulting site (loader-lock cost acceptable: the process is dying). */
    {
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &hm) && hm) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hm, path, MAX_PATH)) {
                const char *b = strrchr(path, '\\');
                _snprintf_s(modbuf, sizeof modbuf, _TRUNCATE, "%s", b ? b + 1 : path);
                mod = modbuf;
            }
            rva = (uintptr_t)rip - (uintptr_t)hm;
        }
    }
    g_rec_stack[0] = '\0';
    if (code != EXCEPTION_STACK_OVERFLOW)   /* never walk an exhausted stack */
        shield_capture_stack(ep->ContextRecord, g_rec_stack, sizeof g_rec_stack, 24);
    g_rec_text[0] = '\0';
    shield_last_engine_msg(g_rec_text, sizeof g_rec_text);
    dump = crash_report_write_dump(ep);
    crash_report_file("fatal", code, rva, fa, mod, g_rec_stack, g_rec_text, dump);
}

/* First-chance one-shot for the filter-bypassing fatal codes (see the header comment). LOG-ONLY. */
static LONG CALLBACK crash_fatal_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == 0xC0000374 || code == 0xC0000409) &&
        InterlockedExchange(&g_fatal_oneshot, 1) == 0) {
        __try { crash_capture_fatal(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Unhandled-exception filter: the definitive "process dying here" record. Chains to the REAL
 * previous filter -- never to itself. */
static LONG WINAPI crash_uef(EXCEPTION_POINTERS *ep)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev_filter;
    if (InterlockedIncrement(&g_uef_entered) == 1) {
        __try { crash_capture_fatal(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (prev && prev != crash_uef) return prev(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Re-assert the filter through engine bring-up (~30s), discarding the return so g_prev_filter is
 * captured exactly once at arm and can never become our own filter. */
static DWORD WINAPI crash_reassert_thread(LPVOID p)
{
    int i;
    (void)p;
    for (i = 0; i < 20; i++) { Sleep(1500); SetUnhandledExceptionFilter(crash_uef); }
    return 0;
}

void crash_report_init(void)
{
    HMODULE dh;
    crash_dirs_from_module();
    crash_read_version();
    dh = LoadLibraryA("dbghelp.dll");
    if (dh) g_minidump_write = (minidump_write_t)GetProcAddress(dh, "MiniDumpWriteDump");
}

void crash_report_arm_fatal_handlers(void)
{
    HANDLE t;
    if (g_crash_dir[0] == '\0') return;   /* init failed -> stay dark rather than half-armed */
    AddVectoredExceptionHandler(1 /* first-in-chain */, crash_fatal_veh);
    g_prev_filter = SetUnhandledExceptionFilter(crash_uef);
    if (g_prev_filter == crash_uef) g_prev_filter = NULL;   /* belt: never chain to ourselves */
    t = CreateThread(NULL, 0, crash_reassert_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    {
        shield_fault f = { "crash-record", -1, "fatal-path crash capture armed", 0, 0 };
        shield_emit(&f);
    }
}
