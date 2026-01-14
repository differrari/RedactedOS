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
static inline bool is_alpha(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static inline bool is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static inline bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static inline int32_t str_has_char(const char* s, uint32_t n, char c){
    for (uint32_t i = 0; i < n && s[i] != '\0'; i++) if (s[i] == c) return i;
    return -1;
}

extern void free_sized(void*,size_t);

static inline int hex_val(char c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

uint32_t u64_to_base(char *tmp, uint64_t v, unsigned base, int upper);
size_t strlen_max(const char *s, uint32_t max_length);
static inline size_t strlen(const char *s) { return strlen_max(s,0); }
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
    if (str.data && str.mem_length) free_sized(str.data, str.mem_length);
}

int tolower(int c);
int toupper(int c);

int strcmp_case(const char *a, const char *b, bool case_insensitive);
static inline int strcmp(const char *a, const char *b){ return strcmp_case(a, b, false); }
int strncmp_case(const char *a, const char *b, bool case_insensitive, size_t length);
static inline int strncmp(const char *a, const char *b, size_t length) { return strncmp_case(a,b, false, length); }
bool strcont(const char *a, const char *b);
int strstart_case(const char *a, const char *b, bool case_insensitive);
static inline int strstart(const char *a, const char *b) { return strstart_case(a,b,false); }
int strend_case(const char *a, const char *b, bool case_insensitive);
static inline int strend(const char *a, const char *b) { return strend_case(a,b,false); }
int strindex(const char *a, const char *b);
int count_occurrences(const char* str, char c);

uint64_t parse_hex_u64(const char* str, size_t size);
uint64_t parse_int_u64(const char* str, size_t size);
int64_t parse_int64(const char* str, size_t size);

bool utf16tochar(uint16_t* str_in, char* out_str, size_t max_len);

void strcat_buf(const char *a, const char *b, char *dest);
char* strcat_new(const char *a, const char *b);
//@DEPRECATED
string string_from_const(const char *literal);
string string_concat(string a, string b);
void string_concat_inplace(string *dest, string src);
void string_append_bytes(string *dest, const void *buf, uint32_t len);
const char* seek_to(const char *string, char character);
char* strncpy(char* dst, const char* src, size_t cap);
bool parse_uint32_dec(const char *s, uint32_t *out);
char* strcasestr(const char* haystack, const char* needle);

string string_replace_character(char* original, char symbol, char *value);

#ifdef __cplusplus
}
#endif