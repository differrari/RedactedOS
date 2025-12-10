#pragma once
#include "types.h"
#include "std/std.h"

typedef struct {
    const char *buf;
    uint32_t len;
    uint32_t pos;
} Scanner;

static inline Scanner scanner_make(const char *buf, uint32_t len) {
    return (Scanner){buf, len, 0};
}

bool scan_eof(Scanner *s);
char scan_peek(Scanner *s);
char scan_next(Scanner *s);

bool scan_match(Scanner *s, char c);
bool scan_match_string(Scanner *s, const char *str);

void scan_skip_ws(Scanner *s);

bool scan_read_until(Scanner *s, char stop, string *out);
bool scan_read_string_token(Scanner *s, string *out);
bool scan_read_number_token(Scanner *s, string *out);
bool scan_read_operator(Scanner *s, string *out);