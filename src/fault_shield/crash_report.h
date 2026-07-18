/* crash_report.h -- crash-record capture for the SHIPPED build (the crash-reporting pipeline's
 * capture end).
 *
 * The shield's recovery layers (veh.c) keep the editor alive where they can; this module is the
 * durable evidence trail for the faults that still matter to the user: each one is written as a
 * small JSON crash record to <game>\snaphak\crash\pending-*.json with crash-safe primitives only.
 * The SnapHak Studio UI polls that directory and raises the crash-report dialog -- seconds later
 * for a survived (recovered) fault, or on the next launch when the process died.
 *
 * Three producers:
 *   - the Class-B recovery path in veh.c (survived fault -> record, dialog appears in-session);
 *   - the terminal idFatalException throw seen by veh.c LAYER 2 (the engine will exit after the
 *     Frame catch rethrows -- this is the ONLY capture point for that death);
 *   - the fatal handlers armed here: an unhandled-exception filter + a first-chance one-shot for
 *     the fatal codes that never reach a filter (heap corruption 0xC0000374, __fastfail 0xC0000409),
 *     both of which also write a crash dump (snaphak\logs\snaphak_crash.dmp, local only -- the
 *     dialog never uploads it).
 *
 * Everything here is LOG-ONLY: no handler alters an exception's disposition, and a write failure
 * is silently absorbed (never make the crash worse).
 */
#ifndef SHIELD_CRASH_REPORT_H
#define SHIELD_CRASH_REPORT_H

#include <windows.h>
#include <stdint.h>

/* Resolve the record/dump directories from this DLL's own location, read the installed version
 * (so a record names the version that CRASHED, not the one that later reports it), and pre-bind
 * dbghelp's dump writer (never LoadLibrary at crash time). Call once, off the loader lock. */
void crash_report_init(void);

/* Arm the fatal-path handlers: SetUnhandledExceptionFilter + a first-chance VEH one-shot for the
 * filter-bypassing fatal codes, plus a bounded re-assert loop (the engine installs its own filter
 * during bring-up and would silently displace ours). Call after crash_report_init. */
void crash_report_arm_fatal_handlers(void);

/* Write one crash record (pending-<stamp>.json, CREATE_NEW, write-through). Crash-safe: static
 * buffers, no CRT heap. Bounded per session so a fault storm cannot spam the directory. */
void crash_report_file(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path);

/* Best-effort minidump for the fatal path (MiniDumpNormal|WithThreadInfo -> snaphak\logs\
 * snaphak_crash.dmp). Returns the dump path ("" if it could not be written). LOCAL ONLY. */
const char *crash_report_write_dump(EXCEPTION_POINTERS *ep);

#endif /* SHIELD_CRASH_REPORT_H */
