#pragma once

#include "types.h"

typedef struct {
    char *data;
    uint32_t length;
} kstring;

#define kstring_format(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        kstring_format_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

kstring kstring_l(const char *literal);
kstring kstring_ca_max(const char *array, uint32_t max_length);
kstring kstring_c(const char c);
kstring kstring_from_hex(uint64_t value);
bool kstring_equals(kstring a, kstring b);
kstring kstring_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count);
kstring kstring_tail(const char *array, uint32_t max_length);
kstring kstring_repeat(char symbol, uint32_t amount);