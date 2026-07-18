/* crash_record_format.c -- see crash_record_format.h. Pure C, no OS/CRT-heap dependency: safe to call
 * from a crash context (static/stack buffers only; _snprintf_s never allocates). */
#include "crash_record_format.h"
#include <stdio.h>
#include <string.h>

int crash_json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    while (*src && o + 7 < cap) {   /* worst case one char -> \u00XX (6) + NUL */
        unsigned char c = (unsigned char)*src++;
        switch (c) {
        case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
        case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
        case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
        case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
        case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
        default:
            if (c < 0x20) {
                int n = _snprintf_s(dst + o, cap - o, _TRUNCATE, "\\u%04x", (unsigned)c);
                if (n <= 0) { dst[o] = '\0'; return (int)o; }
                o += (size_t)n;
            } else {
                dst[o++] = (char)c;   /* UTF-8 bytes pass through verbatim */
            }
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* Per-field escape scratch. The stack walk is the longest field; the engine text is capped upstream
 * (HARVEST_MSG_MAX). Static (not stack): this runs in crash contexts where stack may be precious. */
static char e_stack[4096], e_text[1536], e_small[512];

int crash_record_json(char *buf, size_t cap, const crash_record *r)
{
    int w;
    size_t used = 0;
    if (!buf || cap < 64 || !r) return 0;

#define CR_APPENDF(...) do { \
        w = _snprintf_s(buf + used, cap - used, _TRUNCATE, __VA_ARGS__); \
        if (w < 0) { buf[cap - 1] = '\0'; return (int)strlen(buf); } \
        used += (size_t)w; \
    } while (0)

    crash_json_escape(e_small, sizeof e_small, r->kind ? r->kind : "unknown");
    CR_APPENDF("{\"kind\":\"%s\",", e_small);
    CR_APPENDF("\"code\":\"0x%08lx\",", r->code);
    CR_APPENDF("\"rip_rva\":\"0x%llx\",", r->rip_rva);
    CR_APPENDF("\"fault_addr\":\"0x%llx\",", r->fault_addr);
    crash_json_escape(e_small, sizeof e_small, r->module ? r->module : "");
    CR_APPENDF("\"module\":\"%s\",", e_small);
    crash_json_escape(e_stack, sizeof e_stack, r->stack ? r->stack : "");
    CR_APPENDF("\"stack\":\"%s\",", e_stack);
    crash_json_escape(e_text, sizeof e_text, r->engine_text ? r->engine_text : "");
    CR_APPENDF("\"engineText\":\"%s\",", e_text);
    crash_json_escape(e_small, sizeof e_small, r->dump ? r->dump : "");
    CR_APPENDF("\"dump\":\"%s\",", e_small);
    crash_json_escape(e_small, sizeof e_small, r->version ? r->version : "");
    CR_APPENDF("\"version\":\"%s\",", e_small);
    crash_json_escape(e_small, sizeof e_small, r->time ? r->time : "");
    CR_APPENDF("\"time\":\"%s\"}", e_small);

#undef CR_APPENDF
    return (int)used;
}
