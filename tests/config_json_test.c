#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_json.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static void test_valid_object_preserves_raw_values(void)
{
    const char *src =
        "{\"flag\":true,\"nested\":{\"n\":1.0},\"text\":\"dark\",\"array\":[1,null]}";
    sh_json_object object = {0};

    CHECK(sh_json_parse_object(src, strlen(src), 64, &object));
    CHECK(object.count == 4);
    CHECK(strcmp(sh_json_object_get(&object, "flag"), "true") == 0);
    CHECK(strcmp(sh_json_object_get(&object, "nested"), "{\"n\":1.0}") == 0);
    CHECK(strcmp(sh_json_object_get(&object, "text"), "\"dark\"") == 0);
    CHECK(strcmp(sh_json_object_get(&object, "array"), "[1,null]") == 0);

    sh_json_object_free(&object);
}

static void test_validates_complete_json_values(void)
{
    static const struct {
        const char *json;
        sh_json_kind kind;
    } cases[] = {
        { "null", SH_JSON_NULL },
        { "true", SH_JSON_BOOL },
        { "-12.50e+2", SH_JSON_NUMBER },
        { "\"escaped\\ntext\"", SH_JSON_STRING },
        { "[1, false, {\"x\":\"y\"}]", SH_JSON_ARRAY },
        { "{\"x\":1}", SH_JSON_OBJECT },
    };

    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sh_json_kind kind = SH_JSON_NULL;
        CHECK(sh_json_validate(cases[i].json, strlen(cases[i].json), 64, &kind));
        CHECK(kind == cases[i].kind);
    }
}

static void test_rejects_invalid_syntax_and_duplicate_keys(void)
{
    static const char *invalid[] = {
        "",
        "true false",
        "01",
        "1.",
        "[1,]",
        "{\"x\":1,}",
        "{\"x\":1,\"x\":2}",
        "{\"outer\":{\"same\":1,\"same\":2}}",
        "\"unterminated",
        "\"bad\\qescape\"",
        "\"line\nbreak\"",
        "\"\\uD800\"",
        "\"\\uDC00\"",
        "\"\\uD800\\u0041\"",
    };
    size_t i;

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!sh_json_validate(invalid[i], strlen(invalid[i]), 64, NULL));
}

static void test_rejects_invalid_utf8(void)
{
    static const char overlong[] = { '"', (char)0xC0, (char)0xAF, '"', 0 };
    static const char surrogate[] = { '"', (char)0xED, (char)0xA0, (char)0x80, '"', 0 };
    static const char too_large[] = {
        '"', (char)0xF4, (char)0x90, (char)0x80, (char)0x80, '"', 0
    };

    CHECK(!sh_json_validate(overlong, sizeof(overlong) - 1, 64, NULL));
    CHECK(!sh_json_validate(surrogate, sizeof(surrogate) - 1, 64, NULL));
    CHECK(!sh_json_validate(too_large, sizeof(too_large) - 1, 64, NULL));
}

static void test_enforces_depth_limit(void)
{
    char json[160];
    size_t i;

    for (i = 0; i < 65; i++)
        json[i] = '[';
    json[65] = '0';
    for (i = 0; i < 65; i++)
        json[66 + i] = ']';
    json[131] = 0;

    CHECK(!sh_json_validate(json, strlen(json), 64, NULL));
    CHECK(sh_json_validate(json, strlen(json), 65, NULL));
}

static void test_mutates_objects_without_losing_order(void)
{
    const char *src = "{\"first\":1,\"theme\":\"light\"}";
    sh_json_object object = {0};

    CHECK(sh_json_parse_object(src, strlen(src), 64, &object));
    CHECK(sh_json_object_set(&object, "theme", "\"dark\"", 64));
    CHECK(object.count == 2);
    CHECK(strcmp(object.members[0].key, "first") == 0);
    CHECK(strcmp(object.members[1].key, "theme") == 0);
    CHECK(strcmp(sh_json_object_get(&object, "theme"), "\"dark\"") == 0);

    CHECK(sh_json_object_set(&object, "future", "[true,{\"x\":1}]", 64));
    CHECK(object.count == 3);
    CHECK(strcmp(object.members[2].key, "future") == 0);

    CHECK(!sh_json_object_set(&object, "theme", "[1,]", 64));
    CHECK(strcmp(sh_json_object_get(&object, "theme"), "\"dark\"") == 0);
    sh_json_object_free(&object);
}

static void test_decodes_json_strings_with_size_query(void)
{
    const char *json = "\"line\\n\\uD83D\\uDE00\"";
    const unsigned char expected[] = {
        'l', 'i', 'n', 'e', '\n', 0xF0, 0x9F, 0x98, 0x80, 0
    };
    char small[4] = { 'X', 'X', 'X', 0 };
    char out[16];
    size_t needed = 0;

    CHECK(sh_json_decode_string(json, strlen(json), NULL, 0, &needed));
    CHECK(needed == 9);
    CHECK(sh_json_decode_string(json, strlen(json), small, sizeof(small), &needed));
    CHECK(small[0] == 'X');
    CHECK(sh_json_decode_string(json, strlen(json), out, sizeof(out), &needed));
    CHECK(memcmp(out, expected, sizeof(expected)) == 0);
    CHECK(!sh_json_decode_string("true", 4, out, sizeof(out), &needed));
}

static void test_serializes_nested_config_deterministically(void)
{
    sh_json_object root = {0};
    sh_json_object settings = {0};
    char *settings_json;
    char *config_json;
    size_t settings_length = 0, config_length = 0;
    const char *expected =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"settings\": {\n"
        "    \"theme\": \"dark\"\n"
        "  }\n"
        "}\n";

    CHECK(sh_json_object_set(&settings, "theme", "\"dark\"", 64));
    settings_json = sh_json_serialize_object(&settings, 2, &settings_length);
    CHECK(settings_json != NULL);
    CHECK(sh_json_object_set(&root, "schema_version", "1", 64));
    CHECK(sh_json_object_set(&root, "settings", settings_json, 64));
    config_json = sh_json_serialize_object(&root, 0, &config_length);
    CHECK(config_json != NULL);
    CHECK(config_length == strlen(expected));
    CHECK(strcmp(config_json, expected) == 0);

    free(settings_json);
    free(config_json);
    sh_json_object_free(&settings);
    sh_json_object_free(&root);
}

int main(void)
{
    test_valid_object_preserves_raw_values();
    test_validates_complete_json_values();
    test_rejects_invalid_syntax_and_duplicate_keys();
    test_rejects_invalid_utf8();
    test_enforces_depth_limit();
    test_mutates_objects_without_losing_order();
    test_decodes_json_strings_with_size_query();
    test_serializes_nested_config_deterministically();

    if (g_failed) {
        fprintf(stderr, "config_json_test: %d failure(s)\n", g_failed);
        return 1;
    }
    puts("config_json_test: OK");
    return 0;
}
