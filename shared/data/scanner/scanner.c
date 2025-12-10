#include "scanner.h"
#include "std/string.h"

static const char *ops3[] = {">>>", "<<=", ">>=", "===", 0};

static const char *ops2[] = {"==","!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::", "->", 0};

static const char ops1[] = "+-*/%<>=!&|^~?:;.,(){}[]";

bool scan_eof(Scanner *s) {
    return s->pos >= s->len;
}

char scan_peek(Scanner *s) {
    if (s->pos >= s->len) return 0;
    return s->buf[s->pos];
}

char scan_next(Scanner *s) {
    if (s->pos >= s->len) return 0;
    return s->buf[s->pos++];
}

bool scan_match(Scanner *s, char c) {
    if (scan_eof(s)) return false;
    if (s->buf[s->pos] != c) return false;
    s->pos++;
    return true;
}

bool scan_match_string(Scanner *s, const char *str) {
    uint32_t i = 0;
    while (str[i]) {
        if (s->pos + i >= s->len) return false;
        if (s->buf[s->pos + i] != str[i]) return false;
        i++;
    }
    s->pos += i;
    return true;
}

void scan_skip_ws(Scanner *s) {
    while (!scan_eof(s)) {
        char c = s->buf[s->pos];
        if (c==' '||c=='\n'||c=='\t'||c=='\r') s->pos++;
        else break;
    }
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool scan_read_until(Scanner *s, char stop, string *out) {
    uint32_t start = s->pos;
    while (!scan_eof(s) && s->buf[s->pos] != stop) s->pos++;
    if (s->pos == start) return false;
    *out = string_from_literal_length(s->buf + start, s->pos - start);
    return true;
}

bool scan_read_string_token(Scanner *s, string *out) {
    if (!scan_match(s, '"')) return false;

    string tmp = string_repeat('\0', 0);
    while (!scan_eof(s)) {
        char c = scan_next(s);
        if (c == '"') {
            *out = tmp;
            return true;
        }

        if (c == '\\') {
            if (scan_eof(s)) { string_free(tmp); return false; }
            char e = scan_next(s);
            if (e == 'u') {
                if (s->pos + 4 > s->len) { string_free(tmp); return false; }
                string_append_bytes(&tmp, "\\u", 2);
                for (int i=0;i<4;i++) {
                    char h = scan_next(s);
                    string_append_bytes(&tmp, &h, 1);
                }
                continue;
            }

            char r = e;
            if (e=='b') r='\b';
            else if (e=='f') r='\f';
            else if (e=='n') r='\n';
            else if (e=='r') r='\r';
            else if (e=='t') r='\t';
            else if (!(e=='"'||e=='\\'||e=='/')) { string_free(tmp); return false; }
            string_append_bytes(&tmp, &r, 1);
        } else {
            string_append_bytes(&tmp, &c, 1);
        }
    }

    string_free(tmp);
    return false;
}

bool scan_read_number_token(Scanner *s, string *out) {
    uint32_t start = s->pos;
    const char *buf = s->buf;
    uint32_t len = s->len;
    uint32_t pos = start;

    if (pos >= len) return false;

    if (buf[pos] == '-') {
        pos++;
        if (pos >= len) return false;
    }

    if (pos < len && buf[pos] == '0' && pos + 1 < len) {
        char p = buf[pos + 1];

        if (p == 'x' || p == 'X') {
            pos += 2;
            if (pos >= len) return false;

            int ok = 0;
            while (pos < len) {
                char c = buf[pos];
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                    pos++;
                    ok = 1;
                } else break;
            }

            if (!ok) return false;

            s->pos = pos;
            *out = string_from_literal_length(buf + start, pos - start);
            return true;
        }

        if (p == 'b' || p == 'B') {
            pos += 2;
            if (pos >= len) return false;
            int ok = 0;
            while (pos < len) {
                char c = buf[pos];
                if (c == '0' || c == '1') {
                    pos++;
                    ok = 1;
                } else break;
            }
            if (!ok) return false;
            s->pos = pos;
            *out = string_from_literal_length(buf + start, pos - start);
            return true;
        }

        if (p == 'o' || p == 'O') {
            pos += 2;
            if (pos >= len) return false;
            int ok = 0;
            while (pos < len) {
                char c = buf[pos];
                if (c >= '0' && c <= '7') {
                    pos++;
                    ok = 1;
                } else break;
            }
            if (!ok) return false;
            s->pos = pos;
            *out = string_from_literal_length(buf + start, pos - start);
            return true;
        }
    }

    if (pos >= len || !(buf[pos] >= '0' && buf[pos] <= '9')) return false;

    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;

    uint32_t int_end = pos;

    if (pos < len && buf[pos] == '.') {
        pos++;
        if (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
            while (pos < len && buf[pos] >= '0' &&buf[pos] <= '9') pos++;
        } else {
            pos = int_end;
        }
    }

    uint32_t mant_end = pos;

    if (pos < len && (buf[pos] == 'e' || buf[pos] == 'E')) {
        uint32_t exp_start = pos;
        pos++;
        if (pos < len && (buf[pos] == '+' || buf[pos] == '-')) pos++;

        if (pos < len && (buf[pos] >= '0' && buf[pos] <= '9')) {
            while (pos < len && (buf[pos] >= '0' && buf[pos] <= '9')) pos++;
            mant_end = pos;
        } else {
            pos =exp_start;
        }
    }

    s->pos = mant_end;
    *out = string_from_literal_length(buf + start, mant_end - start);
    return true;
}

bool scan_read_operator(Scanner *s, string *out) {
    if (scan_eof(s)) return false;

    for (int i=0; ops3[i]; i++) {
        if (scan_match_string(s, ops3[i])) {
            *out = string_from_literal(ops3[i]);
            return true;
        }
    }

    for (int i=0; ops2[i]; i++) {
        if (scan_match_string(s, ops2[i])) {
            *out = string_from_literal(ops2[ i]);
            return true;
        }
    }

    char c = scan_peek(s);
    for (int i=0; ops1[i]; i++) {
        if (c == ops1[i]) {
            scan_next(s);
            *out = string_from_char(c);
            return true;
        }
    }

    return false;
}