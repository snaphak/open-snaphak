#include "theme_bootstrap.h"

#include <cstring>

bool sh_theme_json_is_dark(const char *value_json)
{
    return value_json && std::strcmp(value_json, "\"dark\"") == 0;
}

bool sh_theme_seed_html(std::wstring &html, const char *value_json)
{
    static const std::wstring marker = L"<html lang=\"en\">";
    static const std::wstring dark_marker =
        L"<html class=\"dark\" lang=\"en\">";
    const std::wstring::size_type first = html.find(marker);
    if (first == std::wstring::npos ||
        html.find(marker, first + marker.size()) != std::wstring::npos)
        return false;
    if (sh_theme_json_is_dark(value_json))
        html.replace(first, marker.size(), dark_marker);
    return true;
}
