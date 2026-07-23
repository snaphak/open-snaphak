#include <cstdio>
#include <string>

#include "config_message.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",                       \
                     __FILE__, __LINE__, #expr);                                \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static std::wstring make_get(const std::wstring &key)
{
    return L"{\"cmd\":\"configGet\",\"key\":\"" + key + L"\"}";
}

static std::wstring make_set(const std::wstring &key,
                             const std::wstring &value_json)
{
    return L"{\"cmd\":\"configSet\",\"key\":\"" + key +
           L"\",\"valueJson\":\"" + value_json + L"\"}";
}

static void test_decoded_limits_are_enforced()
{
    const std::wstring key_at_limit(128, L'k');
    const std::wstring key_over_limit(129, L'k');
    const std::wstring value_at_limit(65536, L'v');
    const std::wstring value_over_limit(65537, L'v');

    sh_config_message message =
        sh_extract_config_message(make_get(key_at_limit).c_str(), 128, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_GET);
    CHECK(message.fields_valid);
    CHECK(message.key == key_at_limit);

    message =
        sh_extract_config_message(make_get(key_over_limit).c_str(), 128, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_GET);
    CHECK(!message.fields_valid);
    CHECK(message.key.empty());

    message = sh_extract_config_message(
        make_set(L"theme", value_at_limit).c_str(), 128, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_SET);
    CHECK(message.fields_valid);
    CHECK(message.value_json == value_at_limit);

    message = sh_extract_config_message(
        make_set(L"theme", value_over_limit).c_str(), 128, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_SET);
    CHECK(!message.fields_valid);
    CHECK(message.value_json.empty());
}

static void test_json_escapes_count_as_decoded_code_units()
{
    sh_config_message message = sh_extract_config_message(
        L"{\"cmd\":\"configSet\",\"key\":\"th\\u0065me\","
        L"\"valueJson\":\"\\\"dark\\\"\"}",
        5, 6);
    CHECK(message.kind == SH_CONFIG_MESSAGE_SET);
    CHECK(message.fields_valid);
    CHECK(message.key == L"theme");
    CHECK(message.value_json == L"\"dark\"");

    message = sh_extract_config_message(
        L"{\"cmd\":\"configGet\",\"key\":\"\\u0061\\u0062\"}",
        1, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_GET);
    CHECK(!message.fields_valid);
    CHECK(message.key.empty());
}

static void test_only_top_level_command_selects_the_config_path()
{
    sh_config_message message = sh_extract_config_message(
        L"{\"nested\":{\"cmd\":\"configSet\"},\"cmd\":\"refresh\","
        L"\"key\":\"theme\",\"valueJson\":\"\\\"dark\\\"\"}",
        128, 65536);
    CHECK(message.kind == SH_CONFIG_MESSAGE_OTHER);
    CHECK(!message.fields_valid);
}

int main()
{
    test_decoded_limits_are_enforced();
    test_json_escapes_count_as_decoded_code_units();
    test_only_top_level_command_selects_the_config_path();
    if (g_failed) {
        std::fprintf(stderr, "config_message_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::puts("config_message_test: OK");
    return 0;
}
