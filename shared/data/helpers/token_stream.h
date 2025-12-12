#pragma once

#include "types.h"
#include "std/string.h"
#include "std/std.h"
#include "data/tokenizer/tokenizer.h"
#include "token_utils.h"

typedef struct {
    Tokenizer *tz;
    Token cur;
    bool has_cur;
} TokenStream;

void ts_init(TokenStream *ts, Tokenizer *tz);
bool ts_peek(TokenStream *ts, Token *t);
bool ts_next(TokenStream *ts, Token *t);

bool ts_expect(TokenStream *ts, TokenKind k, Token *out);
bool ts_expect_operator(TokenStream *ts, const char *op);
bool ts_expect_identifier(TokenStream *ts, string *out);
bool ts_expect_double(TokenStream *ts, double *out_double);
bool ts_expect_int(TokenStream *ts, int64_t *out_int);