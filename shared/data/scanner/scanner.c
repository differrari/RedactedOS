#include "data/scanner/scanner.h"

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

void scan_skip_ws(Scanner *s, bool skip_nl) {
    while (!scan_eof(s)) {
        char c = s->buf[s->pos];
        if (c==' '||(skip_nl && c=='\n')||c=='\t'||c=='\r') s->pos++;
        else break;
    }
}