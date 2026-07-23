#pragma once

#include <cstddef>
#include <string>

enum sh_config_message_kind {
    SH_CONFIG_MESSAGE_OTHER = 0,
    SH_CONFIG_MESSAGE_GET,
    SH_CONFIG_MESSAGE_SET
};

struct sh_config_message {
    sh_config_message_kind kind = SH_CONFIG_MESSAGE_OTHER;
    bool fields_valid = false;
    std::wstring key;
    std::wstring value_json;
};

sh_config_message sh_extract_config_message(const wchar_t *json,
                                            std::size_t key_code_unit_cap,
                                            std::size_t value_code_unit_cap);
