#ifndef SH_THEME_BOOTSTRAP_H
#define SH_THEME_BOOTSTRAP_H

#include <string>

bool sh_theme_json_is_dark(const char *value_json);
bool sh_theme_seed_html(std::wstring &html, const char *value_json);

#endif /* SH_THEME_BOOTSTRAP_H */
