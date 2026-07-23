#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                            \
                __FILE__, __LINE__, #expr);                                     \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static char *read_all(const char *path)
{
    FILE *file = NULL;
    long length;
    char *bytes;
    if (fopen_s(&file, path, "rb") != 0 || !file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1u);
    if (!bytes) { fclose(file); return NULL; }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[length] = 0;
    return bytes;
}

static unsigned int count_text(const char *text, const char *needle)
{
    unsigned int count = 0;
    size_t length = strlen(needle);
    const char *at = text;
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += length;
    }
    return count;
}

int main(int argc, char **argv)
{
    char *html;
    const char *preview_read;
    const char *preview_write;
    const char *read_storage;
    const char *write_storage;
    if (argc != 2) {
        fprintf(stderr, "usage: theme_contract_test <mockup.html>\n");
        return 2;
    }
    html = read_all(argv[1]);
    CHECK(html != NULL);
    if (!html) return 1;

    CHECK(strstr(html, "<html lang=\"en\">") != NULL);
    CHECK(strstr(html, "function applyTheme(dark, persist)") != NULL);
    CHECK(strstr(html, "function configGet(key)") != NULL);
    CHECK(strstr(html, "function configSet(key, value)") != NULL);
    CHECK(strstr(html, "valueJson:JSON.stringify(value)") != NULL);
    CHECK(strstr(html, "applyTheme(false, true)") != NULL);
    CHECK(strstr(html, "applyTheme(true, true)") != NULL);
    CHECK(strstr(html, "function setTheme(") == NULL);

    CHECK(strstr(html, "d.kind === 'configValue'") != NULL);
    CHECK(strstr(html, "d.kind === 'configSetResult'") != NULL);
    CHECK(strstr(html, "d.kind === 'configStatus'") != NULL);
    CHECK(strstr(html, "SH_CONFIG_STATUS") == NULL);

    preview_read = strstr(html, "function previewThemeRead() {\n    if (!PREVIEW)");
    preview_write = strstr(html, "function previewThemeWrite(value) {\n    if (!PREVIEW)");
    read_storage = strstr(html, "localStorage.getItem('sh_theme')");
    write_storage = strstr(html, "localStorage.setItem('sh_theme', value)");
    CHECK(preview_read != NULL);
    CHECK(preview_write != NULL);
    CHECK(read_storage != NULL);
    CHECK(write_storage != NULL);
    if (preview_read && preview_write && read_storage && write_storage) {
        CHECK(preview_read < read_storage);
        CHECK(read_storage < preview_write);
        CHECK(preview_write < write_storage);
    }
    CHECK(count_text(html, "localStorage") == 2);

    free(html);
    if (g_failed) {
        fprintf(stderr, "theme_contract_test: %d failure(s)\n", g_failed);
        return 1;
    }
    puts("theme_contract_test: OK");
    return 0;
}
