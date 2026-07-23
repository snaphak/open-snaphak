#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "config.h"

static int g_failed;
static unsigned long g_root_serial;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

void backend_log(const char *line)
{
    (void)line;
}

static int make_temp_root(wchar_t *out, size_t capacity)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetTempPathW((DWORD)(sizeof(base) / sizeof(base[0])), base);
    unsigned long pid = GetCurrentProcessId();
    unsigned long tick = GetTickCount() + ++g_root_serial;
    if (!n || n >= sizeof(base) / sizeof(base[0])) return 0;
    if (_snwprintf_s(out, capacity, _TRUNCATE, L"%sSnapmapPlusConfig-%lu-%lu",
                     base, pid, tick) < 0)
        return 0;
    return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_file(const wchar_t *path, const char *bytes, size_t length)
{
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return 0;
    if (length && fwrite(bytes, 1, length, file) != length) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int make_config_dir(const wchar_t *root, wchar_t *dir, wchar_t *path)
{
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\snapmap-plus", root);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\config.json", dir);
    return CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int file_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned int count_files_with_prefix(const wchar_t *dir,
                                            const wchar_t *prefix)
{
    WIN32_FIND_DATAW found;
    HANDLE search;
    wchar_t pattern[MAX_PATH];
    size_t prefix_length = wcslen(prefix);
    unsigned int count = 0;
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcsncmp(found.cFileName, prefix, prefix_length) == 0)
            count++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return count;
}

static int read_file(const wchar_t *path, char **out, size_t *out_length)
{
    FILE *file = NULL;
    long length;
    char *bytes;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    bytes = (char *)malloc((size_t)length + 1);
    if (!bytes) { fclose(file); return 0; }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return 0;
    }
    fclose(file);
    bytes[length] = 0;
    *out = bytes;
    if (out_length) *out_length = (size_t)length;
    return 1;
}

static void cleanup_temp_root(const wchar_t *root)
{
    WIN32_FIND_DATAW found;
    HANDLE search;
    wchar_t config[MAX_PATH], dir[MAX_PATH], pattern[MAX_PATH];
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\snapmap-plus", root);
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") != 0 &&
                wcscmp(found.cFileName, L"..") != 0 &&
                !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                _snwprintf_s(config, MAX_PATH, _TRUNCATE,
                             L"%s\\%s", dir, found.cFileName);
                DeleteFileW(config);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(dir);
    RemoveDirectoryW(root);
}

static void test_first_run_creates_defaults_and_getter_contract(void)
{
    wchar_t root[MAX_PATH], path[MAX_PATH];
    char *disk = NULL;
    char value[16];
    char short_buffer[4] = { 'X', 'X', 'X', 0 };
    size_t disk_length = 0;
    unsigned int flags = 99;
    int needed;
    const char *expected =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"settings\": {\n"
        "    \"theme\": \"light\"\n"
        "  }\n"
        "}\n";

    CHECK(make_temp_root(root, MAX_PATH));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);

    CHECK(sh_config_init() == 1);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"%s\\snapmap-plus\\config.json", root);
    CHECK(read_file(path, &disk, &disk_length));
    CHECK(disk_length == strlen(expected));
    CHECK(disk && strcmp(disk, expected) == 0);

    needed = sh_config_get_json("theme", NULL, 0, &flags);
    CHECK(needed == 7);
    CHECK(flags == 0);
    CHECK(sh_config_get_json("theme", short_buffer,
                             (int)sizeof(short_buffer), NULL) == 7);
    CHECK(short_buffer[0] == 'X');
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), NULL) == 7);
    CHECK(strcmp(value, "\"light\"") == 0);
    CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) == 5);
    CHECK(strcmp(value, "light") == 0);
    CHECK(sh_config_get_json("missing", value, (int)sizeof(value), NULL) < 0);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_loads_existing_theme_without_rewriting(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    size_t disk_length = 0;
    const char existing[] =
        "{\"schema_version\":1,\"future_root\":{\"enabled\":true},"
        "\"settings\":{\"future_array\":[1,null],\"theme\":\"dark\"}}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, existing, sizeof(existing) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);

    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) == 4);
    CHECK(strcmp(value, "dark") == 0);
    CHECK(read_file(path, &disk, &disk_length));
    CHECK(disk_length == sizeof(existing) - 1);
    CHECK(disk && memcmp(disk, existing, sizeof(existing) - 1) == 0);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_sets_valid_theme_and_preserves_unknown_values(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    const char existing[] =
        "{"
        "\"schema_version\":1,"
        "\"future_root\":null,"
        "\"settings\":{"
        "\"future_bool\":true,"
        "\"future_number\":1.25e2,"
        "\"future_string\":\"kept\","
        "\"future_array\":[1,false],"
        "\"future_object\":{\"nested\":\"yes\"},"
        "\"future_null\":null,"
        "\"theme\":\"light\""
        "}"
        "}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, existing, sizeof(existing) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);

    CHECK(sh_config_set_json("theme", "\"dark\"") == SH_CONFIG_SET_PERSISTED);
    CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) == 4);
    CHECK(strcmp(value, "dark") == 0);
    CHECK(sh_config_set_json("missing", "\"dark\"") == SH_CONFIG_SET_REJECTED);
    CHECK(sh_config_set_json("theme", "true") == SH_CONFIG_SET_REJECTED);
    CHECK(sh_config_set_json("theme", "\"system\"") == SH_CONFIG_SET_REJECTED);
    CHECK(sh_config_set_json("theme", "\"dark\" trailing") == SH_CONFIG_SET_REJECTED);

    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"dark\"") != NULL);
    CHECK(disk && strstr(disk, "\"future_root\": null") != NULL);
    CHECK(disk && strstr(disk, "\"future_bool\": true") != NULL);
    CHECK(disk && strstr(disk, "\"future_number\": 1.25e2") != NULL);
    CHECK(disk && strstr(disk, "\"future_string\": \"kept\"") != NULL);
    CHECK(disk && strstr(disk, "\"future_array\": [1,false]") != NULL);
    CHECK(disk && strstr(disk, "\"future_object\": {\"nested\":\"yes\"}") != NULL);
    CHECK(disk && strstr(disk, "\"future_null\": null") != NULL);
    if (disk) {
        const char *theme_position = strstr(disk, "\"theme\": \"dark\"");
        const char *future_position = strstr(disk, "\"future_bool\": true");
        CHECK(theme_position != NULL);
        CHECK(future_position != NULL);
        if (theme_position && future_position)
            CHECK(theme_position < future_position);
    }

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_repairs_missing_and_invalid_registered_values(void)
{
    static const char *documents[] = {
        "{\"schema_version\":1,\"future\":true}",
        "{\"schema_version\":1,\"settings\":{\"future\":42}}",
        "{\"schema_version\":1,\"settings\":{\"theme\":\"system\",\"future\":42}}",
        "{\"schema_version\":1,\"settings\":{\"theme\":false,\"future\":42}}"
    };
    size_t i;

    for (i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char value[16];
        char *disk = NULL;
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        CHECK(make_config_dir(root, dir, path));
        CHECK(write_file(path, documents[i], strlen(documents[i])));
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);

        CHECK(sh_config_init() == 1);
        CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) == 5);
        CHECK(strcmp(value, "light") == 0);
        CHECK(sh_config_get_json("theme", NULL, 0, &flags) == 7);
        CHECK((flags & SH_CONFIG_STATUS_REPAIRED) != 0);
        CHECK(read_file(path, &disk, NULL));
        CHECK(disk && strstr(disk, "\"theme\": \"light\"") != NULL);
        if (strstr(documents[i], "\"future\":true"))
            CHECK(disk && strstr(disk, "\"future\": true") != NULL);
        if (strstr(documents[i], "\"future\":42"))
            CHECK(disk && strstr(disk, "\"future\": 42") != NULL);

        free(disk);
        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_setter_rereads_external_edits_and_recreates_deleted_file(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char *disk = NULL;
    const char initial[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\"},\"sentinel\":1}";
    const char external[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\",\"external\":true},"
        "\"sentinel\":2}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, initial, sizeof(initial) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);

    CHECK(write_file(path, external, sizeof(external) - 1));
    CHECK(sh_config_set_json("theme", "\"dark\"") == SH_CONFIG_SET_PERSISTED);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"sentinel\": 2") != NULL);
    CHECK(disk && strstr(disk, "\"external\": true") != NULL);
    free(disk);
    disk = NULL;

    CHECK(DeleteFileW(path));
    CHECK(sh_config_set_json("theme", "\"dark\"") == SH_CONFIG_SET_PERSISTED);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"dark\"") != NULL);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_accepts_bom_and_canonicalizes_registered_strings(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    unsigned int flags = 0;
    const char bom_document[] =
        "\xEF\xBB\xBF{\"schema_version\":1,\"settings\":{\"theme\":\"dark\"}}";
    const char escaped_document[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"d\\u0061rk\"}}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, bom_document, sizeof(bom_document) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) == 4);
    CHECK(strcmp(value, "dark") == 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && memcmp(disk, bom_document, sizeof(bom_document) - 1) == 0);
    free(disk);
    disk = NULL;

    CHECK(write_file(path, escaped_document, sizeof(escaped_document) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
    CHECK(strcmp(value, "\"dark\"") == 0);
    CHECK((flags & SH_CONFIG_STATUS_REPAIRED) != 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"dark\"") != NULL);
    free(disk);
    disk = NULL;

    CHECK(sh_config_set_json("theme", "\"l\\u0069ght\"") ==
          SH_CONFIG_SET_PERSISTED);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), NULL) == 7);
    CHECK(strcmp(value, "\"light\"") == 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"light\"") != NULL);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_corrupt_file_is_backed_up_and_replaced(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], backup[MAX_PATH];
    char value[16];
    char *disk = NULL;
    char *saved = NULL;
    unsigned int flags = 0;
    const char corrupt[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"dark\",\"same\":1,"
        "\"same\":2}}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, corrupt, sizeof(corrupt) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    sh_config_test_set_timestamp(L"20260722-143012");

    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 7);
    CHECK(strcmp(value, "\"light\"") == 0);
    CHECK((flags & SH_CONFIG_STATUS_RECOVERED_CORRUPT) != 0);
    CHECK((flags & SH_CONFIG_STATUS_VOLATILE) == 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"light\"") != NULL);
    _snwprintf_s(backup, MAX_PATH, _TRUNCATE,
                 L"%s\\config.20260722-143012.corrupt.json", dir);
    CHECK(read_file(backup, &saved, NULL));
    CHECK(saved && strcmp(saved, corrupt) == 0);

    free(disk);
    free(saved);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_newer_schema_is_never_modified(void)
{
    static const char *documents[] = {
        "{\"schema_version\":2,\"settings\":{\"theme\":\"dark\"},\"future\":true}",
        "{\"schema_version\":999999999999999999999999999999999999,"
        "\"settings\":{\"theme\":\"dark\"},\"future\":true}"
    };
    size_t i;

    for (i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char value[16];
        char *disk = NULL;
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        CHECK(make_config_dir(root, dir, path));
        CHECK(write_file(path, documents[i], strlen(documents[i])));
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);

        CHECK(sh_config_init() == 1);
        CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 7);
        CHECK(strcmp(value, "\"light\"") == 0);
        CHECK((flags & SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA) != 0);
        CHECK((flags & SH_CONFIG_STATUS_VOLATILE) == 0);
        CHECK(sh_config_set_json("theme", "\"dark\"") ==
              SH_CONFIG_SET_VOLATILE);
        CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
        CHECK(strcmp(value, "\"dark\"") == 0);
        CHECK((flags & SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA) != 0);
        CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
        CHECK(read_file(path, &disk, NULL));
        CHECK(disk && strlen(documents[i]) == strlen(disk));
        CHECK(disk && memcmp(disk, documents[i], strlen(documents[i])) == 0);

        free(disk);
        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_first_write_failures_use_volatile_defaults_and_clean_temp(void)
{
    static const sh_config_test_fault faults[] = {
        SH_CONFIG_TEST_FAIL_CREATE_TEMP,
        SH_CONFIG_TEST_FAIL_WRITE,
        SH_CONFIG_TEST_FAIL_FLUSH,
        SH_CONFIG_TEST_FAIL_REPLACE
    };
    size_t i;

    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char value[16];
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\snapmap-plus", root);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\config.json", dir);
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);
        sh_config_test_fail_next(faults[i]);

        CHECK(sh_config_init() == 0);
        CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 7);
        CHECK(strcmp(value, "\"light\"") == 0);
        CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
        CHECK(!file_exists(path));
        CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);

        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_mutation_failures_keep_old_file_and_session_value(void)
{
    static const sh_config_test_fault faults[] = {
        SH_CONFIG_TEST_FAIL_CREATE_TEMP,
        SH_CONFIG_TEST_FAIL_WRITE,
        SH_CONFIG_TEST_FAIL_FLUSH,
        SH_CONFIG_TEST_FAIL_REPLACE
    };
    const char original[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\"},\"keep\":true}";
    size_t i;

    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char value[16];
        char *disk = NULL;
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        CHECK(make_config_dir(root, dir, path));
        CHECK(write_file(path, original, sizeof(original) - 1));
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);
        CHECK(sh_config_init() == 1);
        sh_config_test_fail_next(faults[i]);

        CHECK(sh_config_set_json("theme", "\"dark\"") ==
              SH_CONFIG_SET_VOLATILE);
        CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
        CHECK(strcmp(value, "\"dark\"") == 0);
        CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
        CHECK(read_file(path, &disk, NULL));
        CHECK(disk && memcmp(disk, original, sizeof(original) - 1) == 0);
        CHECK(disk && strlen(disk) == sizeof(original) - 1);
        CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);

        free(disk);
        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_partial_existing_replace_keeps_old_file(void)
{
    const char original[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\"},\"keep\":true}";
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    unsigned int flags = 0;

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, original, sizeof(original) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    sh_config_test_fail_next(SH_CONFIG_TEST_FAIL_REPLACE_KEEP_NAMES);

    CHECK(sh_config_set_json("theme", "\"dark\"") ==
          SH_CONFIG_SET_VOLATILE);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
    CHECK(strcmp(value, "\"dark\"") == 0);
    CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && memcmp(disk, original, sizeof(original) - 1) == 0);
    CHECK(disk && strlen(disk) == sizeof(original) - 1);
    CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);
    CHECK(count_files_with_prefix(dir, L"config.json.rollback.") == 0);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_partial_existing_replace_restores_moved_old_file(void)
{
    const char original[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\"},\"keep\":true}";
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    unsigned int flags = 0;

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, original, sizeof(original) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    sh_config_test_fail_next(SH_CONFIG_TEST_FAIL_REPLACE_MOVED_OLD);

    CHECK(sh_config_set_json("theme", "\"dark\"") ==
          SH_CONFIG_SET_VOLATILE);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
    CHECK(strcmp(value, "\"dark\"") == 0);
    CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && memcmp(disk, original, sizeof(original) - 1) == 0);
    CHECK(disk && strlen(disk) == sizeof(original) - 1);
    CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);
    CHECK(count_files_with_prefix(dir, L"config.json.rollback.") == 0);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_partial_startup_repair_restores_moved_old_file(void)
{
    const char original[] =
        "{\"schema_version\":1,\"settings\":{\"future\":42},\"keep\":true}";
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char value[16];
    char *disk = NULL;
    unsigned int flags = 0;

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, original, sizeof(original) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    sh_config_test_fail_next(SH_CONFIG_TEST_FAIL_REPLACE_MOVED_OLD);

    CHECK(sh_config_init() == 0);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 7);
    CHECK(strcmp(value, "\"light\"") == 0);
    CHECK((flags & SH_CONFIG_STATUS_REPAIRED) != 0);
    CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && memcmp(disk, original, sizeof(original) - 1) == 0);
    CHECK(disk && strlen(disk) == sizeof(original) - 1);
    CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);
    CHECK(count_files_with_prefix(dir, L"config.json.rollback.") == 0);

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_startup_restores_interrupted_existing_replace(void)
{
    const char original[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"dark\"},\"keep\":true}";
    const char replacement[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\"},\"keep\":true}";
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    wchar_t rollback[MAX_PATH], temp[MAX_PATH];
    char value[16];
    char *disk = NULL;
    unsigned int flags = 0;

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    _snwprintf_s(rollback, MAX_PATH, _TRUNCATE,
                 L"%s\\config.json.rollback.123.456", dir);
    _snwprintf_s(temp, MAX_PATH, _TRUNCATE,
                 L"%s\\config.json.tmp.123.456", dir);
    CHECK(write_file(rollback, original, sizeof(original) - 1));
    CHECK(write_file(temp, replacement, sizeof(replacement) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);

    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), &flags) == 6);
    CHECK(strcmp(value, "\"dark\"") == 0);
    CHECK(flags == 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && memcmp(disk, original, sizeof(original) - 1) == 0);
    CHECK(disk && strlen(disk) == sizeof(original) - 1);
    CHECK(!file_exists(rollback));
    CHECK(!file_exists(temp));

    free(disk);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_failed_corrupt_recovery_leaves_original_untouched(void)
{
    static const sh_config_test_fault faults[] = {
        SH_CONFIG_TEST_FAIL_BACKUP,
        SH_CONFIG_TEST_FAIL_REPLACE
    };
    const char corrupt[] = "{\"schema_version\":1,\"settings\":";
    size_t i;

    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char *disk = NULL;
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        CHECK(make_config_dir(root, dir, path));
        CHECK(write_file(path, corrupt, sizeof(corrupt) - 1));
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);
        sh_config_test_set_timestamp(L"20260722-150000");
        sh_config_test_fail_next(faults[i]);

        CHECK(sh_config_init() == 0);
        CHECK(sh_config_get_json("theme", NULL, 0, &flags) == 7);
        CHECK((flags & SH_CONFIG_STATUS_VOLATILE) != 0);
        CHECK((flags & SH_CONFIG_STATUS_RECOVERED_CORRUPT) == 0);
        CHECK(read_file(path, &disk, NULL));
        CHECK(disk && memcmp(disk, corrupt, sizeof(corrupt) - 1) == 0);
        CHECK(disk && strlen(disk) == sizeof(corrupt) - 1);
        CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);
        CHECK(count_files_with_prefix(dir, L"config.20260722-150000") == 0);

        free(disk);
        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_partial_replace_result_is_reconciled_from_disk(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], backup[MAX_PATH];
    char *disk = NULL;
    char *saved = NULL;
    unsigned int flags = 0;
    const char corrupt[] = "{\"schema_version\":1,\"settings\":";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, corrupt, sizeof(corrupt) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    sh_config_test_set_timestamp(L"20260722-150500");
    sh_config_test_fail_next(SH_CONFIG_TEST_FAIL_REPLACE_AFTER_COMMIT);

    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_json("theme", NULL, 0, &flags) == 7);
    CHECK((flags & SH_CONFIG_STATUS_RECOVERED_CORRUPT) != 0);
    CHECK((flags & SH_CONFIG_STATUS_VOLATILE) == 0);
    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"theme\": \"light\"") != NULL);
    _snwprintf_s(backup, MAX_PATH, _TRUNCATE,
                 L"%s\\config.20260722-150500.corrupt.json", dir);
    CHECK(read_file(backup, &saved, NULL));
    CHECK(saved && strcmp(saved, corrupt) == 0);
    CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);

    free(disk);
    free(saved);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_oversized_file_is_recovered_with_exact_backup(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], backup[MAX_PATH];
    char *oversized;
    char *saved = NULL;
    size_t length = SH_CONFIG_MAX_FILE_BYTES + 1u;
    unsigned int flags = 0;

    oversized = (char *)malloc(length);
    CHECK(oversized != NULL);
    if (!oversized) return;
    memset(oversized, 'x', length);
    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, oversized, length));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    sh_config_test_set_timestamp(L"20260722-151500");

    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_json("theme", NULL, 0, &flags) == 7);
    CHECK((flags & SH_CONFIG_STATUS_RECOVERED_CORRUPT) != 0);
    _snwprintf_s(backup, MAX_PATH, _TRUNCATE,
                 L"%s\\config.20260722-151500.corrupt.json", dir);
    {
        size_t saved_length = 0;
        CHECK(read_file(backup, &saved, &saved_length));
        CHECK(saved_length == length);
        CHECK(saved && memcmp(saved, oversized, length) == 0);
    }
    CHECK(count_files_with_prefix(dir, L"config.json.tmp.") == 0);

    free(saved);
    free(oversized);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_structural_errors_are_recovered(void)
{
    static const char *documents[] = {
        "[]",
        "{\"settings\":{\"theme\":\"dark\"}}",
        "{\"schema_version\":0,\"settings\":{\"theme\":\"dark\"}}",
        "{\"schema_version\":-1,\"settings\":{\"theme\":\"dark\"}}",
        "{\"schema_version\":1.0,\"settings\":{\"theme\":\"dark\"}}",
        "{\"schema_version\":1e0,\"settings\":{\"theme\":\"dark\"}}",
        "{\"schema_version\":1,\"settings\":true}"
    };
    size_t i;

    for (i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
        wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
        char *disk = NULL;
        unsigned int flags = 0;

        CHECK(make_temp_root(root, MAX_PATH));
        CHECK(make_config_dir(root, dir, path));
        CHECK(write_file(path, documents[i], strlen(documents[i])));
        sh_config_test_reset();
        sh_config_test_set_local_appdata(root);
        sh_config_test_set_timestamp(L"20260722-160000");

        CHECK(sh_config_init() == 1);
        CHECK(sh_config_get_json("theme", NULL, 0, &flags) == 7);
        CHECK((flags & SH_CONFIG_STATUS_RECOVERED_CORRUPT) != 0);
        CHECK(read_file(path, &disk, NULL));
        CHECK(disk && strstr(disk, "\"theme\": \"light\"") != NULL);
        CHECK(count_files_with_prefix(dir, L"config.20260722-160000") == 1);

        free(disk);
        sh_config_test_reset();
        cleanup_temp_root(root);
    }
}

static void test_backup_name_collision_never_overwrites_previous_recovery(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    wchar_t first_backup[MAX_PATH], second_backup[MAX_PATH];
    char *saved = NULL;
    const char previous[] = "previous recovery";
    const char corrupt[] = "{\"schema_version\":";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    _snwprintf_s(first_backup, MAX_PATH, _TRUNCATE,
                 L"%s\\config.20260722-161500.corrupt.json", dir);
    _snwprintf_s(second_backup, MAX_PATH, _TRUNCATE,
                 L"%s\\config.20260722-161500.1.corrupt.json", dir);
    CHECK(write_file(first_backup, previous, sizeof(previous) - 1));
    CHECK(write_file(path, corrupt, sizeof(corrupt) - 1));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    sh_config_test_set_timestamp(L"20260722-161500");

    CHECK(sh_config_init() == 1);
    CHECK(read_file(first_backup, &saved, NULL));
    CHECK(saved && strcmp(saved, previous) == 0);
    free(saved);
    saved = NULL;
    CHECK(read_file(second_backup, &saved, NULL));
    CHECK(saved && strcmp(saved, corrupt) == 0);

    free(saved);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_rejects_oversized_api_inputs_without_mutation(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    char long_key[130];
    char *long_value;
    char value[16];
    char *before = NULL, *after = NULL;
    size_t length = SH_CONFIG_MAX_FILE_BYTES + 1u;

    long_value = (char *)malloc(length + 1u);
    CHECK(long_value != NULL);
    if (!long_value) return;
    memset(long_value, ' ', length);
    long_value[length] = 0;
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = 0;

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    CHECK(read_file(path, &before, NULL));

    CHECK(sh_config_set_json(long_key, "\"dark\"") ==
          SH_CONFIG_SET_REJECTED);
    CHECK(sh_config_set_json("theme", long_value) ==
          SH_CONFIG_SET_REJECTED);
    CHECK(sh_config_get_json(long_key, value, (int)sizeof(value), NULL) < 0);
    CHECK(sh_config_get_json("theme", value, (int)sizeof(value), NULL) == 7);
    CHECK(strcmp(value, "\"light\"") == 0);
    CHECK(read_file(path, &after, NULL));
    CHECK(before && after && strcmp(before, after) == 0);

    free(before);
    free(after);
    free(long_value);
    sh_config_test_reset();
    cleanup_temp_root(root);
}

static int start_child_set(const wchar_t *root, const wchar_t *theme,
                           unsigned int hold_ms, PROCESS_INFORMATION *process)
{
    STARTUPINFOW startup;
    wchar_t executable[MAX_PATH];
    wchar_t command[3 * MAX_PATH];
    if (!process ||
        !GetModuleFileNameW(NULL, executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0]))))
        return 0;
    memset(&startup, 0, sizeof(startup));
    memset(process, 0, sizeof(*process));
    startup.cb = sizeof(startup);
    if (_snwprintf_s(command, sizeof(command) / sizeof(command[0]), _TRUNCATE,
                     L"\"%s\" --child-set \"%s\" %s %u",
                     executable, root, theme, hold_ms) < 0)
        return 0;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, process))
        return 0;
    CloseHandle(process->hThread);
    process->hThread = NULL;
    return 1;
}

static int wait_child_ok(PROCESS_INFORMATION *process)
{
    DWORD exit_code = (DWORD)-1;
    int ok;
    if (!process || !process->hProcess) return 0;
    ok = WaitForSingleObject(process->hProcess, 10000) == WAIT_OBJECT_0 &&
         GetExitCodeProcess(process->hProcess, &exit_code) &&
         exit_code == 0;
    CloseHandle(process->hProcess);
    process->hProcess = NULL;
    return ok;
}

static void test_process_writers_are_serialized(void)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    PROCESS_INFORMATION first = {0}, second = {0};
    char value[16];
    char *disk = NULL;
    const char initial[] =
        "{\"schema_version\":1,\"settings\":{\"theme\":\"light\","
        "\"future\":{\"keep\":true}},\"sentinel\":\"preserve\"}";

    CHECK(make_temp_root(root, MAX_PATH));
    CHECK(make_config_dir(root, dir, path));
    CHECK(write_file(path, initial, sizeof(initial) - 1));
    CHECK(start_child_set(root, L"dark", 400, &first));
    Sleep(50);
    CHECK(start_child_set(root, L"light", 0, &second));
    CHECK(wait_child_ok(&first));
    CHECK(wait_child_ok(&second));

    CHECK(read_file(path, &disk, NULL));
    CHECK(disk && strstr(disk, "\"sentinel\": \"preserve\"") != NULL);
    CHECK(disk && strstr(disk, "\"future\": {\"keep\":true}") != NULL);
    free(disk);
    disk = NULL;

    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    CHECK(sh_config_get_string("theme", value, (int)sizeof(value)) > 0);
    CHECK(strcmp(value, "light") == 0 || strcmp(value, "dark") == 0);

    sh_config_test_reset();
    cleanup_temp_root(root);
}

static void test_shared_interface_uses_registered_config_service(void)
{
    wchar_t root[MAX_PATH];
    sh_iface *iface;
    char value[16] = {0};
    unsigned int flags = 99;

    CHECK(make_temp_root(root, MAX_PATH));
    sh_config_test_reset();
    sh_config_test_set_local_appdata(root);
    CHECK(sh_config_init() == 1);
    iface = sh_iface_create();
    CHECK(iface != NULL);
    sh_config_bind_iface_slots();
    CHECK(iface && iface->vtbl && iface->vtbl->config_get_json);
    CHECK(iface && iface->vtbl && iface->vtbl->config_set_json);
    if (iface && iface->vtbl && iface->vtbl->config_get_json &&
        iface->vtbl->config_set_json) {
        CHECK(iface->vtbl->config_get_json(
                  iface, "theme", value, (int)sizeof(value), &flags) == 7);
        CHECK(strcmp(value, "\"light\"") == 0);
        CHECK(flags == 0);
        CHECK(iface->vtbl->config_set_json(iface, "theme", "\"dark\"") ==
              SH_CONFIG_SET_PERSISTED);
        CHECK(iface->vtbl->config_set_json(iface, "unknown", "true") ==
              SH_CONFIG_SET_REJECTED);
        CHECK(iface->vtbl->config_get_json(
                  iface, "theme", value, (int)sizeof(value), NULL) == 6);
        CHECK(strcmp(value, "\"dark\"") == 0);
    }

    sh_config_test_reset();
    cleanup_temp_root(root);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 5 && wcscmp(argv[1], L"--child-set") == 0) {
        const char *value_json;
        unsigned int hold_ms = (unsigned int)_wtoi(argv[4]);
        if (wcscmp(argv[3], L"dark") == 0)
            value_json = "\"dark\"";
        else if (wcscmp(argv[3], L"light") == 0)
            value_json = "\"light\"";
        else
            return 2;
        sh_config_test_reset();
        sh_config_test_set_local_appdata(argv[2]);
        if (sh_config_init() != 1) return 3;
        sh_config_test_hold_mutex_ms(hold_ms);
        if (sh_config_set_json("theme", value_json) !=
            SH_CONFIG_SET_PERSISTED)
            return 4;
        sh_config_test_reset();
        return 0;
    }
    test_first_run_creates_defaults_and_getter_contract();
    test_loads_existing_theme_without_rewriting();
    test_sets_valid_theme_and_preserves_unknown_values();
    test_repairs_missing_and_invalid_registered_values();
    test_setter_rereads_external_edits_and_recreates_deleted_file();
    test_accepts_bom_and_canonicalizes_registered_strings();
    test_corrupt_file_is_backed_up_and_replaced();
    test_newer_schema_is_never_modified();
    test_first_write_failures_use_volatile_defaults_and_clean_temp();
    test_mutation_failures_keep_old_file_and_session_value();
    test_partial_existing_replace_keeps_old_file();
    test_partial_existing_replace_restores_moved_old_file();
    test_partial_startup_repair_restores_moved_old_file();
    test_startup_restores_interrupted_existing_replace();
    test_failed_corrupt_recovery_leaves_original_untouched();
    test_partial_replace_result_is_reconciled_from_disk();
    test_oversized_file_is_recovered_with_exact_backup();
    test_structural_errors_are_recovered();
    test_backup_name_collision_never_overwrites_previous_recovery();
    test_rejects_oversized_api_inputs_without_mutation();
    test_process_writers_are_serialized();
    test_shared_interface_uses_registered_config_service();
    if (g_failed) {
        fprintf(stderr, "config_test: %d failure(s)\n", g_failed);
        return 1;
    }
    puts("config_test: OK");
    return 0;
}
