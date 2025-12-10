#include "data/parser/parser.h"
#include "std/string.h"

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static bool parse_number_double_token(const char *buf, uint32_t len, double *out) {
    if (!buf || !len) return false;

    uint32_t pos = 0;
    bool neg = false;

    if (buf[pos] == '-') {
        neg = true;
        pos++;
        if (pos >= len) return false;
    }

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
        pos++;
        if (pos >= len) return false;
        if (buf[pos] < '0' || buf[pos] > '9') return false;
        double base = 0.1;
        while (pos < len) {
            char c = buf[pos];
            if (c < '0' || c > '9') break;
            fp += (double)(c - '0') * base;
            base *= 0.1;
            pos++;
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
    if (neg) val = -val;

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

ParserMark parser_mark(Parser *p) {
    ParserMark m;
    m.pos = p->s->pos;
    return m;
}

void parser_reset(Parser *p, ParserMark m) {
    p->s->pos = m.pos;
    p->failed = false;
    p->err = PARSER_ERR_NONE;
    p->err_msg = 0;
    p->err_pos = m.pos;
}

void parser_fail(Parser *p, ParserError err, const char *msg) {
    if (p->failed) return;

    p->failed = true;
    p->err = err;
    p->err_msg = msg;
    p->err_pos = p->s->pos;
}

char parser_peek(Parser *p) {
    return scan_peek(p->s);
}

char parser_next(Parser *p) {
    return scan_next(p->s);
}

bool parser_eof(Parser *p) {
    return scan_eof(p->s);
}

void parser_skip_ws(Parser *p) {
    scan_skip_ws(p->s);
}

bool parser_expect_char(Parser *p, char c, const char *msg) {
    if (p->failed) return false;

    if (!scan_match(p->s, c)) {
        parser_fail(p, scan_eof(p->s) ? PARSER_ERR_EOF : PARSER_ERR_UNEXPECTED_CHAR, msg);
        return false;
    }
    return true;
}

bool parser_expect_string(Parser *p, const char *lit, const char *msg) {
    if (p->failed) return false;

    if (!scan_match_string(p->s, lit)) {
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, msg);
        return false;
    }
    return true;
}

bool parser_read_identifier(Parser *p, string *out) {
    if (p->failed) return false;

    if (parser_eof(p)) {
        parser_fail(p, PARSER_ERR_EOF, 0);
        return false;
    }
    uint32_t start = p->s->pos;
    char c = parser_peek(p);
    if (!(is_alpha(c) || c == '_')) {
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, 0);
        return false;
    }
    parser_next(p);
    while (!parser_eof(p)) {
        char d = parser_peek(p);
        if (!(is_alnum(d) || d == '_')) break;
        parser_next(p);
    }
    uint32_t end = p->s->pos;
    *out = string_from_literal_length(p->s->buf + start, end - start);
    return true;
}

bool parser_read_number_string(Parser *p, string *out) {
    if (p->failed) return false;

    if (!scan_read_number_token(p->s, out)) {
        parser_fail(p, PARSER_ERR_INVALID_NUMBER, 0);
        return false;
    }
    return true;
}

bool parser_read_quoted_string(Parser *p, string *out) {
    if (p->failed) return false;

    if (!scan_read_string_token(p->s, out)) {
        parser_fail(p, PARSER_ERR_INVALID_STRING, 0);
        return false;
    }
    return true;
}

bool parser_read_int64(Parser *p, int64_t *out) {
    if (p->failed) return false;

    string tmp;
    if (!parser_read_number_string(p, &tmp)) return false;

    bool ok = true;
    for (uint32_t i = 0; i < tmp.length; i++) {
        char c = tmp.data[i];
        if (i == 0 && c == '-') continue;
        if (c < '0' || c > '9') {
            ok = false;
            break;
        }
    }

    if (!ok) {
        string_free(tmp);
        parser_fail(p, PARSER_ERR_INVALID_NUMBER, 0);
        return false;
    }

    int64_t v = parse_int64(tmp.data, tmp.length);
    string_free(tmp);
    *out = v;
    return true;
}

bool parser_read_uint64(Parser *p, uint64_t *out) {
    if (p->failed) return false;

    string tmp;
    if (!parser_read_number_string(p, &tmp)) return false;

    bool ok = true;
    for (uint32_t i = 0; i < tmp.length; i++) {
        char c = tmp.data[i];
        if (c < '0' || c > '9') {
            ok = false;
            break;
        }
    }

    if (!ok) {
        string_free(tmp);
        parser_fail(p, PARSER_ERR_INVALID_NUMBER, 0);
        return false;
    }

    uint64_t v = parse_int_u64(tmp.data, tmp.length);
    string_free(tmp);
    *out = v;
    return true;
}

bool parser_read_double(Parser *p, double *out) {
    if (p->failed) return false;

    string tmp;
    if (!parser_read_number_string(p, &tmp)) return false;

    double v = 0.0;
    bool ok = parse_number_double_token(tmp.data, tmp.length, &v);
    string_free(tmp);

    if (!ok) {
        parser_fail(p, PARSER_ERR_INVALID_NUMBER, 0);
        return false;
    }

    *out = v;
    return true;
}

bool parser_span(Parser *p, ParserMark m, string *out) {
    if (p->failed) return false;

    if (p->s->pos < m.pos) {
        parser_fail(p, PARSER_ERR_GENERIC, 0);
        return false;
    }
    uint32_t start = m.pos;
    uint32_t end = p->s->pos;
    *out = string_from_literal_length(p->s->buf + start, end - start);
    return true;
}

bool parser_read_until(Parser *p, char stop, string *out) {
    if (p->failed) return false;

    if (!scan_read_until(p->s, stop, out)) {
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, 0);
        return false;
    }
    return true;
}

bool parser_read_operator(Parser *p, string *out) {
    if (p->failed) return false;

    if (!scan_read_operator(p->s, out)) {
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, 0);
        return false;
    }
    return true;
}

bool parser_expect_operator(Parser *p, const char *op, const char *msg) {

    if (p->failed) return false;

    ParserMark m = parser_mark(p);

    string tmp;
    if (!scan_read_operator(p->s, &tmp)) {
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, msg);
        return false;
    }

    if (strcmp(tmp.data, op) != 0) {
        parser_reset(p, m);
        string_free(tmp);
        parser_fail(p, PARSER_ERR_UNEXPECTED_TOKEN, msg);
        return false;
    }

    string_free(tmp);
    return true;
}