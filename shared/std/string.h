#pragma once

#include "types.h"
#include "args.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STRING_MAX_LEN 256

typedef struct {
    char *data;
    uint32_t length;
    uint32_t mem_length;
} string;

typedef struct string_list {
    uint32_t count;
    char array[];
} string_list;

extern void free(void*,size_t);

uint32_t strlen(const char *s, uint32_t max_length);
string string_from_literal(const char *literal);
string string_from_literal_length(const char *array, uint32_t max_length);
string string_from_char(const char c);
string string_from_hex(uint64_t value);
bool string_equals(string a, string b);
string string_replace(const char *str, char orig, char repl);
string string_format(const char *fmt, ...); //TODO __attribute__((format(printf, 1, 2)));
size_t string_format_buf(char *out, size_t cap, const char *fmt, ...); //TODO __attribute__((format(printf, 3, 4)));
string string_format_va(const char *fmt, va_list args); //TODO __attribute__((format(printf, 1, 0)));
size_t string_format_va_buf(const char *fmt, char *out, size_t cap, va_list args); //TODO __attribute__((format(printf, 1, 0)));
string string_tail(const char *array, uint32_t max_length);
string string_repeat(char symbol, uint32_t amount);

static inline void string_free(string str){
    if (str.data && str.mem_length) free(str.data, str.mem_length);
}

char tolower(char c);
char toupper(char c);

int strcmp(const char *a, const char *b, bool case_insensitive);
int strncmp(const char *a, const char *b, bool case_insensitive, int length);
bool strcont(const char *a, const char *b);
int strstart(const char *a, const char *b, bool case_insensitive);
int strend(const char *a, const char *b, bool case_insensitive);
int strindex(const char *a, const char *b);
int count_occurrences(const char* str, char c);

uint64_t parse_hex_u64(const char* str, size_t size);
uint64_t parse_int_u64(const char* str, size_t size);

bool utf16tochar(uint16_t* str_in, char* out_str, size_t max_len);

string string_from_const(const char *literal);
string string_concat(string a, string b);
void string_concat_inplace(string *dest, string src);
void string_append_bytes(string *dest, const void *buf, uint32_t len);
const char* seek_to(const char *string, char character);
size_t strncpy(char* dst, size_t cap, const char* src);
bool parse_uint32_dec(const char *s, uint32_t *out);

#ifdef __cplusplus
}
#endif