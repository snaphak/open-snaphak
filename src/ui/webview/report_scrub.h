/* report_scrub.h -- pure helpers for the crash-report log attachment: anonymization scrub + tail.
 *
 * Submitting a crash report may attach the tails of the local log files, and log lines carry local
 * filesystem paths -- i.e. the Windows account name (C:\Users\<name>\...) and sometimes the machine
 * name. Reports are anonymous by design, so BOTH are scrubbed out of the attached text before it
 * leaves the machine. Header-only, pure C (no OS calls), so the exact same code is unit-tested
 * off-game (tests/report_scrub_test.c) and compiled into the UI host.
 */
#ifndef SNAPHAK_REPORT_SCRUB_H
#define SNAPHAK_REPORT_SCRUB_H

#include <stddef.h>
#include <string.h>

/* Case-insensitive scrub: copy `src` into dst, replacing EVERY occurrence of `name` (matched
 * case-insensitively) with `repl`. Names shorter than 2 chars are not scrubbed (degenerate match --
 * would mangle the whole text; no real Windows account is 1 char). Always NUL-terminates; truncates
 * at cap. Returns chars written. */
static int rs_scrub(char *dst, size_t cap, const char *src, const char *name, const char *repl)
{
    size_t o = 0, nlen, rlen, i;
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    nlen = name ? strlen(name) : 0;
    rlen = repl ? strlen(repl) : 0;
    while (*src && o + 1 < cap) {
        int hit = 0;
        if (nlen >= 2) {
            hit = 1;
            for (i = 0; i < nlen; i++) {
                char a = src[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { hit = 0; break; }
                if (src[i] == '\0') { hit = 0; break; }
            }
        }
        if (hit) {
            for (i = 0; i < rlen && o + 1 < cap; i++) dst[o++] = repl[i];
            src += nlen;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* Tail: return the offset into a `len`-byte buffer such that at most `keep` bytes remain, snapped
 * FORWARD to the next line start when the cut lands mid-line (so the tail never opens with a torn
 * half-line). A buffer shorter than `keep` tails from 0. Pure arithmetic. */
static size_t rs_tail_offset(const char *buf, size_t len, size_t keep)
{
    size_t off;
    if (!buf || len <= keep) return 0;
    off = len - keep;
    while (off < len && buf[off - 1] != '\n') off++;   /* snap to a line boundary */
    if (off >= len) off = len - keep;                  /* one giant line -> plain byte cut */
    return off;
}

#endif /* SNAPHAK_REPORT_SCRUB_H */
