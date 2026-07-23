/* config_json.h -- bounded JSON validation + raw-value object model for config.json.
 *
 * This is intentionally small and purpose-built. It validates complete JSON values, decodes object
 * keys, rejects duplicate keys at every nesting level, and preserves member values as validated raw
 * JSON fragments. The config service can therefore update registered settings without losing unknown
 * future values or normalizing engine-sensitive number spellings.
 */
#ifndef SH_CONFIG_JSON_H
#define SH_CONFIG_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sh_json_kind {
    SH_JSON_NULL,
    SH_JSON_BOOL,
    SH_JSON_NUMBER,
    SH_JSON_STRING,
    SH_JSON_ARRAY,
    SH_JSON_OBJECT
} sh_json_kind;

typedef struct sh_json_member {
    char *key;
    size_t key_length;
    char *value_json;
} sh_json_member;

typedef struct sh_json_object {
    sh_json_member *members;
    size_t count;
    size_t capacity;
} sh_json_object;

int sh_json_validate(const char *json, size_t length, unsigned max_depth,
                     sh_json_kind *out_kind);
int sh_json_parse_object(const char *json, size_t length, unsigned max_depth,
                         sh_json_object *out);
const char *sh_json_object_get(const sh_json_object *object, const char *key);
int sh_json_object_set(sh_json_object *object, const char *key,
                       const char *value_json, unsigned max_depth);
int sh_json_object_set_n(sh_json_object *object, const char *key,
                         size_t key_length, const char *value_json,
                         unsigned max_depth);
int sh_json_decode_string(const char *json, size_t length,
                          char *out, size_t out_capacity, size_t *out_length);
char *sh_json_serialize_object(const sh_json_object *object,
                               unsigned base_indent, size_t *out_length);
void sh_json_object_free(sh_json_object *object);

#ifdef __cplusplus
}
#endif

#endif /* SH_CONFIG_JSON_H */
