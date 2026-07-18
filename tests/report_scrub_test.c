/* report_scrub_test.c -- pure-logic tests for the crash-report log anonymization scrub + tail. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/ui/webview/report_scrub.h"

int main(void)
{
    char out[512];

    /* the canonical leak shape: a Windows profile path, mixed case */
    rs_scrub(out, sizeof out, "log: C:\\Users\\AlexD\\thing.log opened by alexd", "alexd", "<user>");
    assert(strcmp(out, "log: C:\\Users\\<user>\\thing.log opened by <user>") == 0);

    /* machine name, embedded mid-token */
    rs_scrub(out, sizeof out, "\\\\DESKTOP-AB12\\share on DESKTOP-ab12", "DESKTOP-AB12", "<machine>");
    assert(strcmp(out, "\\\\<machine>\\share on <machine>") == 0);

    /* a 1-char name is NOT scrubbed (would mangle the whole text) */
    rs_scrub(out, sizeof out, "banana", "a", "<user>");
    assert(strcmp(out, "banana") == 0);

    /* empty/NULL name: pass-through */
    rs_scrub(out, sizeof out, "hello", "", "<user>");
    assert(strcmp(out, "hello") == 0);

    /* no-match text unchanged; replacement can be longer than the name (bounded by cap) */
    rs_scrub(out, sizeof out, "abcabcabc", "abc", "<user>");
    assert(strcmp(out, "<user><user><user>") == 0);

    /* truncation at cap stays NUL-terminated */
    {
        char tiny[8];
        rs_scrub(tiny, sizeof tiny, "aaaa bbbb cccc", "zz", "<user>");
        assert(strlen(tiny) < sizeof tiny);
    }

    /* tail: short buffer -> from 0 */
    assert(rs_tail_offset("abc\ndef\n", 8, 100) == 0);
    /* tail: cut snaps FORWARD to the next line start (never opens with a torn line) */
    {
        const char *b = "line1\nline2\nline3\n";   /* len 18 */
        size_t off = rs_tail_offset(b, 18, 10);     /* raw cut at 8 = mid "line2" -> snap to 12 */
        assert(off == 12);
        assert(strncmp(b + off, "line3", 5) == 0);
    }
    /* tail: one giant line -> plain byte cut */
    {
        const char *b = "aaaaaaaaaaaaaaaaaaaa";     /* len 20, no newline */
        assert(rs_tail_offset(b, 20, 5) == 15);
    }

    printf("report_scrub_test OK\n");
    return 0;
}
