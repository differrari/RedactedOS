#include "json.h"
#include "std/std.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "data/scanner/scanner.h"
#include "data/tokenizer/tokenizer.h"
#include "helpers/token_stream.h"
#include "helpers/token_utils.h"

static JsonError json_parse_value(TokenStream *ts, JsonValue **out);

static JsonError json_parse_string_token(Token *tok, JsonValue **out) {
    const char *buf = tok->start;
    uint32_t len = tok->length;
    if (len < 2 || buf[0] != '"' || buf[len - 1] != '"') return JSON_ERR_INVALID;

    uint32_t pos = 1;
    string s = string_repeat('\0', 0);

    while (pos < len) {
        char c = buf[pos++];
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
            if (pos >= len) {
                free_sized(s.data, s.mem_length);
                return JSON_ERR_INVALID;
            }
            char e = buf[pos++];
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

static JsonError json_parse_number_tokens(TokenStream *ts, JsonValue **out){
    Token a, b;
    if (!ts_peek(ts, &a)) {
        if (ts->tz->failed) return JSON_ERR_INVALID;
        return JSON_ERR_INVALID;
    }

    bool negative = false;
    Token num;

    if (a.kind == TOK_OPERATOR && token_is_operator_token(&a, "-")) {
        ts_next(ts, &a);
        if (!ts_next(ts, &b)) {
            if (ts->tz->failed)return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }
        if (!token_is_number(&b)) return JSON_ERR_INVALID;
        negative = true;
        num = b;
    } else {
        if (!ts_next(ts, &a)) {
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }
        if (!token_is_number(&a)) return JSON_ERR_INVALID;
        num = a;
    }

    bool is_int = true;
    for (uint32_t i = 0; i < num.length; i++) {
        char c = num.start[i];
        if (c == '.' || c == 'e' || c == 'E') {
            is_int = false;
            break;
        }
    }

    if (is_int) {
        int64_t iv;
        if (!token_to_int64(&num, &iv)) return JSON_ERR_INVALID;
        if (negative) iv =-iv;
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_INT;
        v->u.integer = iv;
        *out = v;
        return JSON_OK;
    }

    double d;
    if (!token_to_double(&num, &d)) return JSON_ERR_INVALID;
    if (negative) d = -d;
    JsonValue *v = malloc(sizeof(JsonValue));
    if (!v) return JSON_ERR_OOM;
    v->kind = JSON_DOUBLE;
    v->u.real = d;
    *out = v;
    return JSON_OK;
}

static JsonError json_parse_array(TokenStream *ts, JsonValue **out) {
    Token t;
    if (!ts_expect(ts, TOK_LBRACKET, &t)) return JSON_ERR_INVALID;

    JsonValue *arr = malloc(sizeof(JsonValue));
    if (!arr) return JSON_ERR_OOM;
    arr->kind = JSON_ARRAY;
    arr->u.array.items = 0;
    arr->u.array.count = 0;

    Token p;
    if (!ts_peek(ts, &p)) {
        json_free(arr);
        if (ts->tz->failed) return JSON_ERR_INVALID;
        return JSON_ERR_INVALID;
    }

    if (p.kind == TOK_RBRACKET) {
        ts_next(ts, &p);
        *out = arr;
        return JSON_OK;
    }

    for (;;) {
        JsonValue *elem = 0;
        JsonError e = json_parse_value(ts, &elem);
        if (e != JSON_OK) {
            json_free(arr);
            return e;
        }

        uint32_t n = arr->u.array.count;
        JsonValue **tmp = malloc((n + 1) * sizeof(JsonValue *));
        if (!tmp) {
            json_free(arr);
            json_free(elem);
            return JSON_ERR_OOM;
        }

        for (uint32_t i = 0; i < n; i++) tmp[i] = arr->u.array.items[i];
        tmp[n] = elem;

        if (arr->u.array.items) free_sized(arr->u.array.items, n * sizeof(JsonValue *));
        arr->u.array.items = tmp;
        arr->u.array.count = n + 1;

        if (!ts_peek(ts, &p)) {
            json_free(arr);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }

        if (p.kind == TOK_RBRACKET) {
            ts_next(ts, &p);
            break;
        }

        if (!ts_expect(ts, TOK_COMMA, &t)) {
            json_free(arr);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }
    }

    *out = arr;
    return JSON_OK;
}

static JsonError json_parse_object(TokenStream *ts, JsonValue **out) {
    Token t;
    if (!ts_expect(ts, TOK_LBRACE, &t)) return JSON_ERR_INVALID;

    JsonValue *obj =malloc(sizeof(JsonValue));
    if (!obj) return JSON_ERR_OOM;
    obj->kind = JSON_OBJECT;
    obj->u.object.pairs = 0;
    obj->u.object.count = 0;

    Token p;
    if (!ts_peek(ts, &p)) {
        json_free(obj);
        if (ts->tz->failed) return JSON_ERR_INVALID;
        return JSON_ERR_INVALID;
    }

    if (p.kind == TOK_RBRACE) {
        ts_next(ts, &p);
        *out = obj;
        return JSON_OK;
    }

    for (;;) {
        Token keytok;
        if (!ts_expect(ts, TOK_STRING, &keytok)) {
            json_free(obj);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }

        if (keytok.length < 2) {
            json_free(obj);
            return JSON_ERR_INVALID;
        }

        string key = string_from_literal_length(keytok.start + 1, keytok.length - 2);

        if (!ts_expect(ts, TOK_COLON, &t)) {
            free_sized(key.data, key.mem_length);
            json_free(obj);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }

        JsonValue *val = 0;
        JsonError e = json_parse_value(ts, &val);
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

        if (!ts_peek(ts, &p)) {
            json_free(obj);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }

        if (p.kind == TOK_RBRACE) {
            ts_next(ts, &p);
            break;
        }

        if (!ts_expect(ts, TOK_COMMA, &t)) {
            json_free(obj);
            if (ts->tz->failed) return JSON_ERR_INVALID;
            return JSON_ERR_INVALID;
        }
    }

    *out = obj;
    return JSON_OK;
}

static JsonError json_parse_value(TokenStream *ts, JsonValue **out) {
    Token t;
    if (!ts_peek(ts, &t)) {
        if (ts->tz->failed) return JSON_ERR_INVALID;
        return JSON_ERR_INVALID;
    }

    if (t.kind == TOK_STRING) {
        ts_next(ts,&t);
        return json_parse_string_token(&t, out);
    }

    if (t.kind == TOK_NUMBER || (t.kind == TOK_OPERATOR && token_is_operator_token(&t, "-"))) {
        return json_parse_number_tokens(ts, out);
    }

    if (t.kind == TOK_LBRACE) return json_parse_object(ts, out);
    if (t.kind == TOK_LBRACKET) return json_parse_array(ts, out);

    if (t.kind == TOK_IDENTIFIER && t.length == 4 && strncmp(t.start, "true", 4) == 0) {
        ts_next(ts, &t);
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_BOOL;
        v->u.boolean = true;
        *out = v;
        return JSON_OK;
    }

    if (t.kind == TOK_IDENTIFIER && t.length ==5 &&strncmp(t.start, "false", 5) == 0) {
        ts_next(ts, &t);
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_BOOL;
        v->u.boolean = false;
        *out = v;
        return JSON_OK;
    }

    if (t.kind == TOK_IDENTIFIER && t.length == 4 &&
        strncmp(t.start, "null", 4) == 0) {
        ts_next(ts, &t);
        JsonValue *v = malloc(sizeof(JsonValue));
        if (!v) return JSON_ERR_OOM;
        v->kind = JSON_NULL;
        *out = v;
        return JSON_OK;
    }

    return JSON_ERR_INVALID;
}

JsonError json_parse(const char *buf, uint32_t len, JsonValue **out) {
    Scanner s = scanner_make(buf, len);
    Tokenizer tz = tokenizer_make(&s);
    TokenStream ts;
    ts_init(&ts, &tz);

    JsonError e =json_parse_value(&ts, out);
    if (e != JSON_OK) return e;

    Token t;
    if (!ts_peek(&ts, &t)) {
        json_free(*out);
        return JSON_ERR_INVALID;
    }
    if (t.kind != TOK_EOF) {
        json_free(*out);
        return JSON_ERR_INVALID;
    }

    return JSON_OK;
}

void json_free(JsonValue *v) {
    if (!v) return;

    if (v->kind == JSON_STRING) {
        free_sized(v->u.string.data, v->u.string.mem_length);
    } else if (v->kind == JSON_ARRAY) {
        for (uint32_t i = 0; i < v->u.array.count; i++) json_free(v->u.array.items[i]);
        if (v->u.array.items)
            free_sized(v->u.array.items, v->u.array.count * sizeof(JsonValue *));
    } else if (v->kind == JSON_OBJECT) {
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
    arr->u.array.count = n + 1;
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
            obj->u.object.pairs[i].value = value;
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

static void serialize_string(const string *s, string *out) {
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

static void serialize_value(const JsonValue *v, string *out, uint32_t indent, uint32_t level);

static void serialize_array(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
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

static void serialize_object(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
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

static void serialize_value(const JsonValue *v, string *out, uint32_t indent, uint32_t level) {
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