#include "json.h"
#include "std/std.h"
#include "syscalls/syscalls.h"

JsonError parse_value(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out);

static void json_skip_whitespace(const char *buf, uint32_t len, uint32_t *pos) {
    while (*pos < len) {
        char c = buf[*pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') (*pos)++;
        else break;
    }
}

JsonError parse_string(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out) {
    if (!(*pos < len && buf[*pos] == '"')) return JSON_ERR_INVALID;
    (*pos)++;
    string s = string_repeat('\0', 0);

    while (*pos < len) {
        char c = buf[(*pos)++];
        if (c == '"') {
            JsonValue *v = malloc(sizeof(JsonValue));
            if (!v) {
                free_sized(s.data, s.mem_length);
                return JSON_ERR_OOM;
            }
            v->kind = JSON_STRING;
            v->u.string = s;
            *out = v;
            return JSON_OK;
        }
        if (c == '\\') {
            if (*pos >= len) {
                free_sized(s.data, s.mem_length);
                return JSON_ERR_INVALID;
            }
            char e = buf[(*pos)++];
            char r = e;
            if (e == 'b') r = '\b';
            else if (e == 'f') r = '\f';
            else if (e == 'n') r = '\n';
            else if (e == 'r') r = '\r';
            else if (e == 't') r = '\t';
            else if (!(e == '"' || e == '\\' || e == '/')) {
                free_sized(s.data, s.mem_length);
                return JSON_ERR_INVALID;
            }
            string_append_bytes(&s, &r, 1);
            continue;
        }
        string_append_bytes(&s, &c, 1);
    }

    free_sized(s.data, s.mem_length);
    return JSON_ERR_INVALID;
}

JsonError parse_number(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out) {
    uint32_t start = *pos;
    bool neg = false;

    if (*pos < len && buf[*pos] == '-') {
        neg = true;
        (*pos)++;
    }

    if (!(*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9')) return JSON_ERR_INVALID;
    while (*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9') (*pos)++;

    bool has_frac = false;
    uint32_t frac_start = 0;

    if (*pos < len && buf[*pos] == '.') {
        has_frac = true;
        (*pos)++;
        if (!(*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9')) return JSON_ERR_INVALID;
        frac_start = *pos;
        while (*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9') (*pos)++;
    }

    bool has_exp = false;
    bool exp_neg = false;
    int exp_val = 0;

    if (*pos < len && (buf[*pos] == 'e' || buf[*pos] == 'E')) {
        has_exp = true;
        (*pos)++;
        if (*pos < len && (buf[*pos] == '+' || buf[*pos] == '-')) {
            if (buf[*pos] == '-') exp_neg = true;
            (*pos)++;
        }
        if (!(*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9')) return JSON_ERR_INVALID;
        while (*pos < len && buf[*pos] >= '0' && buf[*pos] <= '9') {
            exp_val = exp_val * 10 + (buf[*pos] - '0');
            (*pos)++;
        }
        if (exp_neg) exp_val = -exp_val;
    }

    uint32_t end = *pos;
    if (end <= start) return JSON_ERR_INVALID;

    if (!has_frac && !has_exp) {
        int64_t x = 0;
        uint32_t i = start + (neg ? 1u : 0u);
        for (; i < end; i++) x = x * 10 + (buf[i] - '0');
        if (neg) x = -x;

        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_INT;
        v->u.integer = x;
        *out = v;
        return JSON_OK;
    }

    double ip = 0.0;
    uint32_t i = start + (neg ? 1u : 0u);
    for (; i < end; i++) {
        char c = buf[i];
        if (c == '.' || c == 'e' || c == 'E') break;
        ip = ip * 10.0 + (double)(c - '0');
    }

    double fp = 0.0;
    if (has_frac) {
        double base = 0.1;
        for (i = frac_start; i < end; i++) {
            char c = buf[i];
            if (c == 'e' || c == 'E') break;
            fp += (double)(c - '0') * base;
            base *= 0.1;
        }
    }

    double val = ip + fp;
    if (neg) val = -val;

    if (has_exp) {
        double y = 1.0;
        if (exp_val > 0) while (exp_val--) y *= 10.0;
        else while (exp_val++) y /= 10.0;
        val *= y;
    }

    JsonValue *v = malloc(sizeof(JsonValue));
    if (!v) return JSON_ERR_OOM;
    v->kind = JSON_DOUBLE;
    v->u.real = val;
    *out = v;
    return JSON_OK;
}

JsonError parse_array(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out) {
    if (!(*pos < len && buf[*pos] == '[')) return JSON_ERR_INVALID;
    (*pos)++;
    json_skip_whitespace(buf, len, pos);

    JsonValue *arr = malloc(sizeof(JsonValue));
    if (!arr) return JSON_ERR_OOM;
    arr->kind = JSON_ARRAY;
    arr->u.array.items = 0;
    arr->u.array.count = 0;

    if (*pos < len && buf[*pos] == ']') {
        (*pos)++;
        *out = arr;
        return JSON_OK;
    }

    for (;;) {
        JsonValue *elem = 0;
        JsonError e = parse_value(buf, len, pos, &elem);
        if (e != JSON_OK) {
            json_free(arr);
            return e;
        }

        uint32_t n = arr->u.array.count;
        JsonValue **tmp = malloc((n + 1) * sizeof(JsonValue *));
        if (!tmp) {
            json_free(elem);
            json_free(arr);
            return JSON_ERR_OOM;
        }

        for (uint32_t i = 0; i < n; i++) tmp[i] = arr->u.array.items[i];
        tmp[n] = elem;

        if (arr->u.array.items) free_sized(arr->u.array.items, n * sizeof(JsonValue *));
        arr->u.array.items = tmp;
        arr->u.array.count = n + 1;

        json_skip_whitespace(buf, len, pos);

        if (*pos < len && buf[*pos] == ']') {
            (*pos)++;
            break;
        }

        if (!(*pos < len && buf[*pos] == ',')) {
            json_free(arr);
            return JSON_ERR_INVALID;
        }

        (*pos)++;
        json_skip_whitespace(buf, len, pos);
    }

    *out = arr;
    return JSON_OK;
}

JsonError parse_object(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out) {
    if (!(*pos < len && buf[*pos] == '{')) return JSON_ERR_INVALID;
    (*pos)++;
    json_skip_whitespace(buf, len, pos);

    JsonValue *obj = malloc(sizeof(JsonValue));
    if (!obj) return JSON_ERR_OOM;
    obj->kind = JSON_OBJECT;
    obj->u.object.pairs = 0;
    obj->u.object.count = 0;

    if (*pos < len && buf[*pos] == '}') {
        (*pos)++;
        *out = obj;
        return JSON_OK;
    }

    for (;;) {
        JsonValue *ks = 0;
        JsonError e = parse_string(buf, len, pos, &ks);
        if (e != JSON_OK) {
            if (ks) json_free(ks);
            json_free(obj);
            return JSON_ERR_INVALID;
        }

        string key = ks->u.string;
        free_sized(ks, sizeof(JsonValue));

        json_skip_whitespace(buf, len, pos);

        if (!(*pos < len && buf[*pos] == ':')) {
            free_sized(key.data, key.mem_length);
            json_free(obj);
            return JSON_ERR_INVALID;
        }

        (*pos)++;
        json_skip_whitespace(buf, len, pos);

        JsonValue *val = 0;
        e = parse_value(buf, len, pos, &val);
        if (e != JSON_OK) {
            free_sized(key.data, key.mem_length);
            json_free(obj);
            return e;
        }

        uint32_t n = obj->u.object.count;
        JsonPair *tmp = malloc((n + 1) * sizeof(JsonPair));
        if (!tmp) {
            free_sized(key.data, key.mem_length);
            json_free(val);
            json_free(obj);
            return JSON_ERR_OOM;
        }

        for (uint32_t i = 0; i < n; i++) tmp[i] = obj->u.object.pairs[i];
        tmp[n].key = key;
        tmp[n].value = val;

        if (obj->u.object.pairs) free_sized(obj->u.object.pairs, n * sizeof(JsonPair));
        obj->u.object.pairs = tmp;
        obj->u.object.count = n + 1;

        json_skip_whitespace(buf, len, pos);

        if (*pos < len && buf[*pos] == '}') {
            (*pos)++;
            break;
        }

        if (!(*pos < len && buf[*pos] == ',')) {
            json_free(obj);
            return JSON_ERR_INVALID;
        }

        (*pos)++;
        json_skip_whitespace(buf, len, pos);
    }

    *out = obj;
    return JSON_OK;
}

JsonError parse_value(const char *buf, uint32_t len, uint32_t *pos, JsonValue **out) {
    json_skip_whitespace(buf, len, pos);
    if (*pos >= len) return JSON_ERR_INVALID;

    char c = buf[*pos];

    if (c == '"') return parse_string(buf, len, pos, out);
    if (c == '{') return parse_object(buf, len, pos, out);
    if (c == '[') return parse_array(buf, len, pos, out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(buf, len, pos, out);

    if (c == 't' && *pos + 4 <= len &&
        buf[*pos] == 't' && buf[*pos+1] == 'r' && buf[*pos+2] == 'u' && buf[*pos+3] == 'e') {
        *pos += 4;
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_BOOL;
        v->u.boolean = true;
        *out = v;
        return JSON_OK;
    }

    if (c == 'f' && *pos + 5 <= len &&
        buf[*pos] == 'f' && buf[*pos+1] == 'a' && buf[*pos+2] == 'l' && buf[*pos+3] == 's' && buf[*pos+4] == 'e') {
        *pos += 5;
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_BOOL;
        v->u.boolean = false;
        *out = v;
        return JSON_OK;
    }

    if (c == 'n' && *pos + 4 <= len &&
        buf[*pos] == 'n' && buf[*pos+1] == 'u' && buf[*pos+2] == 'l' && buf[*pos+3] == 'l') {
        *pos += 4;
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_NULL;
        *out = v;
        return JSON_OK;
    }

    return JSON_ERR_INVALID;
}

JsonError json_parse(const char *buf, uint32_t len, JsonValue **out) {
    uint32_t pos = 0;
    JsonError e = parse_value(buf, len, &pos, out);
    if (e != JSON_OK) return e;
    json_skip_whitespace(buf, len, &pos);
    if (pos != len) {
        json_free(*out);
        return JSON_ERR_INVALID;
    }
    return JSON_OK;
}

void json_free(JsonValue *v) {
    if (!v) return;

    if (v->kind == JSON_STRING) {
        free_sized(v->u.string.data, v->u.string.mem_length);
    }

    else if (v->kind == JSON_ARRAY) {
        for (uint32_t i = 0; i < v->u.array.count; i++) json_free(v->u.array.items[i]);
        if (v->u.array.items)
            free_sized(v->u.array.items, v->u.array.count * sizeof(JsonValue *));
    }

    else if (v->kind == JSON_OBJECT) {
        for (uint32_t i = 0; i < v->u.object.count; i++) {
            free_sized(v->u.object.pairs[i].key.data, v->u.object.pairs[i].key.mem_length);
            json_free(v->u.object.pairs[i].value);
        }
        if (v->u.object.pairs)
            free_sized(v->u.object.pairs, v->u.object.count * sizeof(JsonPair));
    }

    free_sized(v, sizeof(JsonValue));
}

bool json_get_bool(const JsonValue *v, bool *out) {
    if (!v || v->kind != JSON_BOOL) return false;
    *out = v->u.boolean;
    return true;
}

bool json_get_int(const JsonValue *v, int64_t *out) {
    if (!v || v->kind != JSON_INT) return false;
    *out = v->u.integer;
    return true;
}

bool json_get_double(const JsonValue *v, double *out) {
    if (!v || v->kind != JSON_DOUBLE) return false;
    *out = v->u.real;
    return true;
}

bool json_get_string(const JsonValue *v, string *out) {
    if (!v || v->kind != JSON_STRING) return false;
    *out = v->u.string;
    return true;
}

bool json_get_number_as_double(const JsonValue *v, double *out) {
    if (!v) return false;
    if (v->kind == JSON_DOUBLE) {
        *out = v->u.real;
        return true;
    }
    if (v->kind == JSON_INT) {
        *out = (double)v->u.integer;
        return true;
    }
    return false;
}

uint32_t json_array_size(const JsonValue *v) {
    if (!v || v->kind != JSON_ARRAY) return 0;
    return v->u.array.count;
}

JsonValue *json_array_get(const JsonValue *v, uint32_t index) {
    if (!v || v->kind != JSON_ARRAY) return 0;
    if (index >= v->u.array.count) return 0;
    return v->u.array.items[index];
}

JsonValue *json_obj_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT) return 0;
    for (uint32_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.pairs[i].key.data, key) == 0)
            return obj->u.object.pairs[i].value;
    }
    return 0;
}

bool json_obj_get_bool(const JsonValue *obj, const char *key, bool *out) {
    return json_get_bool(json_obj_get(obj, key), out);
}

bool json_obj_get_int(const JsonValue *obj, const char *key, int64_t *out) {
    return json_get_int(json_obj_get(obj, key), out);
}

bool json_obj_get_double(const JsonValue *obj, const char *key, double *out) {
    return json_get_double(json_obj_get(obj, key), out);
}

bool json_obj_get_string(const JsonValue *obj, const char *key, string *out) {
    return json_get_string(json_obj_get(obj, key), out);
}

JsonValue *json_new_null() {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_NULL;
    return x;
}

JsonValue *json_new_bool(bool v) {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_BOOL;
    x->u.boolean = v;
    return x;
}

JsonValue *json_new_int(int64_t v) {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_INT;
    x->u.integer = v;
    return x;
}

JsonValue *json_new_double(double v) {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_DOUBLE;
    x->u.real = v;
    return x;
}

JsonValue *json_new_string(const char *data, uint32_t len) {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_STRING;
    x->u.string = string_from_literal_length((char *)data, len);
    return x;
}

JsonValue *json_new_array() {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_ARRAY;
    x->u.array.items = 0;
    x->u.array.count = 0;
    return x;
}

JsonValue *json_new_object() {
    JsonValue *x = malloc(sizeof(JsonValue));
    if (!x) return 0;
    x->kind = JSON_OBJECT;
    x->u.object.pairs = 0;
    x->u.object.count = 0;
    return x;
}

bool json_array_push(JsonValue *arr, JsonValue *elem) {
    if (!arr || arr->kind != JSON_ARRAY) return false;
    uint32_t n = arr->u.array.count;
    JsonValue **tmp = malloc((n + 1) * sizeof(JsonValue *));
    if (!tmp) return false;
    for (uint32_t i = 0; i < n; i++) tmp[i] = arr->u.array.items[i];
    tmp[n] = elem;
    if (arr->u.array.items) free_sized(arr->u.array.items, n * sizeof(JsonValue *));
    arr->u.array.items = tmp;
    arr-> u.array.count = n + 1;
    return true;
}

bool json_obj_set(JsonValue *obj, const char *key, JsonValue *value) {
    if (!obj || obj->kind != JSON_OBJECT) return false;

    uint32_t klen = strlen(key);

    for (uint32_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.pairs[i].key.data, key) == 0) {
            free_sized(obj->u.object.pairs[i].key.data, obj->u.object.pairs[i].key.mem_length);
            obj->u.object.pairs[i].key = string_from_literal_length((char *)key, klen);
            json_free(obj->u.object.pairs[i].value);
            obj->u.object.pairs[i ].value = value;
            return true;
        }
    }

    string sk = string_from_literal_length((char *)key, klen);
    uint32_t n = obj->u.object.count;

    JsonPair *tmp = malloc((n + 1) * sizeof(JsonPair));
    if (!tmp) {
        free_sized(sk.data, sk.mem_length);
        return false;
    }

    for (uint32_t i = 0; i < n; i++) tmp[i] = obj->u.object.pairs[i];
    tmp[n].key = sk;
    tmp[n].value = value;

    if (obj-> u.object.pairs) free_sized(obj->u.object.pairs, n * sizeof(JsonPair));
    obj->u.object.pairs = tmp;
    obj->u.object.count = n + 1;

    return true;
}

JsonValue *json_clone(const JsonValue *src) {
    if (!src) return 0;

    if (src->kind == JSON_NULL) return json_new_null();
    if (src->kind == JSON_BOOL) return json_new_bool(src->u.boolean);
    if (src->kind == JSON_INT) return json_new_int(src->u.integer);
    if (src->kind == JSON_DOUBLE) return json_new_double(src->u.real);
    if (src->kind == JSON_STRING) return json_new_string(src->u.string.data, src->u.string.length);

    if (src->kind == JSON_ARRAY) {
        JsonValue *a = json_new_array();
        if (!a) return 0;
        for (uint32_t i = 0; i < src->u.array.count; i++) {
            JsonValue *c = json_clone(src->u.array.items[i]);
            if (!c) {
                json_free(a);
                return 0;
            }
            json_array_push(a, c);
        }
        return a;
    }

    if (src->kind == JSON_OBJECT) {
        JsonValue *o = json_new_object();
        if (!o) return 0;
        for (uint32_t i = 0; i < src->u.object.count; i++) {
            JsonPair *p = &src->u.object.pairs[i];
            JsonValue *c = json_clone(p->value);
            if (!c) {
                json_free(o);
                return 0;
            }
            json_obj_set(o, p->key.data, c);
        }
        return o;
    }

    return 0;
}

void serialize_string(const string *s, string *out) {
    string_append_bytes(out, "\"", 1);
    for (uint32_t i = 0; i < s->length; i++) {
        char c = s->data[i];
        if (c == '"' || c == '\\') {
            char b[2] = {'\\', c};
            string_append_bytes(out, b, 2);
        } else if (c == '\b') string_append_bytes(out, "\\b", 2);
        else if (c == '\f') string_append_bytes(out, "\\f", 2);
        else if (c == '\n') string_append_bytes(out, "\\n", 2);
        else if (c == '\r') string_append_bytes(out, "\\r", 2);
        else if (c == '\t') string_append_bytes(out, "\\t", 2);
        else string_append_bytes(out, &c, 1);
    }
    string_append_bytes(out, "\"", 1);
}

void serialize_value(const JsonValue *v, string *out, uint32_t indent, uint32_t level);

void serialize_array(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
    string_append_bytes(out, "[", 1);

    uint32_t n = v->u.array.count;
    if (n == 0) {
        string_append_bytes(out, "]", 1);
        return;
    }

    if (indent) string_append_bytes(out, "\n", 1);

    for (uint32_t i = 0; i < n; i++) {
        if (indent) {
            for (uint32_t k = 0; k < (level + 1) * indent; k++) string_append_bytes(out, " ", 1);
        }

        serialize_value(v->u.array.items[i], out, indent, level + 1);

        if (i + 1 < n) string_append_bytes(out, ",", 1);
        if (indent) string_append_bytes(out, "\n", 1);
    }

    if (indent) {
        for (uint32_t k = 0; k < level * indent; k++) string_append_bytes(out, " ", 1);
    }

    string_append_bytes(out, "]", 1);
}

void serialize_object(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
    string_append_bytes(out, "{", 1);

    uint32_t n = v->u.object.count;
    if (n == 0) {
        string_append_bytes(out, "}", 1);
        return;
    }

    if (indent) string_append_bytes(out, "\n", 1);

    for (uint32_t i = 0; i < n; i++) {
        JsonPair *p = &v->u.object.pairs[i];

        if (indent) {
            for (uint32_t k = 0; k < (level + 1) * indent; k++) string_append_bytes(out, " ", 1);
        }

        serialize_string(&p->key, out);
        string_append_bytes(out, ":", 1);
        if (indent) string_append_bytes(out, " ", 1);

        serialize_value(p->value, out, indent, level + 1);

        if (i + 1 < n) string_append_bytes(out, ",", 1);
        if (indent) string_append_bytes(out, "\n", 1);
    }

    if (indent) {
        for (uint32_t k = 0; k < level * indent; k++) string_append_bytes(out, " ", 1);
    }

    string_append_bytes(out, "}", 1);
}

void serialize_value(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
    if (v->kind == JSON_NULL) {
        string_append_bytes(out, "null", 4);
        return;
    }

    if (v->kind == JSON_BOOL) {
        if (v->u.boolean) string_append_bytes(out, "true", 4);
        else string_append_bytes(out, "false", 5);
        return;
    }

    if (v->kind == JSON_INT) {
        string s = string_format("%lli", (long long)v->u.integer);
        string_append_bytes(out, s.data, s.length);
        free_sized(s.data, s.mem_length);
        return;
    }

    if (v->kind == JSON_DOUBLE) {
        string s = string_format("%.17g", v->u.real);
        string_append_bytes(out, s.data, s.length);
        free_sized(s.data, s.mem_length);
        return;
    }

    if (v->kind == JSON_STRING) {
        serialize_string(&v->u.string, out);
        return;
    }

    if (v->kind == JSON_ARRAY) {
        serialize_array(v, out, indent, level);
        return;
    }

    if (v->kind == JSON_OBJECT) {
        serialize_object(v, out, indent, level);
        return;
    }
}

JsonError json_serialize(const JsonValue *value, string *out, uint32_t indent) {
    if (!value || !out) return JSON_ERR_INVALID;
    *out = string_repeat('\0', 0);
    serialize_value(value, out, indent, 0);
    return JSON_OK;
}