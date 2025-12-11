#pragma once

#include "types.h"
#include "std/string.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_INT,
    JSON_DOUBLE,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonKind;

struct JsonValue;

typedef struct {
    uint32_t count;
    struct JsonValue **items;
} JsonArray;

typedef struct {
    string key;
    struct JsonValue *value;
} JsonPair;

typedef struct {
    uint32_t count;
    JsonPair *pairs;
} JsonObject;

typedef struct JsonValue {
    JsonKind kind;
    union {
        bool boolean;
        int64_t integer;
        double real;
        string string;
        JsonArray array;
        JsonObject object;
    } u;
} JsonValue;

typedef enum {
    JSON_OK = 0,
    JSON_ERR_INVALID,
    JSON_ERR_OOM,
    JSON_ERR_TYPE
} JsonError;

JsonError json_parse(const char *buf, uint32_t len, JsonValue **out);
JsonError json_serialize(const JsonValue *value, string *out, uint32_t indent);
void json_free(JsonValue *v);

JsonValue *json_obj_get(const JsonValue *obj, const char *key);
static inline bool json_is_null(const JsonValue *v) { return v && v->kind == JSON_NULL; }
static inline bool json_is_bool(const JsonValue *v) { return v && v->kind == JSON_BOOL; }
static inline bool json_is_int(const JsonValue *v) { return v && v->kind == JSON_INT; }
static inline bool json_is_double(const JsonValue *v) { return v && v->kind == JSON_DOUBLE; }
static inline bool json_is_number(const JsonValue *v) { return v && (v->kind == JSON_INT || v->kind == JSON_DOUBLE); }
static inline bool json_is_string(const JsonValue *v) { return v && v->kind == JSON_STRING; }
static inline bool json_is_array(const JsonValue *v) { return v && v->kind == JSON_ARRAY; }
static inline bool json_is_object(const JsonValue *v) { return v && v->kind == JSON_OBJECT; }

bool json_get_bool(const JsonValue *v, bool *out);
bool json_get_int(const JsonValue *v, int64_t *out);
bool json_get_double(const JsonValue *v, double *out);
bool json_get_string(const JsonValue *v, string *out);
bool json_get_number_as_double(const JsonValue *v, double *out);

uint32_t json_array_size(const JsonValue *v);
JsonValue *json_array_get(const JsonValue *v, uint32_t index);

JsonValue *json_new_null();
JsonValue *json_new_bool(bool v);
JsonValue *json_new_int(int64_t v);
JsonValue *json_new_double(double v);
JsonValue *json_new_string(const char *data, uint32_t len);
JsonValue *json_new_array();
JsonValue *json_new_object();

bool json_array_push(JsonValue *arr, JsonValue *elem);
bool json_obj_set(JsonValue *obj, const char *key, JsonValue *value);

JsonValue *json_clone(const JsonValue *src);

#ifdef __cplusplus
}
#endif
