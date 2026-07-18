/* crash_record_format.h -- the crash record's PURE JSON formatter (no I/O, no OS calls).
 *
 * A crash record is one small JSON file describing one fault: what class it was, where it hit, the
 * call stack, the engine's own error text when it exists, and when/what version. crash_report.c writes
 * it to disk at fault time; the SnapHak Studio UI reads it back on its next launch (or seconds later,
 * for a recovered fault) and offers the user a crash-report dialog. Kept pure + separately-compilable
 * so the formatter and its escaping are unit-tested off-game (tests/crash_record_test.c), the same
 * split fault_record.h uses for shield_format.
 */
#ifndef SHIELD_CRASH_RECORD_FORMAT_H
#define SHIELD_CRASH_RECORD_FORMAT_H

#include <stddef.h>

typedef struct crash_record {
    const char *kind;         /* "fatal" | "classB" | "engine_fatalerror" */
    unsigned long code;       /* exception code (0 if not a hardware fault) */
    unsigned long long rip_rva;    /* faulting rip - module base (0 if unknown) */
    unsigned long long fault_addr; /* faulting data address (0 if none) */
    const char *module;       /* faulting module basename, e.g. "DOOMx64vk.exe" */
    const char *stack;        /* multi-line "MOD+0xRVA" walk, or "" */
    const char *engine_text;  /* the engine's own formatted error text, or "" */
    const char *dump;         /* crash-dump path if one was written, or "" */
    const char *version;      /* installed version string AT FAULT TIME (read at arm), or "" */
    const char *time;         /* preformatted local "YYYY-MM-DD HH:MM:SS" */
} crash_record;

/* Escape `src` as a JSON string body (no surrounding quotes) into dst. Handles \\ \" and control
 * chars (\n \r \t and \u00XX for the rest). Always NUL-terminates; truncates at cap. Returns the
 * number of chars written (excluding the NUL). Pure + deterministic. */
int crash_json_escape(char *dst, size_t cap, const char *src);

/* Format the whole record as a single JSON object into buf. Returns chars written (>0), or 0 on a
 * too-small buffer / NULL args. Pure + deterministic: same record -> same bytes. */
int crash_record_json(char *buf, size_t cap, const crash_record *r);

#endif /* SHIELD_CRASH_RECORD_FORMAT_H */
