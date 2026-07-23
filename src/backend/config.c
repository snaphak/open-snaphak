/* config.c -- backend-owned registry + persistence for %LOCALAPPDATA%\snapmap-plus\config.json. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "backend_log.h"
#include "config.h"
#include "config_json.h"

#define SH_CONFIG_MUTEX_NAME L"Local\\SnapmapPlus.Config"
#define SH_CONFIG_MUTEX_WAIT_MS 2000u
#define SH_CONFIG_JSON_DEPTH 64u
#define SH_CONFIG_MAX_KEY_BYTES 128u

typedef int (*config_validator)(const char *value_json);
typedef const char *(*config_normalizer)(const char *value_json);

typedef struct config_descriptor {
    const char *key;
    sh_json_kind kind;
    const char *default_json;
    unsigned int access;
    config_validator validate;
    config_normalizer normalize;
} config_descriptor;

typedef struct config_document {
    sh_json_object root;
    sh_json_object settings;
} config_document;

typedef enum config_read_result {
    CONFIG_READ_OK,
    CONFIG_READ_MISSING,
    CONFIG_READ_TOO_LARGE,
    CONFIG_READ_IO_ERROR
} config_read_result;

typedef enum config_parse_result {
    CONFIG_PARSE_INVALID,
    CONFIG_PARSE_SUPPORTED,
    CONFIG_PARSE_FUTURE
} config_parse_result;

typedef enum config_fault {
    CONFIG_FAIL_NONE,
    CONFIG_FAIL_RESOLVE,
    CONFIG_FAIL_READ,
    CONFIG_FAIL_CREATE_TEMP,
    CONFIG_FAIL_WRITE,
    CONFIG_FAIL_FLUSH,
    CONFIG_FAIL_REPLACE,
    CONFIG_FAIL_BACKUP,
    CONFIG_FAIL_REPLACE_AFTER_COMMIT,
    CONFIG_FAIL_REPLACE_KEEP_NAMES,
    CONFIG_FAIL_REPLACE_MOVED_OLD
} config_fault;

static int validate_theme(const char *value_json);
static const char *normalize_theme(const char *value_json);

static const config_descriptor g_registry[] = {
    {
        "theme", SH_JSON_STRING, "\"light\"",
        SH_CONFIG_BACKEND_READ | SH_CONFIG_BACKEND_WRITE |
        SH_CONFIG_UI_READ | SH_CONFIG_UI_WRITE,
        validate_theme,
        normalize_theme
    }
};

#define CONFIG_COUNT (sizeof(g_registry) / sizeof(g_registry[0]))

static SRWLOCK g_config_lock = SRWLOCK_INIT;
static int g_initialized;
static int g_future_schema;
static unsigned int g_status_flags;
static wchar_t *g_config_dir;
static wchar_t *g_config_path;
static char *g_values[CONFIG_COUNT];
static LONG g_temp_counter;

#ifdef SH_CONFIG_TESTING
static wchar_t *g_test_local_appdata;
static sh_config_test_fault g_test_fault;
static wchar_t *g_test_timestamp;
static unsigned int g_test_hold_mutex_ms;
#endif

static int file_matches_bytes(const wchar_t *path, const char *bytes,
                              size_t length);

static char *copy_string(const char *value)
{
    size_t length;
    char *copy;
    if (!value) return NULL;
    length = strlen(value);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

static wchar_t *copy_wstring(const wchar_t *value)
{
    size_t length;
    wchar_t *copy;
    if (!value) return NULL;
    length = wcslen(value);
    if (length > (SIZE_MAX / sizeof(wchar_t)) - 1) return NULL;
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (!copy) return NULL;
    memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    return copy;
}

static wchar_t *join_path(const wchar_t *left, const wchar_t *right)
{
    size_t left_length, right_length, total;
    int separator;
    wchar_t *joined;
    if (!left || !right) return NULL;
    left_length = wcslen(left);
    right_length = wcslen(right);
    separator = left_length > 0 && left[left_length - 1] != L'\\' &&
                left[left_length - 1] != L'/';
    if (left_length > SIZE_MAX - right_length - (size_t)separator - 1) return NULL;
    total = left_length + (size_t)separator + right_length;
    if (total > (SIZE_MAX / sizeof(wchar_t)) - 1) return NULL;
    joined = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
    if (!joined) return NULL;
    memcpy(joined, left, left_length * sizeof(wchar_t));
    if (separator) joined[left_length++] = L'\\';
    memcpy(joined + left_length, right, (right_length + 1) * sizeof(wchar_t));
    return joined;
}

static void clear_values_locked(void)
{
    size_t i;
    for (i = 0; i < CONFIG_COUNT; i++) {
        free(g_values[i]);
        g_values[i] = NULL;
    }
}

static void clear_state_locked(void)
{
    clear_values_locked();
    free(g_config_dir);
    free(g_config_path);
    g_config_dir = NULL;
    g_config_path = NULL;
    g_status_flags = 0;
    g_initialized = 0;
    g_future_schema = 0;
}

static int load_defaults_locked(void)
{
    char *values[CONFIG_COUNT] = {0};
    size_t i;
    for (i = 0; i < CONFIG_COUNT; i++) {
        values[i] = copy_string(g_registry[i].default_json);
        if (!values[i]) {
            size_t j;
            for (j = 0; j < CONFIG_COUNT; j++) free(values[j]);
            return 0;
        }
    }
    clear_values_locked();
    for (i = 0; i < CONFIG_COUNT; i++) g_values[i] = values[i];
    return 1;
}

static int replace_value_locked(size_t index, const char *value_json)
{
    char *copy;
    if (index >= CONFIG_COUNT) return 0;
    copy = copy_string(value_json);
    if (!copy) return 0;
    free(g_values[index]);
    g_values[index] = copy;
    return 1;
}

static int find_descriptor(const char *key)
{
    size_t key_length;
    size_t i;
    if (!key) return -1;
    for (key_length = 0; key_length <= SH_CONFIG_MAX_KEY_BYTES; key_length++) {
        if (key[key_length] == 0) break;
    }
    if (key_length == 0 || key_length > SH_CONFIG_MAX_KEY_BYTES) return -1;
    for (i = 0; i < CONFIG_COUNT; i++) {
        if (strcmp(g_registry[i].key, key) == 0) return (int)i;
    }
    return -1;
}

static int find_descriptor_n(const char *key, size_t key_length)
{
    size_t i;
    if (!key || key_length == 0 || key_length > SH_CONFIG_MAX_KEY_BYTES)
        return -1;
    for (i = 0; i < CONFIG_COUNT; i++) {
        size_t registered_length = strlen(g_registry[i].key);
        if (registered_length == key_length &&
            memcmp(g_registry[i].key, key, key_length) == 0)
            return (int)i;
    }
    return -1;
}

static int validate_theme(const char *value_json)
{
    return normalize_theme(value_json) != NULL;
}

static const char *normalize_theme(const char *value_json)
{
    char decoded[16];
    const char *begin, *end;
    size_t length = 0;
    if (!value_json) return NULL;
    begin = value_json;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n')
        begin++;
    end = value_json + strlen(value_json);
    while (end > begin &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
        end--;
    if (!sh_json_decode_string(begin, (size_t)(end - begin),
                               decoded, sizeof(decoded), &length))
        return NULL;
    if (length == 5 && memcmp(decoded, "light", 5) == 0) return "\"light\"";
    if (length == 4 && memcmp(decoded, "dark", 4) == 0) return "\"dark\"";
    return NULL;
}

static const char *normalize_registered_value(
    const config_descriptor *descriptor, const char *value_json)
{
    sh_json_kind kind;
    size_t length;
    if (!descriptor || !value_json) return NULL;
    for (length = 0; length <= SH_CONFIG_MAX_FILE_BYTES; length++) {
        if (value_json[length] == 0) break;
    }
    if (length > SH_CONFIG_MAX_FILE_BYTES ||
        !sh_json_validate(value_json, length, SH_CONFIG_JSON_DEPTH, &kind) ||
        kind != descriptor->kind)
        return NULL;
    if (descriptor->validate && !descriptor->validate(value_json)) return NULL;
    if (descriptor->normalize) return descriptor->normalize(value_json);
    return value_json;
}

static int validate_registry(void)
{
    size_t i, j;
    if (SH_CONFIG_SCHEMA_VERSION == 0u || CONFIG_COUNT == 0u) return 0;
    for (i = 0; i < CONFIG_COUNT; i++) {
        const config_descriptor *descriptor = &g_registry[i];
        const char *normalized;
        size_t key_length = strlen(descriptor->key);
        if (key_length == 0 || key_length > SH_CONFIG_MAX_KEY_BYTES ||
            !descriptor->default_json || descriptor->access == 0)
            return 0;
        normalized = normalize_registered_value(descriptor,
                                                descriptor->default_json);
        if (!normalized || strcmp(normalized, descriptor->default_json) != 0)
            return 0;
        for (j = i + 1; j < CONFIG_COUNT; j++) {
            if (strcmp(descriptor->key, g_registry[j].key) == 0) return 0;
        }
    }
    return 1;
}

static int test_fault(config_fault fault)
{
#ifdef SH_CONFIG_TESTING
    if ((int)g_test_fault == (int)fault) {
        g_test_fault = SH_CONFIG_TEST_FAIL_NONE;
        SetLastError(ERROR_WRITE_FAULT);
        return 1;
    }
#else
    (void)fault;
#endif
    return 0;
}

static int resolve_paths_locked(void)
{
    wchar_t *base = NULL;
    PWSTR known = NULL;
    if (test_fault(CONFIG_FAIL_RESOLVE)) return 0;
#ifdef SH_CONFIG_TESTING
    if (g_test_local_appdata) base = copy_wstring(g_test_local_appdata);
#endif
    if (!base) {
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) ||
            !known)
            return 0;
        base = copy_wstring(known);
        CoTaskMemFree(known);
    }
    if (!base) return 0;
    g_config_dir = join_path(base, L"snapmap-plus");
    free(base);
    if (!g_config_dir) return 0;
    g_config_path = join_path(g_config_dir, L"config.json");
    return g_config_path != NULL;
}

static int ensure_config_dir_locked(void)
{
    int result;
    if (!g_config_dir) return 0;
    result = SHCreateDirectoryExW(NULL, g_config_dir, NULL);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

static HANDLE acquire_process_mutex(void)
{
    const wchar_t *mutex_name = SH_CONFIG_MUTEX_NAME;
#ifdef SH_CONFIG_TESTING
    wchar_t test_mutex_name[96];
    if (g_test_local_appdata) {
        const wchar_t *p = g_test_local_appdata;
        uint64_t hash = UINT64_C(1469598103934665603);
        while (*p) {
            hash ^= (uint64_t)(unsigned int)*p++;
            hash *= UINT64_C(1099511628211);
        }
        _snwprintf_s(test_mutex_name,
                     sizeof(test_mutex_name) / sizeof(test_mutex_name[0]),
                     _TRUNCATE, L"Local\\SnapmapPlus.Config.Test.%08lx%08lx",
                     (unsigned long)(hash >> 32),
                     (unsigned long)(hash & UINT32_MAX));
        mutex_name = test_mutex_name;
    }
#endif
    HANDLE mutex = CreateMutexW(NULL, FALSE, mutex_name);
    DWORD wait;
    if (!mutex) return NULL;
    wait = WaitForSingleObject(mutex, SH_CONFIG_MUTEX_WAIT_MS);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return NULL;
    }
#ifdef SH_CONFIG_TESTING
    if (g_test_hold_mutex_ms) Sleep(g_test_hold_mutex_ms);
#endif
    return mutex;
}

static void release_process_mutex(HANDLE mutex)
{
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static wchar_t *make_temp_path_locked(void)
{
    wchar_t suffix[96];
    LONG serial = InterlockedIncrement(&g_temp_counter);
    _snwprintf_s(suffix, sizeof(suffix) / sizeof(suffix[0]), _TRUNCATE,
                 L"config.json.tmp.%lu.%ld",
                 (unsigned long)GetCurrentProcessId(), (long)serial);
    return join_path(g_config_dir, suffix);
}

static int valid_transaction_suffix(const wchar_t *suffix)
{
    const wchar_t *p = suffix;
    if (!p || *p < L'0' || *p > L'9') return 0;
    while (*p >= L'0' && *p <= L'9') p++;
    if (*p++ != L'.' || *p < L'0' || *p > L'9') return 0;
    while (*p >= L'0' && *p <= L'9') p++;
    return *p == 0;
}

static wchar_t *make_rollback_path_locked(const wchar_t *temp_path)
{
    static const wchar_t temp_prefix[] = L"config.json.tmp.";
    const wchar_t *filename;
    const wchar_t *suffix;
    wchar_t name[96];
    wchar_t *path;
    DWORD error;
    if (!temp_path) return NULL;
    filename = wcsrchr(temp_path, L'\\');
    filename = filename ? filename + 1 : temp_path;
    if (wcsncmp(filename, temp_prefix,
                (sizeof(temp_prefix) / sizeof(temp_prefix[0])) - 1) != 0)
        return NULL;
    suffix = filename + (sizeof(temp_prefix) / sizeof(temp_prefix[0])) - 1;
    if (!valid_transaction_suffix(suffix) ||
        _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                     L"config.json.rollback.%s", suffix) < 0)
        return NULL;
    path = join_path(g_config_dir, name);
    if (!path) return NULL;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return path;
    }
    free(path);
    return NULL;
}

static int create_flushed_temp_locked(const char *bytes, size_t length,
                                      wchar_t **out_temp_path)
{
    wchar_t *temp_path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t written_total = 0;
    int ok = 0;
    if (!bytes || !out_temp_path || length > SH_CONFIG_MAX_FILE_BYTES) return 0;
    temp_path = make_temp_path_locked();
    if (!temp_path || test_fault(CONFIG_FAIL_CREATE_TEMP)) goto done;
    file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    while (written_total < length) {
        DWORD chunk = (DWORD)(length - written_total);
        DWORD written = 0;
        if (test_fault(CONFIG_FAIL_WRITE) ||
            !WriteFile(file, bytes + written_total, chunk, &written, NULL) ||
            written == 0)
            goto done;
        written_total += written;
    }
    if (test_fault(CONFIG_FAIL_FLUSH) || !FlushFileBuffers(file)) goto done;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    *out_temp_path = temp_path;
    temp_path = NULL;
    ok = 1;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (temp_path) {
        DeleteFileW(temp_path);
        free(temp_path);
    }
    return ok;
}

static int replace_file_locked(const wchar_t *temp_path,
                               const wchar_t *rollback_path)
{
    int ok;
    if (test_fault(CONFIG_FAIL_REPLACE_KEEP_NAMES)) {
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
        return 0;
    }
    if (test_fault(CONFIG_FAIL_REPLACE_MOVED_OLD)) {
        if (!MoveFileExW(g_config_path, rollback_path,
                         MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH))
            return 0;
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return 0;
    }
    ok = ReplaceFileW(g_config_path, temp_path, rollback_path,
                      0, NULL, NULL) != 0;
    if (ok && test_fault(CONFIG_FAIL_REPLACE_AFTER_COMMIT))
        ok = 0;
    return ok;
}

static int restore_old_after_replace_failure_locked(
    const wchar_t *temp_path, const wchar_t *rollback_path,
    const char *replacement, size_t replacement_length,
    const char *original, size_t original_length, DWORD replace_error)
{
    DWORD attrs;
    DWORD attrs_error;
    if (file_matches_bytes(g_config_path, replacement, replacement_length)) {
        DeleteFileW(rollback_path);
        DeleteFileW(temp_path);
        backend_log("config: replacement completed despite a partial ReplaceFile result");
        return 1;
    }
    if (file_matches_bytes(g_config_path, original, original_length)) {
        if (file_matches_bytes(temp_path, replacement, replacement_length))
            DeleteFileW(temp_path);
        if (file_matches_bytes(rollback_path, original, original_length))
            DeleteFileW(rollback_path);
        return 0;
    }

    attrs = GetFileAttributesW(g_config_path);
    attrs_error = attrs == INVALID_FILE_ATTRIBUTES
                      ? GetLastError()
                      : ERROR_SUCCESS;
    if (attrs == INVALID_FILE_ATTRIBUTES &&
        (attrs_error == ERROR_FILE_NOT_FOUND ||
         attrs_error == ERROR_PATH_NOT_FOUND) &&
        file_matches_bytes(rollback_path, original, original_length) &&
        MoveFileExW(rollback_path, g_config_path,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) &&
        file_matches_bytes(g_config_path, original, original_length)) {
        if (file_matches_bytes(temp_path, replacement, replacement_length))
            DeleteFileW(temp_path);
        backend_log("config: restored the previous file after a partial ReplaceFile result");
        return 0;
    }

    if (replace_error == ERROR_UNABLE_TO_REMOVE_REPLACED ||
        replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
        replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2)
        backend_log("config: preserving ReplaceFile artifacts after an unresolved partial failure");
    return 0;
}

static int write_atomic_locked(const char *bytes, size_t length,
                               int destination_exists,
                               const char *original, size_t original_length)
{
    wchar_t *temp_path = NULL;
    wchar_t *rollback_path = NULL;
    DWORD replace_error;
    int ok;
    if (!create_flushed_temp_locked(bytes, length, &temp_path)) return 0;
    if (!destination_exists) {
        ok = !test_fault(CONFIG_FAIL_REPLACE) &&
             MoveFileExW(temp_path, g_config_path,
                         MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(temp_path);
        free(temp_path);
        return ok;
    }
    if (!original || test_fault(CONFIG_FAIL_REPLACE)) {
        DeleteFileW(temp_path);
        free(temp_path);
        return 0;
    }
    rollback_path = make_rollback_path_locked(temp_path);
    if (!rollback_path) {
        DeleteFileW(temp_path);
        free(temp_path);
        return 0;
    }
    ok = replace_file_locked(temp_path, rollback_path);
    if (ok) {
        DeleteFileW(rollback_path);
    } else {
        replace_error = GetLastError();
        ok = restore_old_after_replace_failure_locked(
            temp_path, rollback_path, bytes, length,
            original, original_length, replace_error);
    }
    free(rollback_path);
    free(temp_path);
    return ok;
}

static config_read_result read_config_locked(char **out_bytes, size_t *out_length)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    char *bytes = NULL;
    DWORD total = 0;
    config_read_result result = CONFIG_READ_IO_ERROR;
    if (!out_bytes || !out_length || !g_config_path) return CONFIG_READ_IO_ERROR;
    *out_bytes = NULL;
    *out_length = 0;
    if (test_fault(CONFIG_FAIL_READ)) return CONFIG_READ_IO_ERROR;
    file = CreateFileW(g_config_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return CONFIG_READ_MISSING;
        return CONFIG_READ_IO_ERROR;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) goto done;
    if ((ULONGLONG)size.QuadPart > SH_CONFIG_MAX_FILE_BYTES) {
        result = CONFIG_READ_TOO_LARGE;
        goto done;
    }
    bytes = (char *)malloc((size_t)size.QuadPart + 1);
    if (!bytes) goto done;
    while ((LONGLONG)total < size.QuadPart) {
        DWORD chunk = (DWORD)(size.QuadPart - total);
        DWORD read = 0;
        if (!ReadFile(file, bytes + total, chunk, &read, NULL) || read == 0)
            goto done;
        total += read;
    }
    bytes[total] = 0;
    *out_bytes = bytes;
    *out_length = (size_t)total;
    bytes = NULL;
    result = CONFIG_READ_OK;
done:
    free(bytes);
    CloseHandle(file);
    return result;
}

static int restore_interrupted_replace_locked(void)
{
    static const wchar_t rollback_prefix[] = L"config.json.rollback.";
    const size_t prefix_length =
        (sizeof(rollback_prefix) / sizeof(rollback_prefix[0])) - 1;
    WIN32_FIND_DATAW found;
    HANDLE search = INVALID_HANDLE_VALUE;
    wchar_t pattern[MAX_PATH];
    wchar_t temp_name[96];
    wchar_t *best_rollback = NULL;
    wchar_t *best_temp = NULL;
    FILETIME best_time = {0, 0};
    int restored = 0;

    if (!g_config_dir || !g_config_path ||
        _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s\\config.json.rollback.*",
                     g_config_dir) < 0)
        return 0;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        const wchar_t *suffix;
        wchar_t *rollback_path;
        wchar_t *temp_path;
        DWORD temp_attrs;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ||
            wcsncmp(found.cFileName, rollback_prefix, prefix_length) != 0)
            continue;
        suffix = found.cFileName + prefix_length;
        if (!valid_transaction_suffix(suffix) ||
            _snwprintf_s(temp_name,
                         sizeof(temp_name) / sizeof(temp_name[0]),
                         _TRUNCATE, L"config.json.tmp.%s", suffix) < 0)
            continue;
        rollback_path = join_path(g_config_dir, found.cFileName);
        temp_path = join_path(g_config_dir, temp_name);
        if (!rollback_path || !temp_path) {
            free(rollback_path);
            free(temp_path);
            continue;
        }
        temp_attrs = GetFileAttributesW(temp_path);
        if (temp_attrs == INVALID_FILE_ATTRIBUTES ||
            (temp_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            free(rollback_path);
            free(temp_path);
            continue;
        }
        if (!best_rollback ||
            CompareFileTime(&found.ftLastWriteTime, &best_time) > 0) {
            free(best_rollback);
            free(best_temp);
            best_rollback = rollback_path;
            best_temp = temp_path;
            best_time = found.ftLastWriteTime;
        } else {
            free(rollback_path);
            free(temp_path);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);

    if (best_rollback &&
        MoveFileExW(best_rollback, g_config_path, MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(best_temp);
        backend_log("config: restored an interrupted atomic replacement");
        restored = 1;
    }
    free(best_rollback);
    free(best_temp);
    return restored;
}

/* Returns 1 for an in-range positive integer, 2 for positive overflow (therefore
 * unambiguously newer than this build), and 0 for a non-integer/invalid version. */
static int parse_schema_version(const char *value_json, unsigned int *out_version)
{
    const unsigned char *p;
    unsigned int value = 0;
    if (!value_json || !out_version) return 0;
    p = (const unsigned char *)value_json;
    if (*p < '1' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (UINT_MAX - digit) / 10u) {
            do { p++; } while (*p >= '0' && *p <= '9');
            return *p == 0 ? 2 : 0;
        }
        value = value * 10u + digit;
        p++;
    }
    if (*p != 0) return 0;
    *out_version = value;
    return 1;
}

static void document_free(config_document *document)
{
    if (!document) return;
    sh_json_object_free(&document->settings);
    sh_json_object_free(&document->root);
}

static int document_defaults(config_document *document)
{
    char schema_json[32];
    size_t i;
    if (!document) return 0;
    memset(document, 0, sizeof(*document));
    _snprintf_s(schema_json, sizeof(schema_json), _TRUNCATE, "%u",
                SH_CONFIG_SCHEMA_VERSION);
    if (!sh_json_object_set(&document->root, "schema_version", schema_json,
                            SH_CONFIG_JSON_DEPTH))
        goto fail;
    for (i = 0; i < CONFIG_COUNT; i++) {
        if (!sh_json_object_set(&document->settings, g_registry[i].key,
                                g_registry[i].default_json,
                                SH_CONFIG_JSON_DEPTH))
            goto fail;
    }
    return 1;
fail:
    document_free(document);
    return 0;
}

static config_parse_result document_parse(const char *bytes, size_t length,
                                          config_document *document,
                                          int *out_repaired)
{
    const char *schema_json;
    const char *settings_json;
    int schema_result;
    unsigned int version;
    size_t i;
    int repaired = 0;
    if (!bytes || !document || !out_repaired) return CONFIG_PARSE_INVALID;
    memset(document, 0, sizeof(*document));
    if (length >= 3 &&
        (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF) {
        bytes += 3;
        length -= 3;
    }
    if (!sh_json_parse_object(bytes, length, SH_CONFIG_JSON_DEPTH,
                              &document->root))
        return CONFIG_PARSE_INVALID;
    schema_json = sh_json_object_get(&document->root, "schema_version");
    schema_result = parse_schema_version(schema_json, &version);
    if (schema_result == 0) goto invalid;
    if (schema_result == 2 || version > SH_CONFIG_SCHEMA_VERSION) {
        *out_repaired = 0;
        return CONFIG_PARSE_FUTURE;
    }
    if (version != SH_CONFIG_SCHEMA_VERSION) goto invalid;

    settings_json = sh_json_object_get(&document->root, "settings");
    if (settings_json) {
        if (!sh_json_parse_object(settings_json, strlen(settings_json),
                                  SH_CONFIG_JSON_DEPTH, &document->settings))
            goto invalid;
    } else {
        repaired = 1;
    }
    for (i = 0; i < CONFIG_COUNT; i++) {
        const char *value_json =
            sh_json_object_get(&document->settings, g_registry[i].key);
        const char *normalized =
            normalize_registered_value(&g_registry[i], value_json);
        if (!normalized) {
            if (!sh_json_object_set(&document->settings, g_registry[i].key,
                                    g_registry[i].default_json,
                                    SH_CONFIG_JSON_DEPTH))
                goto invalid;
            repaired = 1;
        } else if (strcmp(normalized, value_json) != 0) {
            if (!sh_json_object_set(&document->settings, g_registry[i].key,
                                    normalized, SH_CONFIG_JSON_DEPTH))
                goto invalid;
            repaired = 1;
        }
    }
    *out_repaired = repaired;
    return CONFIG_PARSE_SUPPORTED;
invalid:
    document_free(document);
    return CONFIG_PARSE_INVALID;
}

static char *document_serialize(config_document *document, size_t *out_length)
{
    sh_json_object ordered = {0};
    char *settings_json = NULL;
    char *config_json = NULL;
    size_t settings_length = 0;
    size_t i;
    if (!document) return NULL;
    for (i = 0; i < CONFIG_COUNT; i++) {
        const char *value =
            sh_json_object_get(&document->settings, g_registry[i].key);
        if (!value ||
            !sh_json_object_set(&ordered, g_registry[i].key, value,
                                SH_CONFIG_JSON_DEPTH))
            goto done;
    }
    for (i = 0; i < document->settings.count; i++) {
        const sh_json_member *member = &document->settings.members[i];
        if (find_descriptor_n(member->key, member->key_length) < 0 &&
            !sh_json_object_set_n(&ordered, member->key, member->key_length,
                                  member->value_json, SH_CONFIG_JSON_DEPTH))
            goto done;
    }
    sh_json_object_free(&document->settings);
    document->settings = ordered;
    memset(&ordered, 0, sizeof(ordered));
    settings_json = sh_json_serialize_object(&document->settings, 2,
                                             &settings_length);
    if (!settings_json) return NULL;
    if (!sh_json_object_set(&document->root, "settings", settings_json,
                            SH_CONFIG_JSON_DEPTH))
        goto done;
    config_json = sh_json_serialize_object(&document->root, 0, out_length);
    if (config_json && out_length && *out_length > SH_CONFIG_MAX_FILE_BYTES) {
        free(config_json);
        config_json = NULL;
    }
done:
    sh_json_object_free(&ordered);
    free(settings_json);
    return config_json;
}

static int install_document_values_locked(const config_document *document)
{
    char *values[CONFIG_COUNT] = {0};
    size_t i;
    for (i = 0; i < CONFIG_COUNT; i++) {
        const char *value =
            sh_json_object_get(&document->settings, g_registry[i].key);
        if (!value) goto fail;
        values[i] = copy_string(value);
        if (!values[i]) goto fail;
    }
    clear_values_locked();
    for (i = 0; i < CONFIG_COUNT; i++) g_values[i] = values[i];
    return 1;
fail:
    for (i = 0; i < CONFIG_COUNT; i++) free(values[i]);
    return 0;
}

static wchar_t *make_backup_path_locked(void)
{
    SYSTEMTIME now;
    wchar_t generated[32];
    const wchar_t *timestamp;
    unsigned int collision;
#ifdef SH_CONFIG_TESTING
    timestamp = g_test_timestamp;
#else
    timestamp = NULL;
#endif
    if (!timestamp) {
        GetLocalTime(&now);
        _snwprintf_s(generated, sizeof(generated) / sizeof(generated[0]),
                     _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u",
                     (unsigned)now.wYear, (unsigned)now.wMonth,
                     (unsigned)now.wDay, (unsigned)now.wHour,
                     (unsigned)now.wMinute, (unsigned)now.wSecond);
        timestamp = generated;
    }
    for (collision = 0; collision < 10000u; collision++) {
        wchar_t name[128];
        wchar_t *path;
        if (collision == 0) {
            _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                         L"config.%s.corrupt.json", timestamp);
        } else {
            _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                         L"config.%s.%u.corrupt.json", timestamp, collision);
        }
        path = join_path(g_config_dir, name);
        if (!path) return NULL;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND)
            return path;
        free(path);
    }
    return NULL;
}

static int file_matches_bytes(const wchar_t *path, const char *bytes,
                              size_t length)
{
    HANDLE file;
    LARGE_INTEGER size;
    char buffer[4096];
    size_t compared = 0;
    int matches = 0;
    if (!path || !bytes) return 0;
    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (ULONGLONG)size.QuadPart != (ULONGLONG)length)
        goto done;
    while (compared < length) {
        DWORD want = (DWORD)((length - compared) < sizeof(buffer)
                                 ? (length - compared)
                                 : sizeof(buffer));
        DWORD got = 0;
        if (!ReadFile(file, buffer, want, &got, NULL) || got != want ||
            memcmp(buffer, bytes + compared, want) != 0)
            goto done;
        compared += got;
    }
    matches = 1;
done:
    CloseHandle(file);
    return matches;
}

static int recover_corrupt_locked(const char *replacement, size_t length)
{
    wchar_t *temp_path = NULL;
    wchar_t *backup_path = NULL;
    DWORD replace_error = ERROR_SUCCESS;
    int backup_exists;
    int ok = 0;
    if (!create_flushed_temp_locked(replacement, length, &temp_path)) return 0;
    backup_path = make_backup_path_locked();
    if (!backup_path || test_fault(CONFIG_FAIL_BACKUP) ||
        test_fault(CONFIG_FAIL_REPLACE))
        goto done;
    ok = ReplaceFileW(g_config_path, temp_path, backup_path, 0, NULL, NULL) != 0;
    if (ok && test_fault(CONFIG_FAIL_REPLACE_AFTER_COMMIT))
        ok = 0; /* test the documented partial-failure reconciliation path */
    if (!ok) {
        replace_error = GetLastError();
        backup_exists = GetFileAttributesW(backup_path) != INVALID_FILE_ATTRIBUTES;
        /* ReplaceFileW documents failure states where the names may already have moved. Reopen both
         * artifacts instead of assuming a false return means the original is still at config.json. */
        if (backup_exists &&
            file_matches_bytes(g_config_path, replacement, length)) {
            ok = 1;
        } else if (backup_exists &&
                   GetFileAttributesW(g_config_path) == INVALID_FILE_ATTRIBUTES &&
                   GetFileAttributesW(temp_path) != INVALID_FILE_ATTRIBUTES &&
                   MoveFileExW(temp_path, g_config_path,
                               MOVEFILE_REPLACE_EXISTING |
                               MOVEFILE_WRITE_THROUGH) &&
                   file_matches_bytes(g_config_path, replacement, length)) {
            ok = 1;
        } else if (replace_error == ERROR_UNABLE_TO_REMOVE_REPLACED ||
                   replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
                   replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            backend_log("config: atomic recovery stopped in a partial ReplaceFile state");
        }
    }
done:
    if (temp_path) DeleteFileW(temp_path);
    free(backup_path);
    free(temp_path);
    return ok;
}

static int init_missing_locked(void)
{
    config_document document;
    char *serialized = NULL;
    size_t length = 0;
    int ok = 0;
    if (!document_defaults(&document)) return 0;
    serialized = document_serialize(&document, &length);
    if (!serialized) goto done;
    if (!write_atomic_locked(serialized, length, 0, NULL, 0)) goto done;
    ok = install_document_values_locked(&document);
done:
    free(serialized);
    document_free(&document);
    return ok;
}

static int init_existing_locked(const char *bytes, size_t length)
{
    config_document document;
    config_parse_result parsed;
    char *serialized = NULL;
    size_t serialized_length = 0;
    int repaired = 0;
    int ok = 0;
    parsed = document_parse(bytes, length, &document, &repaired);
    if (parsed == CONFIG_PARSE_FUTURE) {
        document_free(&document);
        g_future_schema = 1;
        g_status_flags |= SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA;
        backend_log("config: newer schema left untouched; using session defaults");
        return 1;
    }
    if (parsed == CONFIG_PARSE_INVALID) {
        if (!document_defaults(&document)) return 0;
        serialized = document_serialize(&document, &serialized_length);
        if (serialized &&
            recover_corrupt_locked(serialized, serialized_length)) {
            g_status_flags |= SH_CONFIG_STATUS_RECOVERED_CORRUPT;
            ok = install_document_values_locked(&document);
            backend_log("config: corrupt file backed up; defaults restored");
        }
        free(serialized);
        document_free(&document);
        return ok;
    }

    if (!install_document_values_locked(&document)) goto done;
    ok = 1;
    if (repaired) {
        serialized = document_serialize(&document, &serialized_length);
        g_status_flags |= SH_CONFIG_STATUS_REPAIRED;
        if (!serialized ||
            !write_atomic_locked(serialized, serialized_length, 1,
                                 bytes, length))
            ok = 0;
        else
            backend_log("config: repaired missing or invalid registered settings");
    }
done:
    free(serialized);
    document_free(&document);
    return ok;
}

int sh_config_init(void)
{
    HANDLE mutex = NULL;
    char *bytes = NULL;
    size_t length = 0;
    config_read_result read_result;
    int persisted = 0;
    AcquireSRWLockExclusive(&g_config_lock);
    clear_state_locked();
    if (!validate_registry()) goto volatile_done;
    if (!load_defaults_locked()) goto volatile_done;
    if (!resolve_paths_locked() || !ensure_config_dir_locked()) goto volatile_done;
    mutex = acquire_process_mutex();
    if (!mutex) goto volatile_done;
    read_result = read_config_locked(&bytes, &length);
    if (read_result == CONFIG_READ_MISSING &&
        restore_interrupted_replace_locked())
        read_result = read_config_locked(&bytes, &length);
    if (read_result == CONFIG_READ_MISSING) {
        persisted = init_missing_locked();
    } else if (read_result == CONFIG_READ_OK) {
        persisted = init_existing_locked(bytes, length);
    } else if (read_result == CONFIG_READ_TOO_LARGE) {
        config_document document;
        char *serialized = NULL;
        size_t serialized_length = 0;
        if (document_defaults(&document)) {
            serialized = document_serialize(&document, &serialized_length);
            if (serialized &&
                recover_corrupt_locked(serialized, serialized_length)) {
                g_status_flags |= SH_CONFIG_STATUS_RECOVERED_CORRUPT;
                persisted = install_document_values_locked(&document);
            }
            free(serialized);
            document_free(&document);
        }
    }
    free(bytes);
    bytes = NULL;
    if (!persisted && !g_future_schema) goto volatile_done;
    g_initialized = 1;
    release_process_mutex(mutex);
    ReleaseSRWLockExclusive(&g_config_lock);
    return 1;

volatile_done:
    free(bytes);
    release_process_mutex(mutex);
    g_status_flags |= SH_CONFIG_STATUS_VOLATILE;
    g_initialized = 1;
    backend_log("config: using volatile settings (path, read, or write failure)");
    ReleaseSRWLockExclusive(&g_config_lock);
    return 0;
}

static int get_json_for_access(const char *key, unsigned int access,
                               char *out_json, int out_capacity,
                               unsigned int *out_flags)
{
    int index, length = -1;
    AcquireSRWLockShared(&g_config_lock);
    if (out_flags) *out_flags = g_status_flags;
    index = find_descriptor(key);
    if (!g_initialized || index < 0 ||
        !(g_registry[index].access & access) || !g_values[index])
        goto done;
    if (strlen(g_values[index]) > INT_MAX) goto done;
    length = (int)strlen(g_values[index]);
    if (out_json && out_capacity > length)
        memcpy(out_json, g_values[index], (size_t)length + 1);
done:
    ReleaseSRWLockShared(&g_config_lock);
    return length;
}

int sh_config_get_json(const char *key, char *out_json, int out_capacity,
                       unsigned int *out_flags)
{
    return get_json_for_access(key, SH_CONFIG_BACKEND_READ, out_json,
                               out_capacity, out_flags);
}

static int set_json_for_access(const char *key, const char *value_json,
                               unsigned int access)
{
    HANDLE mutex = NULL;
    config_document document;
    config_parse_result parsed = CONFIG_PARSE_INVALID;
    config_read_result read_result;
    char *bytes = NULL;
    char *serialized = NULL;
    size_t length = 0, serialized_length = 0;
    int destination_exists = 0;
    int repaired = 0;
    int index = find_descriptor(key);
    int result = SH_CONFIG_SET_REJECTED;
    int have_document = 0;
    const char *normalized;

    if (index < 0 || !(g_registry[index].access & access))
        return SH_CONFIG_SET_REJECTED;
    normalized = normalize_registered_value(&g_registry[index], value_json);
    if (!normalized) return SH_CONFIG_SET_REJECTED;

    AcquireSRWLockExclusive(&g_config_lock);
    if (!g_initialized) goto done;
    if (g_future_schema || !g_config_path || !g_config_dir) goto volatile_value;
    if (!ensure_config_dir_locked()) goto volatile_value;
    mutex = acquire_process_mutex();
    if (!mutex) goto volatile_value;

    read_result = read_config_locked(&bytes, &length);
    if (read_result == CONFIG_READ_MISSING) {
        if (!document_defaults(&document)) goto volatile_value;
        have_document = 1;
    } else if (read_result == CONFIG_READ_OK) {
        parsed = document_parse(bytes, length, &document, &repaired);
        if (parsed == CONFIG_PARSE_FUTURE) {
            document_free(&document);
            g_future_schema = 1;
            g_status_flags |= SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA;
            goto volatile_value;
        }
        if (parsed == CONFIG_PARSE_INVALID) {
            if (!document_defaults(&document)) goto volatile_value;
            have_document = 1;
        } else {
            have_document = 1;
            destination_exists = 1;
        }
    } else if (read_result == CONFIG_READ_TOO_LARGE) {
        if (!document_defaults(&document)) goto volatile_value;
        have_document = 1;
    } else {
        goto volatile_value;
    }

    if (!sh_json_object_set(&document.settings, key, normalized,
                            SH_CONFIG_JSON_DEPTH))
        goto volatile_value;
    serialized = document_serialize(&document, &serialized_length);
    if (!serialized) goto volatile_value;

    if (read_result == CONFIG_READ_OK && parsed == CONFIG_PARSE_INVALID) {
        if (!recover_corrupt_locked(serialized, serialized_length))
            goto volatile_value;
        g_status_flags |= SH_CONFIG_STATUS_RECOVERED_CORRUPT;
    } else if (read_result == CONFIG_READ_TOO_LARGE) {
        if (!recover_corrupt_locked(serialized, serialized_length))
            goto volatile_value;
        g_status_flags |= SH_CONFIG_STATUS_RECOVERED_CORRUPT;
    } else if (!write_atomic_locked(serialized, serialized_length,
                                    destination_exists,
                                    destination_exists ? bytes : NULL,
                                    destination_exists ? length : 0)) {
        goto volatile_value;
    }
    if (repaired) g_status_flags |= SH_CONFIG_STATUS_REPAIRED;
    if (!install_document_values_locked(&document)) goto volatile_value;
    result = SH_CONFIG_SET_PERSISTED;
    goto done;

volatile_value:
    if (replace_value_locked((size_t)index, normalized)) {
        g_status_flags |= SH_CONFIG_STATUS_VOLATILE;
        result = SH_CONFIG_SET_VOLATILE;
    }
done:
    free(bytes);
    free(serialized);
    if (have_document) document_free(&document);
    release_process_mutex(mutex);
    ReleaseSRWLockExclusive(&g_config_lock);
    return result;
}

int sh_config_set_json(const char *key, const char *value_json)
{
    return set_json_for_access(key, value_json, SH_CONFIG_BACKEND_WRITE);
}

int sh_config_get_string(const char *key, char *out, int out_capacity)
{
    int json_length, result = -1;
    char *json = NULL;
    size_t decoded_length = 0;
    json_length = sh_config_get_json(key, NULL, 0, NULL);
    if (json_length < 0) return -1;
    json = (char *)malloc((size_t)json_length + 1);
    if (!json) return -1;
    if (sh_config_get_json(key, json, json_length + 1, NULL) != json_length)
        goto done;
    if (!sh_json_decode_string(json, (size_t)json_length, out,
                               out_capacity > 0 ? (size_t)out_capacity : 0,
                               &decoded_length) ||
        decoded_length > INT_MAX)
        goto done;
    result = (int)decoded_length;
done:
    free(json);
    return result;
}

static int iface_config_get_json(sh_iface *self, const char *key,
                                 char *out_json, int out_capacity,
                                 unsigned int *out_flags)
{
    (void)self;
    return get_json_for_access(key, SH_CONFIG_UI_READ, out_json,
                               out_capacity, out_flags);
}

static int iface_config_set_json(sh_iface *self, const char *key,
                                 const char *value_json)
{
    (void)self;
    return set_json_for_access(key, value_json, SH_CONFIG_UI_WRITE);
}

void sh_config_bind_iface_slots(void)
{
    sh_iface_config_slots slots;
    slots.config_get_json = iface_config_get_json;
    slots.config_set_json = iface_config_set_json;
    sh_iface_bind_config_slots(&slots);
}

#ifdef SH_CONFIG_TESTING
void sh_config_test_reset(void)
{
    AcquireSRWLockExclusive(&g_config_lock);
    clear_state_locked();
    free(g_test_local_appdata);
    free(g_test_timestamp);
    g_test_local_appdata = NULL;
    g_test_timestamp = NULL;
    g_test_fault = SH_CONFIG_TEST_FAIL_NONE;
    g_test_hold_mutex_ms = 0;
    ReleaseSRWLockExclusive(&g_config_lock);
}

void sh_config_test_set_local_appdata(const wchar_t *path)
{
    wchar_t *copy = copy_wstring(path);
    AcquireSRWLockExclusive(&g_config_lock);
    free(g_test_local_appdata);
    g_test_local_appdata = copy;
    ReleaseSRWLockExclusive(&g_config_lock);
}

void sh_config_test_fail_next(sh_config_test_fault fault)
{
    AcquireSRWLockExclusive(&g_config_lock);
    g_test_fault = fault;
    ReleaseSRWLockExclusive(&g_config_lock);
}

void sh_config_test_set_timestamp(const wchar_t *timestamp)
{
    wchar_t *copy = copy_wstring(timestamp);
    AcquireSRWLockExclusive(&g_config_lock);
    free(g_test_timestamp);
    g_test_timestamp = copy;
    ReleaseSRWLockExclusive(&g_config_lock);
}

void sh_config_test_hold_mutex_ms(unsigned int milliseconds)
{
    AcquireSRWLockExclusive(&g_config_lock);
    g_test_hold_mutex_ms = milliseconds;
    ReleaseSRWLockExclusive(&g_config_lock);
}
#endif
