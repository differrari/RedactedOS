#include "token_stream.h"

void ts_init(TokenStream *ts, Tokenizer *tz) {
    ts->tz = tz;
    ts->has_cur = false;
}

bool ts_peek(TokenStream *ts, Token *t) {
    if (!ts->has_cur) {
        if (!tokenizer_next(ts->tz,&ts->cur)) return false;
        ts->has_cur = true;
    }
    if (t) *t = ts->cur;
    return true;
}

bool ts_next(TokenStream *ts, Token *t) {
    if (ts->has_cur) {
        if (t) *t = ts->cur;
        ts->has_cur = false;
        return true;
    }
    return tokenizer_next(ts->tz, t);
}

bool ts_expect(TokenStream *ts, TokenKind k, Token *out) {
    Token t;
    if (!ts_next(ts, &t)) return false;
    if (t.kind != k) return false;
    if (out) *out = t;
    return true;
}

bool ts_expect_operator(TokenStream *ts, const char *op) {
    Token t;
    if (!ts_next(ts, &t)) return false;
    return token_is_operator_token(&t, op);
}

bool ts_expect_identifier(TokenStream *ts, string *out) {
    Token t;
    if (!ts_next(ts, &t)) return false;
    if (t.kind != TOK_IDENTIFIER) return false;
    if (out) *out = string_from_literal_length(t.start, t.length);
    return true;
}

bool ts_expect_double(TokenStream *ts,double *out_double) {
    Token a, b;
    if (!ts_peek(ts, &a))return false;

    if (a.kind == TOK_OPERATOR && token_is_operator_token(&a, "-")) {
        ts_next(ts, &a);
        if (!ts_peek(ts, &b)) return false;
        if (!token_is_number(&b)) return false;
        ts_next(ts, &b);

        string merged;
        if (!token_merge_negative_number(&a, &b, &merged)) return false;

        Token tmp;
        tmp.kind = TOK_NUMBER;
        tmp.start=merged.data;
        tmp.length = merged.length;
        tmp.pos = 0;

        bool ok = token_to_double(&tmp, out_double);
        string_free(merged);
        return ok;
    }

    ts_next(ts, &a);
    if (!token_is_number(&a)) return false;
    return token_to_double(&a, out_double);
}

bool ts_expect_int(TokenStream *ts, int64_t *out_int) {
    Token a, b;

    if (!ts_peek(ts, &a)) return false;

    if (a.kind == TOK_OPERATOR && token_is_operator_token(&a, "-")) {
        ts_next(ts, &a);

        if (!ts_peek(ts, &b)) return false;
        if (!token_is_number(&b)) return false;

        ts_next(ts, &b);

        string merged;
        if (!token_merge_negative_number(&a, &b, &merged)) return false;

        Token tmp;
        tmp.kind = TOK_NUMBER;
        tmp.start = merged.data;
        tmp.length = merged.length;
        tmp.pos = a.pos;

        int64_t iv;
        bool ok = token_to_int64(&tmp, &iv);

        string_free(merged);
        if (!ok) return false;

        *out_int = iv;
        return true;
    }

    ts_next(ts, &a);
    if (!token_is_number(&a)) return false;

    for (uint32_t i = 0; i < a.length; i++) {
        char c = a.start[i];
        if (c == '.' || c == 'e' || c == 'E') return false; 
    }

    return token_to_int64(&a, out_int);
}