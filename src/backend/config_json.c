/* config_json.c -- see config_json.h. Pure C; no Windows or engine dependencies. */
#include "config_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_cursor {
    const unsigned char *begin;
    const unsigned char *p;
    const unsigned char *end;
    unsigned max_depth;
} json_cursor;

static void skip_ws(json_cursor *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n'))
        c->p++;
}

static int is_hex(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static unsigned hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch - 'A' + 10;
}

static int read_hex4(const unsigned char *p, const unsigned char *end, unsigned *out)
{
    unsigned v;
    int i;
    if ((size_t)(end - p) < 4) return 0;
    v = 0;
    for (i = 0; i < 4; i++) {
        if (!is_hex(p[i])) return 0;
        v = (v << 4) | hex_value(p[i]);
    }
    if (out) *out = v;
    return 1;
}

static int valid_utf8_at(const unsigned char *p, const unsigned char *end, size_t *used)
{
    unsigned char a, b, d, e;
    size_t left;
    if (p >= end) return 0;
    a = p[0];
    left = (size_t)(end - p);
    if (a < 0x80) {
        *used = 1;
        return 1;
    }
    if (a >= 0xC2 && a <= 0xDF) {
        if (left < 2) return 0;
        b = p[1];
        if (b < 0x80 || b > 0xBF) return 0;
        *used = 2;
        return 1;
    }
    if (left < 3) return 0;
    b = p[1];
    d = p[2];
    if (d < 0x80 || d > 0xBF) return 0;
    if (a == 0xE0) {
        if (b < 0xA0 || b > 0xBF) return 0;
        *used = 3;
        return 1;
    }
    if ((a >= 0xE1 && a <= 0xEC) || (a >= 0xEE && a <= 0xEF)) {
        if (b < 0x80 || b > 0xBF) return 0;
        *used = 3;
        return 1;
    }
    if (a == 0xED) {
        if (b < 0x80 || b > 0x9F) return 0;
        *used = 3;
        return 1;
    }
    if (left < 4) return 0;
    e = p[3];
    if (e < 0x80 || e > 0xBF) return 0;
    if (a == 0xF0) {
        if (b < 0x90 || b > 0xBF) return 0;
        *used = 4;
        return 1;
    }
    if (a >= 0xF1 && a <= 0xF3) {
        if (b < 0x80 || b > 0xBF) return 0;
        *used = 4;
        return 1;
    }
    if (a == 0xF4) {
        if (b < 0x80 || b > 0x8F) return 0;
        *used = 4;
        return 1;
    }
    return 0;
}

static int scan_string(json_cursor *c)
{
    if (c->p >= c->end || *c->p != '"') return 0;
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = *c->p++;
        if (ch == '"') return 1;
        if (ch < 0x20) return 0;
        if (ch == '\\') {
            unsigned escaped;
            if (c->p >= c->end) return 0;
            ch = *c->p++;
            if (ch == '"' || ch == '\\' || ch == '/' ||
                ch == 'b' || ch == 'f' || ch == 'n' ||
                ch == 'r' || ch == 't')
                continue;
            if (ch != 'u' || !read_hex4(c->p, c->end, &escaped)) return 0;
            c->p += 4;
            if (escaped >= 0xD800 && escaped <= 0xDBFF) {
                unsigned low;
                if ((size_t)(c->end - c->p) < 6 ||
                    c->p[0] != '\\' || c->p[1] != 'u' ||
                    !read_hex4(c->p + 2, c->end, &low) ||
                    low < 0xDC00 || low > 0xDFFF)
                    return 0;
                c->p += 6;
            } else if (escaped >= 0xDC00 && escaped <= 0xDFFF) {
                return 0;
            }
            continue;
        }
        if (ch >= 0x80) {
            size_t used;
            c->p--;
            if (!valid_utf8_at(c->p, c->end, &used)) return 0;
            c->p += used;
        }
    }
    return 0;
}

static int append_utf8(unsigned codepoint, char *out, size_t cap, size_t *at)
{
    unsigned char bytes[4];
    size_t n, i;
    if (codepoint <= 0x7F) {
        bytes[0] = (unsigned char)codepoint;
        n = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        n = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        n = 4;
    }
    if (out && *at + n < cap) {
        for (i = 0; i < n; i++) out[*at + i] = (char)bytes[i];
    }
    *at += n;
    return 1;
}

static int decode_string_alloc(const unsigned char *start, const unsigned char *end,
                               char **out_string, size_t *out_length)
{
    const unsigned char *p;
    char *out;
    size_t at, cap;
    if (!start || !end || end <= start + 1 || *start != '"' || end[-1] != '"')
        return 0;
    cap = (size_t)(end - start);
    out = (char *)malloc(cap);
    if (!out) return 0;
    p = start + 1;
    at = 0;
    while (p < end - 1) {
        unsigned char ch = *p++;
        if (ch != '\\') {
            out[at++] = (char)ch;
            continue;
        }
        ch = *p++;
        switch (ch) {
        case '"': out[at++] = '"'; break;
        case '\\': out[at++] = '\\'; break;
        case '/': out[at++] = '/'; break;
        case 'b': out[at++] = '\b'; break;
        case 'f': out[at++] = '\f'; break;
        case 'n': out[at++] = '\n'; break;
        case 'r': out[at++] = '\r'; break;
        case 't': out[at++] = '\t'; break;
        case 'u': {
            unsigned high, codepoint;
            if (!read_hex4(p, end - 1, &high)) { free(out); return 0; }
            p += 4;
            codepoint = high;
            if (high >= 0xD800 && high <= 0xDBFF) {
                unsigned low;
                if (p + 6 > end - 1 || p[0] != '\\' || p[1] != 'u' ||
                    !read_hex4(p + 2, end - 1, &low)) {
                    free(out);
                    return 0;
                }
                p += 6;
                codepoint = 0x10000u + ((high - 0xD800u) << 10) + (low - 0xDC00u);
            }
            append_utf8(codepoint, out, cap, &at);
            break;
        }
        default:
            free(out);
            return 0;
        }
    }
    out[at] = 0;
    *out_string = out;
    if (out_length) *out_length = at;
    return 1;
}

static int scan_value(json_cursor *c, unsigned depth, sh_json_kind *out_kind);

typedef struct decoded_key {
    char *bytes;
    size_t length;
} decoded_key;

static void free_keys(decoded_key *keys, size_t count)
{
    size_t i;
    if (!keys) return;
    for (i = 0; i < count; i++) free(keys[i].bytes);
    free(keys);
}

static int add_unique_key(decoded_key **keys, size_t *count, size_t *capacity,
                          char *key, size_t key_length)
{
    decoded_key *grown;
    size_t i, next;
    for (i = 0; i < *count; i++) {
        if ((*keys)[i].length == key_length &&
            memcmp((*keys)[i].bytes, key, key_length) == 0)
            return 0;
    }
    if (*count == *capacity) {
        next = *capacity ? *capacity * 2 : 8;
        if (next < *capacity || next > SIZE_MAX / sizeof(decoded_key)) return 0;
        grown = (decoded_key *)realloc(*keys, next * sizeof(decoded_key));
        if (!grown) return 0;
        *keys = grown;
        *capacity = next;
    }
    (*keys)[*count].bytes = key;
    (*keys)[*count].length = key_length;
    (*count)++;
    return 1;
}

static int scan_object(json_cursor *c, unsigned depth)
{
    decoded_key *keys = NULL;
    size_t key_count = 0, key_capacity = 0;
    int ok = 0;
    if (depth >= c->max_depth || c->p >= c->end || *c->p != '{') return 0;
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return 1;
    }
    for (;;) {
        const unsigned char *key_start, *key_end;
        char *key = NULL;
        size_t key_length = 0;
        key_start = c->p;
        if (!scan_string(c)) goto done;
        key_end = c->p;
        if (!decode_string_alloc(key_start, key_end, &key, &key_length)) goto done;
        if (!add_unique_key(&keys, &key_count, &key_capacity, key, key_length)) {
            free(key);
            goto done;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') goto done;
        c->p++;
        skip_ws(c);
        if (!scan_value(c, depth + 1, NULL)) goto done;
        skip_ws(c);
        if (c->p >= c->end) goto done;
        if (*c->p == '}') {
            c->p++;
            ok = 1;
            break;
        }
        if (*c->p != ',') goto done;
        c->p++;
        skip_ws(c);
    }
done:
    free_keys(keys, key_count);
    return ok;
}

static int scan_array(json_cursor *c, unsigned depth)
{
    if (depth >= c->max_depth || c->p >= c->end || *c->p != '[') return 0;
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return 1;
    }
    for (;;) {
        if (!scan_value(c, depth + 1, NULL)) return 0;
        skip_ws(c);
        if (c->p >= c->end) return 0;
        if (*c->p == ']') {
            c->p++;
            return 1;
        }
        if (*c->p != ',') return 0;
        c->p++;
        skip_ws(c);
    }
}

static int scan_number(json_cursor *c)
{
    const unsigned char *start = c->p;
    if (c->p < c->end && *c->p == '-') c->p++;
    if (c->p >= c->end) return 0;
    if (*c->p == '0') {
        c->p++;
        if (c->p < c->end && *c->p >= '0' && *c->p <= '9') return 0;
    } else {
        if (*c->p < '1' || *c->p > '9') return 0;
        do { c->p++; }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9');
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') return 0;
        do { c->p++; }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9');
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') return 0;
        do { c->p++; }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9');
    }
    return c->p > start;
}

static int scan_literal(json_cursor *c, const char *literal)
{
    size_t n = strlen(literal);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, literal, n) != 0) return 0;
    c->p += n;
    return 1;
}

static int scan_value(json_cursor *c, unsigned depth, sh_json_kind *out_kind)
{
    if (c->p >= c->end) return 0;
    switch (*c->p) {
    case 'n':
        if (out_kind) *out_kind = SH_JSON_NULL;
        return scan_literal(c, "null");
    case 't':
        if (out_kind) *out_kind = SH_JSON_BOOL;
        return scan_literal(c, "true");
    case 'f':
        if (out_kind) *out_kind = SH_JSON_BOOL;
        return scan_literal(c, "false");
    case '"':
        if (out_kind) *out_kind = SH_JSON_STRING;
        return scan_string(c);
    case '[':
        if (out_kind) *out_kind = SH_JSON_ARRAY;
        return scan_array(c, depth);
    case '{':
        if (out_kind) *out_kind = SH_JSON_OBJECT;
        return scan_object(c, depth);
    default:
        if (*c->p == '-' || (*c->p >= '0' && *c->p <= '9')) {
            if (out_kind) *out_kind = SH_JSON_NUMBER;
            return scan_number(c);
        }
        return 0;
    }
}

int sh_json_validate(const char *json, size_t length, unsigned max_depth,
                     sh_json_kind *out_kind)
{
    json_cursor c;
    sh_json_kind kind;
    if (!json || max_depth == 0) return 0;
    c.begin = (const unsigned char *)json;
    c.p = c.begin;
    c.end = c.begin + length;
    c.max_depth = max_depth;
    skip_ws(&c);
    if (!scan_value(&c, 0, &kind)) return 0;
    skip_ws(&c);
    if (c.p != c.end) return 0;
    if (out_kind) *out_kind = kind;
    return 1;
}

static char *copy_range(const unsigned char *begin, const unsigned char *end)
{
    size_t n;
    char *copy;
    if (!begin || !end || end < begin) return NULL;
    n = (size_t)(end - begin);
    if (n == SIZE_MAX) return NULL;
    copy = (char *)malloc(n + 1);
    if (!copy) return NULL;
    if (n) memcpy(copy, begin, n);
    copy[n] = 0;
    return copy;
}

static int object_append_owned(sh_json_object *object, char *key, size_t key_length,
                               char *value)
{
    sh_json_member *grown;
    size_t next;
    if (object->count == object->capacity) {
        next = object->capacity ? object->capacity * 2 : 8;
        if (next < object->capacity || next > SIZE_MAX / sizeof(sh_json_member)) return 0;
        grown = (sh_json_member *)realloc(object->members, next * sizeof(sh_json_member));
        if (!grown) return 0;
        object->members = grown;
        object->capacity = next;
    }
    object->members[object->count].key = key;
    object->members[object->count].key_length = key_length;
    object->members[object->count].value_json = value;
    object->count++;
    return 1;
}

int sh_json_parse_object(const char *json, size_t length, unsigned max_depth,
                         sh_json_object *out)
{
    json_cursor c;
    sh_json_kind kind;
    sh_json_object parsed = {0};
    if (!out || !sh_json_validate(json, length, max_depth, &kind) ||
        kind != SH_JSON_OBJECT)
        return 0;
    c.begin = (const unsigned char *)json;
    c.p = c.begin;
    c.end = c.begin + length;
    c.max_depth = max_depth;
    skip_ws(&c);
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        *out = parsed;
        return 1;
    }
    for (;;) {
        const unsigned char *key_start = c.p;
        const unsigned char *key_end;
        const unsigned char *value_start, *value_end;
        char *key = NULL, *value = NULL;
        size_t key_length = 0;
        if (!scan_string(&c)) goto fail;
        key_end = c.p;
        if (!decode_string_alloc(key_start, key_end, &key, &key_length)) goto fail;
        skip_ws(&c);
        if (c.p >= c.end || *c.p != ':') { free(key); goto fail; }
        c.p++;
        skip_ws(&c);
        value_start = c.p;
        if (!scan_value(&c, 1, NULL)) { free(key); goto fail; }
        value_end = c.p;
        value = copy_range(value_start, value_end);
        if (!value || !object_append_owned(&parsed, key, key_length, value)) {
            free(key);
            free(value);
            goto fail;
        }
        skip_ws(&c);
        if (c.p < c.end && *c.p == '}') {
            c.p++;
            break;
        }
        if (c.p >= c.end || *c.p != ',') goto fail;
        c.p++;
        skip_ws(&c);
    }
    *out = parsed;
    return 1;
fail:
    sh_json_object_free(&parsed);
    return 0;
}

const char *sh_json_object_get(const sh_json_object *object, const char *key)
{
    size_t i, key_length;
    if (!object || !key) return NULL;
    key_length = strlen(key);
    for (i = 0; i < object->count; i++) {
        if (object->members[i].key_length == key_length &&
            memcmp(object->members[i].key, key, key_length) == 0)
            return object->members[i].value_json;
    }
    return NULL;
}

void sh_json_object_free(sh_json_object *object)
{
    size_t i;
    if (!object) return;
    for (i = 0; i < object->count; i++) {
        free(object->members[i].key);
        free(object->members[i].value_json);
    }
    free(object->members);
    object->members = NULL;
    object->count = 0;
    object->capacity = 0;
}

int sh_json_object_set(sh_json_object *object, const char *key,
                       const char *value_json, unsigned max_depth)
{
    if (!key) return 0;
    return sh_json_object_set_n(object, key, strlen(key), value_json, max_depth);
}

int sh_json_object_set_n(sh_json_object *object, const char *key,
                         size_t key_length, const char *value_json,
                         unsigned max_depth)
{
    size_t i, value_length;
    char *key_copy = NULL, *value_copy = NULL;
    sh_json_kind ignored;
    if (!object || !key || !value_json) return 0;
    value_length = strlen(value_json);
    if (!sh_json_validate(value_json, value_length, max_depth, &ignored)) return 0;
    value_copy = copy_range((const unsigned char *)value_json,
                            (const unsigned char *)value_json + value_length);
    if (!value_copy) return 0;
    for (i = 0; i < object->count; i++) {
        if (object->members[i].key_length == key_length &&
            memcmp(object->members[i].key, key, key_length) == 0) {
            free(object->members[i].value_json);
            object->members[i].value_json = value_copy;
            return 1;
        }
    }
    key_copy = copy_range((const unsigned char *)key,
                          (const unsigned char *)key + key_length);
    if (!key_copy ||
        !object_append_owned(object, key_copy, key_length, value_copy)) {
        free(key_copy);
        free(value_copy);
        return 0;
    }
    return 1;
}

int sh_json_decode_string(const char *json, size_t length,
                          char *out, size_t out_capacity, size_t *out_length)
{
    json_cursor c;
    char *decoded = NULL;
    size_t decoded_length = 0;
    if (!json) return 0;
    c.begin = (const unsigned char *)json;
    c.p = c.begin;
    c.end = c.begin + length;
    c.max_depth = 1;
    if (!scan_string(&c) || c.p != c.end ||
        !decode_string_alloc(c.begin, c.end, &decoded, &decoded_length))
        return 0;
    if (out_length) *out_length = decoded_length;
    if (out && out_capacity > decoded_length) {
        if (decoded_length) memcpy(out, decoded, decoded_length);
        out[decoded_length] = 0;
    }
    free(decoded);
    return 1;
}

typedef struct json_builder {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} json_builder;

static void builder_reserve(json_builder *b, size_t extra)
{
    size_t needed, next;
    char *grown;
    if (b->failed) return;
    if (extra > SIZE_MAX - b->length - 1) {
        b->failed = 1;
        return;
    }
    needed = b->length + extra + 1;
    if (needed <= b->capacity) return;
    next = b->capacity ? b->capacity : 128;
    while (next < needed) {
        if (next > SIZE_MAX / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }
    grown = (char *)realloc(b->data, next);
    if (!grown) {
        b->failed = 1;
        return;
    }
    b->data = grown;
    b->capacity = next;
}

static void builder_bytes(json_builder *b, const char *bytes, size_t length)
{
    builder_reserve(b, length);
    if (b->failed) return;
    if (length) memcpy(b->data + b->length, bytes, length);
    b->length += length;
}

static void builder_char(json_builder *b, char ch)
{
    builder_bytes(b, &ch, 1);
}

static void builder_indent(json_builder *b, unsigned count)
{
    while (count--) builder_char(b, ' ');
}

static void builder_quoted(json_builder *b, const char *bytes, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    builder_char(b, '"');
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)bytes[i];
        switch (ch) {
        case '"': builder_bytes(b, "\\\"", 2); break;
        case '\\': builder_bytes(b, "\\\\", 2); break;
        case '\b': builder_bytes(b, "\\b", 2); break;
        case '\f': builder_bytes(b, "\\f", 2); break;
        case '\n': builder_bytes(b, "\\n", 2); break;
        case '\r': builder_bytes(b, "\\r", 2); break;
        case '\t': builder_bytes(b, "\\t", 2); break;
        default:
            if (ch < 0x20) {
                char escaped[6] = {
                    '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0x0F]
                };
                builder_bytes(b, escaped, sizeof(escaped));
            } else {
                builder_char(b, (char)ch);
            }
            break;
        }
    }
    builder_char(b, '"');
}

char *sh_json_serialize_object(const sh_json_object *object,
                               unsigned base_indent, size_t *out_length)
{
    json_builder b = {0};
    size_t i;
    if (!object) return NULL;
    builder_char(&b, '{');
    if (object->count) builder_char(&b, '\n');
    for (i = 0; i < object->count; i++) {
        const sh_json_member *member = &object->members[i];
        builder_indent(&b, base_indent + 2);
        builder_quoted(&b, member->key, member->key_length);
        builder_bytes(&b, ": ", 2);
        builder_bytes(&b, member->value_json, strlen(member->value_json));
        if (i + 1 < object->count) builder_char(&b, ',');
        builder_char(&b, '\n');
    }
    if (object->count) builder_indent(&b, base_indent);
    builder_char(&b, '}');
    if (base_indent == 0) builder_char(&b, '\n');
    builder_reserve(&b, 0);
    if (b.failed) {
        free(b.data);
        return NULL;
    }
    b.data[b.length] = 0;
    if (out_length) *out_length = b.length;
    return b.data;
}
