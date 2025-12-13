#pragma once

#include "types.h"
#include "std/string.h"
#include "data/tokenizer/tokenizer.h"

bool token_is_number(const Token *t);
bool token_is_operator_token(const Token *t, const char *op);
bool token_is_negative_number(const Token *op, const Token *num);
bool token_merge_negative_number(const Token *op, const Token *num, string *out);

bool token_to_int64(const Token *t, int64_t *out);
bool token_to_uint64(const Token *t, uint64_t *out);
bool token_to_double(const Token *t, double *out);