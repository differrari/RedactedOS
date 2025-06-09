#pragma once

#include "types.h"

typedef struct {
    char *data;
    uint32_t length;
    uint32_t mem_length;
} string;

typedef struct string_list {
    uint32_t count;
    char array[];
} string_list;

#define CAST_ARG(x) \
    _Generic((x), \
        double: ((union { double d; uint64_t u; }){ .d = (x) }).u, \
        float:  ((union { float f; uint64_t u; }){ .f = (x) }).u, \
        default: (x) \
    )

#define string_format(fmt, ...) \
    ({ \
        uint64_t _args[] = { CAST_ARG(__VA_ARGS__) }; \
        string_format_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

string string_l(const char *literal);
string string_ca_max(const char *array, uint32_t max_length);
string string_c(const char c);
string string_from_hex(uint64_t value);
bool string_equals(string a, string b);
string string_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count);
string string_tail(const char *array, uint32_t max_length);
string string_repeat(char symbol, uint32_t amount);

int strcmp(const char *a, const char *b);
bool strcont(const char *a, const char *b);
int strstart(const char *a, const char *b);
int strend(const char *a, const char *b);

bool utf16tochar(const uint16_t* str_in, char* out_str, size_t max_len);