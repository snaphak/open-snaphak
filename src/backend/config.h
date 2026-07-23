/* config.h -- backend-owned persistent Snapmap+ settings service.
 *
 * Runtime preferences live in %LOCALAPPDATA%\snapmap-plus\config.json. The backend is the sole file
 * owner; callers use registered keys, and the frontend reaches the same service through the append-only
 * shared interface slots.
 */
#ifndef SH_CONFIG_H
#define SH_CONFIG_H

#include <wchar.h>

#include "snapmap_plus_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_CONFIG_SCHEMA_VERSION 1u
#define SH_CONFIG_MAX_FILE_BYTES (64u * 1024u)

#define SH_CONFIG_BACKEND_READ  0x01u
#define SH_CONFIG_BACKEND_WRITE 0x02u
#define SH_CONFIG_UI_READ       0x04u
#define SH_CONFIG_UI_WRITE      0x08u

int sh_config_init(void);
int sh_config_get_json(const char *key, char *out_json, int out_capacity,
                       unsigned int *out_flags);
int sh_config_set_json(const char *key, const char *value_json);
int sh_config_get_string(const char *key, char *out, int out_capacity);
void sh_config_bind_iface_slots(void);

#ifdef SH_CONFIG_TESTING
typedef enum sh_config_test_fault {
    SH_CONFIG_TEST_FAIL_NONE,
    SH_CONFIG_TEST_FAIL_RESOLVE,
    SH_CONFIG_TEST_FAIL_READ,
    SH_CONFIG_TEST_FAIL_CREATE_TEMP,
    SH_CONFIG_TEST_FAIL_WRITE,
    SH_CONFIG_TEST_FAIL_FLUSH,
    SH_CONFIG_TEST_FAIL_REPLACE,
    SH_CONFIG_TEST_FAIL_BACKUP,
    SH_CONFIG_TEST_FAIL_REPLACE_AFTER_COMMIT,
    SH_CONFIG_TEST_FAIL_REPLACE_KEEP_NAMES,
    SH_CONFIG_TEST_FAIL_REPLACE_MOVED_OLD
} sh_config_test_fault;

void sh_config_test_reset(void);
void sh_config_test_set_local_appdata(const wchar_t *path);
void sh_config_test_fail_next(sh_config_test_fault fault);
void sh_config_test_set_timestamp(const wchar_t *timestamp);
void sh_config_test_hold_mutex_ms(unsigned int milliseconds);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SH_CONFIG_H */
