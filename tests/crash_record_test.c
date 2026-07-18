/* crash_record_test.c -- pure-logic tests for the crash-record JSON formatter (no game, no engine). */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/fault_shield/crash_record_format.h"

int main(void)
{
    char buf[8192];

    /* escaping: backslash, quote, newline, tab, control char */
    {
        char e[128];
        int n = crash_json_escape(e, sizeof e, "a\\b\"c\nd\te\x01" "f");   /* spliced: \x is greedy */
        assert(n > 0);
        assert(strcmp(e, "a\\\\b\\\"c\\nd\\te\\u0001f") == 0);
    }
    /* escaping: truncation is clean (NUL-terminated, never a torn escape) */
    {
        char e[6];
        crash_json_escape(e, sizeof e, "aaaaaaaaaa");
        assert(strlen(e) < sizeof e);
    }
    /* full record: every field lands, hex fields formatted, JSON stays balanced */
    {
        crash_record r;
        int n;
        memset(&r, 0, sizeof r);
        r.kind = "classB"; r.code = 0xC0000005ul; r.rip_rva = 0x5e0b12ull; r.fault_addr = 0x3f00000060ull;
        r.module = "DOOMx64vk.exe";
        r.stack = "DOOM+0x5e0b12\n    DOOM+0x5e6410";
        r.engine_text = "^1ERROR: \"quoted\" thing";
        r.dump = ""; r.version = "0.2.0-beta.3"; r.time = "2026-07-18 12:00:00";
        n = crash_record_json(buf, sizeof buf, &r);
        assert(n > 0 && (int)strlen(buf) == n);
        assert(buf[0] == '{' && buf[n - 1] == '}');
        assert(strstr(buf, "\"kind\":\"classB\""));
        assert(strstr(buf, "\"code\":\"0xc0000005\""));
        assert(strstr(buf, "\"rip_rva\":\"0x5e0b12\""));
        assert(strstr(buf, "\"fault_addr\":\"0x3f00000060\""));
        assert(strstr(buf, "\"module\":\"DOOMx64vk.exe\""));
        assert(strstr(buf, "DOOM+0x5e0b12\\n"));                     /* newline escaped inside stack */
        assert(strstr(buf, "\\\"quoted\\\""));                       /* quotes escaped inside text */
        assert(strstr(buf, "\"version\":\"0.2.0-beta.3\""));
        assert(strstr(buf, "\"time\":\"2026-07-18 12:00:00\""));
    }
    /* NULL fields degrade to empty strings, not crashes */
    {
        crash_record r;
        memset(&r, 0, sizeof r);
        r.kind = "fatal";
        assert(crash_record_json(buf, sizeof buf, &r) > 0);
        assert(strstr(buf, "\"kind\":\"fatal\""));
        assert(strstr(buf, "\"stack\":\"\""));
    }
    printf("crash_record_test OK\n");
    return 0;
}
