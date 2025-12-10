#pragma once

#include "types.h"
#include "data/scanner/scanner.h"
#include "std/string.h"

typedef enum {
    PARSER_ERR_NONE = 0,
    PARSER_ERR_GENERIC,
    PARSER_ERR_EOF,
    PARSER_ERR_UNEXPECTED_CHAR,
    PARSER_ERR_UNEXPECTED_TOKEN,
    PARSER_ERR_INVALID_NUMBER,
    PARSER_ERR_INVALID_STRING
} ParserError;

typedef struct {
    Scanner *s;
    bool failed;
    uint32_t err_pos;
    const char *err_msg;
    ParserError err;
} Parser;

typedef struct {
    uint32_t pos;
} ParserMark;

static inline Parser parser_make(Scanner *s) {
    Parser p;
    p.s = s;
    p.failed = false;
    p.err_pos = 0;
    p.err_msg = 0;
    p.err = PARSER_ERR_NONE;
    return p;
}

static inline bool parser_ok(const Parser *p) {
    return !p->failed;
}

ParserMark parser_mark(Parser *p);
void parser_reset(Parser *p, ParserMark m);

void parser_fail(Parser *p, ParserError err, const char *msg);

char parser_peek(Parser *p);
char parser_next(Parser *p);
bool parser_eof(Parser *p);

void parser_skip_ws(Parser *p);

bool parser_expect_char(Parser *p, char c, const char *msg);
bool parser_expect_string(Parser *p, const char *lit, const char *msg);

bool parser_read_identifier(Parser *p, string *out);
bool parser_read_number_string(Parser *p, string *out);
bool parser_read_quoted_string(Parser *p, string *out);

bool parser_read_int64(Parser *p, int64_t *out);
bool parser_read_uint64(Parser *p, uint64_t *out);
bool parser_read_double(Parser *p, double *out);

bool parser_span(Parser *p, ParserMark m, string *out);
bool parser_read_until(Parser *p, char stop, string *out);
bool parser_read_operator(Parser *p, string *out);
bool parser_expect_operator(Parser *p, const char *op, const char *msg);
