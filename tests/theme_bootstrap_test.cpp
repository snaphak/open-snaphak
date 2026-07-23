#include <cstdio>
#include <string>

#include "theme_bootstrap.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",                       \
                     __FILE__, __LINE__, #expr);                                \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static void test_dark_is_seeded_before_navigation()
{
    std::wstring html =
        L"<!doctype html><html lang=\"en\"><head></head><body></body></html>";
    CHECK(sh_theme_json_is_dark("\"dark\""));
    CHECK(sh_theme_seed_html(html, "\"dark\""));
    CHECK(html.find(L"<html class=\"dark\" lang=\"en\">") !=
          std::wstring::npos);
}

static void test_light_and_invalid_values_leave_default_markup()
{
    const std::wstring original =
        L"<!doctype html><html lang=\"en\"><head></head></html>";
    std::wstring light = original;
    std::wstring invalid = original;

    CHECK(!sh_theme_json_is_dark("\"light\""));
    CHECK(!sh_theme_json_is_dark("\"system\""));
    CHECK(!sh_theme_json_is_dark(nullptr));
    CHECK(sh_theme_seed_html(light, "\"light\""));
    CHECK(light == original);
    CHECK(sh_theme_seed_html(invalid, "not-json"));
    CHECK(invalid == original);
}

static void test_marker_must_exist_exactly_once()
{
    std::wstring missing = L"<html><head></head></html>";
    std::wstring duplicate =
        L"<html lang=\"en\"><template><html lang=\"en\"></template>";
    const std::wstring missing_original = missing;
    const std::wstring duplicate_original = duplicate;

    CHECK(!sh_theme_seed_html(missing, "\"dark\""));
    CHECK(missing == missing_original);
    CHECK(!sh_theme_seed_html(duplicate, "\"dark\""));
    CHECK(duplicate == duplicate_original);
}

int main()
{
    test_dark_is_seeded_before_navigation();
    test_light_and_invalid_values_leave_default_markup();
    test_marker_must_exist_exactly_once();
    if (g_failed) {
        std::fprintf(stderr, "theme_bootstrap_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::puts("theme_bootstrap_test: OK");
    return 0;
}
