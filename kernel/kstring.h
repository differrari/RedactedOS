#pragma once

#include "types.h"

typedef struct {
    char *data;
    uint32_t length;
} kstring;

#define string_format(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        string_format_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

kstring string_l(const char *literal);
kstring string_ca_max(const char *array, uint32_t max_length);
kstring string_c(const char c);
kstring string_from_hex(uint64_t value);
bool string_equals(kstring a, kstring b);
kstring string_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count);

bool strcmp(const char *a, const char *b);
bool strcont(const char *a, const char *b);