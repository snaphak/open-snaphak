#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "snapmap_plus_iface.h"

SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_get_json) == 0x2B0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_set_json) == 0x2B8);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x2C0);
SH_STATIC_ASSERT(offsetof(sh_iface, sub) == 0x58);
SH_STATIC_ASSERT(sizeof(sh_iface) == 0x60);

static int stub_get(sh_iface *self, const char *key, char *out_json,
                    int out_capacity, unsigned int *out_flags)
{
    static const char value[] = "\"dark\"";
    (void)self;
    (void)key;
    if (out_flags) *out_flags = SH_CONFIG_STATUS_REPAIRED;
    if (out_json && out_capacity >= (int)sizeof(value))
        memcpy(out_json, value, sizeof(value));
    return (int)sizeof(value) - 1;
}

static int stub_set(sh_iface *self, const char *key, const char *value_json)
{
    (void)self;
    return key && value_json ? SH_CONFIG_SET_PERSISTED : SH_CONFIG_SET_REJECTED;
}

int main(void)
{
    sh_iface_config_slots slots;
    sh_iface_engine_slots engine_slots;
    sh_iface *iface;
    char value[16] = {0};
    unsigned int flags = 0;

    memset(&slots, 0, sizeof(slots));
    slots.config_get_json = stub_get;
    slots.config_set_json = stub_set;

    iface = sh_iface_create();
    if (!iface || !iface->vtbl) {
        fprintf(stderr, "iface_config_test: interface creation failed\n");
        return 1;
    }
    sh_iface_bind_config_slots(&slots);
    memset(&engine_slots, 0, sizeof(engine_slots));
    sh_iface_bind_engine_slots(&engine_slots);
    if (iface->vtbl->config_get_json != stub_get ||
        iface->vtbl->config_set_json != stub_set) {
        fprintf(stderr, "iface_config_test: config callbacks were not preserved\n");
        return 1;
    }
    if (iface->vtbl->config_get_json(iface, "theme", value,
                                     (int)sizeof(value), &flags) != 6 ||
        strcmp(value, "\"dark\"") != 0 ||
        flags != SH_CONFIG_STATUS_REPAIRED) {
        fprintf(stderr, "iface_config_test: getter callback contract failed\n");
        return 1;
    }
    if (iface->vtbl->config_set_json(iface, "theme", "\"light\"") !=
        SH_CONFIG_SET_PERSISTED) {
        fprintf(stderr, "iface_config_test: setter callback contract failed\n");
        return 1;
    }

    puts("iface_config_test: OK");
    return 0;
}
