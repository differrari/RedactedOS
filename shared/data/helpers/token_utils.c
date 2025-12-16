#include "token_utils.h"
#include "std/std.h"

bool token_is_number(const Token *t) {
    return t && t->kind == TOK_NUMBER;
}

bool token_is_operator_token(const Token *t, const char *op) {
    if (!t || t->kind != TOK_OPERATOR) return false;
    uint32_t n = (uint32_t)strlen(op);
    if (t->length != n) return false;
    return strncmp(t->start, op, n) == 0;
}

bool token_is_negative_number(const Token *op, const Token *num) {
    if (!op || !num) return false;
    if (!token_is_operator_token(op, "-")) return false;
    return token_is_number(num);
}

bool token_merge_negative_number(const Token *op, const Token *num, string *out) {
    if (!token_is_negative_number(op, num)) return false;
    *out = string_repeat('\0', 0);
    string_append_bytes(out, "-", 1);
    string_append_bytes(out, num->start,num->length);
    return true;
}

bool token_to_int64(const Token *t, int64_t *out) {
    if (!t || !token_is_number(t)) return false;
    string tmp = string_from_literal_length(t->start, t->length);
    int64_t v = parse_int64(tmp.data, tmp.length);
    string_free(tmp);
    *out = v;
    return true;
}

bool token_to_uint64(const Token *t, uint64_t *out) {
    if (!t || !token_is_number(t)) return false;
    string tmp = string_from_literal_length(t->start, t->length);
    uint64_t v= parse_int_u64(tmp.data, tmp.length);
    string_free(tmp);
    *out = v;
    return true;
}

static bool parse_double_literal(const char *buf, uint32_t len, double *out) {
    if (!buf || !len) return false;

    uint32_t pos = 0;
    if (buf[pos] < '0' || buf[pos] > '9') return false;

    double ip = 0.0;
    while (pos < len) {
        char c = buf[pos];
        if (c < '0' || c > '9') break;
        ip = ip * 10.0 + (double)(c - '0');
        pos++;
    }

    double fp = 0.0;
    if (pos < len && buf[pos] == '.') {
        uint32_t p2 = pos + 1;
        if (p2 < len && buf[p2] >= '0' && buf[p2] <= '9') {
            pos = p2;
            double base = 0.1;
            while (pos < len) {
                char c = buf[pos];
                if (c < '0' || c > '9') break;
                
                fp += (double)(c - '0') * base;
                base *= 0.1;
                pos++;
            }
        }
    }

    int exp_val = 0;
    if (pos < len && (buf[pos] == 'e' || buf[pos] == 'E')) {
        pos++;
        if (pos >= len) return false;

        bool exp_neg = false;
        if (buf[pos] == '+' || buf[pos] == '-') {
            if (buf[pos] == '-') exp_neg = true;
            pos++;
            if (pos >= len) return false;
        }

        if (buf[pos] < '0' || buf[pos] > '9') return false;

        while (pos < len) {
            char c = buf[pos];
            if (c < '0' || c > '9') break;
            exp_val = exp_val * 10 + (c - '0');
            pos++;
        }
        if (exp_neg) exp_val = -exp_val;
    }

    if (pos != len) return false;

    double val = ip + fp;
    if (exp_val != 0) {
        double y = 1.0;
        if (exp_val > 0) {
            while (exp_val--) y *= 10.0;
        } else {
            while (exp_val++) y /= 10.0;
        }
        val *= y;
    }

    *out = val;
    return true;
}

bool token_to_double(const Token *t, double *out) {
    if (!t || !token_is_number(t)) return false;
    string tmp = string_from_literal_length(t->start, t->length);
    double v = 0.0;
    bool ok = parse_double_literal(tmp.data, tmp.length, &v);
    string_free(tmp);
    if (!ok) return false;
    *out = v;
    return true;
}