#include "config_message.h"

namespace {

enum string_field_result {
    STRING_FIELD_MISSING = 0,
    STRING_FIELD_OK,
    STRING_FIELD_INVALID
};

static void skip_space(const wchar_t *&p)
{
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
        p++;
}

static int hex_value(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'a' && c <= L'f') return (int)(c - L'a') + 10;
    if (c >= L'A' && c <= L'F') return (int)(c - L'A') + 10;
    return -1;
}

static bool read_string_code_unit(const wchar_t *&p, wchar_t &decoded)
{
    wchar_t c = *p++;
    if (c != L'\\') {
        if (c < 0x20) return false;
        decoded = c;
        return true;
    }

    wchar_t escaped = *p++;
    switch (escaped) {
    case L'"':  decoded = L'"';  return true;
    case L'\\': decoded = L'\\'; return true;
    case L'/':  decoded = L'/';  return true;
    case L'b':  decoded = L'\b'; return true;
    case L'f':  decoded = L'\f'; return true;
    case L'n':  decoded = L'\n'; return true;
    case L'r':  decoded = L'\r'; return true;
    case L't':  decoded = L'\t'; return true;
    case L'u': {
        unsigned int value = 0;
        for (int i = 0; i < 4; i++) {
            int digit = hex_value(*p++);
            if (digit < 0) return false;
            value = (value << 4) | (unsigned int)digit;
        }
        decoded = (wchar_t)value;
        return true;
    }
    default:
        return false;
    }
}

static bool read_property_name(const wchar_t *&p, const wchar_t *wanted,
                               bool &matches)
{
    if (*p++ != L'"') return false;
    std::size_t index = 0;
    matches = true;
    while (*p && *p != L'"') {
        wchar_t decoded = 0;
        if (!read_string_code_unit(p, decoded)) return false;
        if (matches && (!wanted[index] || wanted[index] != decoded))
            matches = false;
        index++;
    }
    if (*p++ != L'"') return false;
    if (matches && wanted[index]) matches = false;
    return true;
}

static bool skip_string(const wchar_t *&p)
{
    if (*p++ != L'"') return false;
    while (*p && *p != L'"') {
        wchar_t ignored = 0;
        if (!read_string_code_unit(p, ignored)) return false;
    }
    if (*p++ != L'"') return false;
    return true;
}

static bool skip_value(const wchar_t *&p)
{
    skip_space(p);
    if (*p == L'"') return skip_string(p);

    if (*p == L'{' || *p == L'[') {
        int depth = 0;
        do {
            if (*p == L'"') {
                if (!skip_string(p)) return false;
                continue;
            }
            if (*p == L'{' || *p == L'[') depth++;
            else if (*p == L'}' || *p == L']') depth--;
            if (!*p) return false;
            p++;
        } while (depth > 0);
        return depth == 0;
    }

    const wchar_t *start = p;
    while (*p && *p != L',' && *p != L'}') p++;
    return p != start;
}

static bool read_bounded_string(const wchar_t *&p, std::size_t cap,
                                std::wstring &out)
{
    out.clear();
    if (*p++ != L'"') return false;
    while (*p && *p != L'"') {
        wchar_t decoded = 0;
        if (!read_string_code_unit(p, decoded)) {
            out.clear();
            return false;
        }
        if (out.size() == cap) {
            out.clear();
            return false;
        }
        out.push_back(decoded);
    }
    if (*p++ != L'"') {
        out.clear();
        return false;
    }
    return true;
}

static string_field_result find_top_level_string(
    const wchar_t *json, const wchar_t *field, std::size_t cap,
    std::wstring &out)
{
    out.clear();
    if (!json || !field) return STRING_FIELD_INVALID;

    const wchar_t *p = json;
    skip_space(p);
    if (*p++ != L'{') return STRING_FIELD_INVALID;

    for (;;) {
        skip_space(p);
        if (*p == L'}') return STRING_FIELD_MISSING;

        bool matches = false;
        if (!read_property_name(p, field, matches))
            return STRING_FIELD_INVALID;
        skip_space(p);
        if (*p++ != L':') return STRING_FIELD_INVALID;
        skip_space(p);

        if (matches)
            return read_bounded_string(p, cap, out)
                       ? STRING_FIELD_OK
                       : STRING_FIELD_INVALID;
        if (!skip_value(p)) return STRING_FIELD_INVALID;

        skip_space(p);
        if (*p == L',') {
            p++;
            continue;
        }
        if (*p == L'}') return STRING_FIELD_MISSING;
        return STRING_FIELD_INVALID;
    }
}

} // namespace

sh_config_message sh_extract_config_message(const wchar_t *json,
                                            std::size_t key_code_unit_cap,
                                            std::size_t value_code_unit_cap)
{
    sh_config_message message;
    std::wstring command;
    if (find_top_level_string(json, L"cmd", 9, command) != STRING_FIELD_OK)
        return message;
    if (command == L"configGet")
        message.kind = SH_CONFIG_MESSAGE_GET;
    else if (command == L"configSet")
        message.kind = SH_CONFIG_MESSAGE_SET;
    else
        return message;

    if (find_top_level_string(json, L"key", key_code_unit_cap,
                              message.key) != STRING_FIELD_OK)
        return message;
    if (message.kind == SH_CONFIG_MESSAGE_SET &&
        find_top_level_string(json, L"valueJson", value_code_unit_cap,
                              message.value_json) != STRING_FIELD_OK)
        return message;

    message.fields_valid = true;
    return message;
}
