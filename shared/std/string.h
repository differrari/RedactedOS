#pragma once

#include "types.h"
#include "args.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    uint32_t length;
    uint32_t mem_length;
} string;

typedef struct string_list {
    uint32_t count;
    char array[];
} string_list;

uint32_t strlen(const char *s, uint32_t max_length);
string string_l(const char *literal);
string string_ca_max(const char *array, uint32_t max_length);
string string_c(const char c);
string string_from_hex(uint64_t value);
bool string_equals(string a, string b);
string string_format(const char *fmt, ...);
string string_format_va(const char *fmt, va_list args);
__attribute__((used))
size_t string_format_va_buf(const char *fmt, char *out, va_list args);
string string_tail(const char *array, uint32_t max_length);
string string_repeat(char symbol, uint32_t amount);

char tolower(char c);
int strcmp(const char *a, const char *b, bool case_insensitive);
bool strcont(const char *a, const char *b);
int strstart(const char *a, const char *b, bool case_insensitive);
int strend(const char *a, const char *b, bool case_insensitive);
int strindex(const char *a, const char *b);

uint64_t parse_hex_u64(char* str, size_t size);

bool utf16tochar( uint16_t* str_in, char* out_str, size_t max_len);

string string_from_const(const char *literal);
string string_concat(string a, string b);
void string_concat_inplace(string *dest, string src);
void string_append_bytes(string *dest, const void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif