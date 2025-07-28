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

uint32_t strlen(char *s, uint32_t max_length);
string string_l( char *literal);
string string_ca_max( char *array, uint32_t max_length);
string string_c( char c);
string string_from_hex(uint64_t value);
bool string_equals(string a, string b);
string string_format( char *fmt, ...);
string string_format_va( char *fmt, va_list args);
size_t string_format_va_buf( char *fmt, char *out, va_list args);
string string_tail( char *array, uint32_t max_length);
string string_repeat(char symbol, uint32_t amount);

char tolower(char c);
int strcmp( char *a,  char *b, bool case_insensitive);
bool strcont( char *a,  char *b);
int strstart(char *a, char *b, bool case_insensitive);
int strend( char *a,  char *b, bool case_insensitive);
int strindex( char *a,  char *b);

uint64_t parse_hex_u64(char* str, size_t size);

bool utf16tochar( uint16_t* str_in, char* out_str, size_t max_len);

#ifdef __cplusplus
}
#endif