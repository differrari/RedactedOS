#include "std/string.h"
#include "syscalls/syscalls.h"
#include "std/memfunctions.h"

uint32_t strlen(const char *s, uint32_t max_length){
    if (s == NULL) return 0;
    
    uint32_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len] != '\0') len++;
    
    return len;
}

string string_l(const char *literal){
    if (literal == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0};
    
    uint32_t len = strlen(literal, 0);
    char *buf = (char*)malloc(len + 1);
    if (!buf) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    for (uint32_t i = 0; i < len; i++) buf[i] = literal[i];

    buf[len] = '\0';
    return (string){ .data = buf, .length = len, .mem_length = len + 1 };
}

string string_repeat(char symbol, uint32_t amount){
    char *buf = (char*)malloc(amount + 1);
    memset(buf, symbol, amount);
    buf[amount] = 0;
    return (string){ .data = buf, .length = amount, .mem_length = amount+1 };
}

string string_tail(const char *array, uint32_t max_length){
    
    if (array == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    uint32_t len = strlen(array, 0);
    int offset = (int)len - (int)max_length;
    if (offset < 0) offset = 0;
    
    uint32_t adjusted_len = len - offset;
    char *buf = (char*)malloc(adjusted_len + 1);
    if (!buf) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    for (uint32_t i = 0; i < adjusted_len; i++) buf[i] = array[offset + i];
    
    buf[adjusted_len] = '\0';
    return (string){.data = buf, .length = adjusted_len, .mem_length = adjusted_len + 1 };
}

string string_ca_max(const char *array, uint32_t max_length){
    if (array == NULL) return (string){.data = NULL, .length = 0, .mem_length= 0 };

    uint32_t len = strlen(array, max_length);
    char *buf = (char*)malloc(len + 1);
    if(!buf) return (string){ .data = NULL, .length = 0, .mem_length=0 };

    for (uint32_t i = 0; i < len; i++) buf[i] = array[i];

    buf[len] = '\0';
    return (string){ .data = buf, .length = len, .mem_length = len+1};
}

string string_c(const char c){
    char *buf = (char*)malloc(2);
    buf[0] = c;
    buf[1] = 0;
    return (string){.data = buf, .length = 1, .mem_length = 2};
}

uint32_t parse_hex(uint64_t value, char* buf){
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    bool started = false;
    for (uint32_t i = 60;; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char curr_char = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        if (started || curr_char != '0' || i == 0) {
            started = true;
            buf[len++] = curr_char;
        }
        
        if (i == 0) break;
    }

    buf[len] = 0;
    return len;
}

string string_from_hex(uint64_t value){
    char *buf = (char*)malloc(18);
    uint32_t len = parse_hex(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 18 };
}

uint32_t parse_bin(uint64_t value, char* buf){
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'b';
    bool started = false;
    for (uint32_t i = 63;; i --){
        char bit = (value >> i) & 1  ? '1' : '0';
        if (started || bit != '0' || i == 0){
            started = true;
            buf[len++] = bit;
        }
        
        if (i == 0) break;
    }

    buf[len] = 0;
    return len;
}

string string_from_bin(uint64_t value){
    char *buf = (char*)malloc(66);
    uint32_t len = parse_bin(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 66 };
}

bool string_equals(string a, string b){
    return strcmp(a.data,b.data, false) == 0;
}

string string_format(const char *fmt, ...){
    if (fmt == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0};

    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    string result = string_format_va(fmt, args);
    va_end(args);
    return result;
}

string string_format_va(const char *fmt, va_list args){
    char *buf = (char*)malloc(256);
    size_t len = string_format_va_buf(fmt, buf, args);
    return (string){ .data = buf, .length = len, .mem_length = 256 };
}

size_t string_format_va_buf(const char *fmt, char *buf, va_list args){
    size_t len = 0;
    for (uint32_t i = 0; fmt[i] && len < 255; i++){
        if (fmt[i] == '%' && fmt[i+1]){
            i++;
            if (fmt[i] == 'x'){
                uint64_t val = va_arg(args, uint64_t);
                len += parse_hex(val,(char*)(buf + len));
                
            } else if (fmt[i] == 'b') {
                uint64_t val = va_arg(args, uint64_t);
                string bin = string_from_bin(val);
                for(uint32_t j = 0; j < bin.length && len < 255; j++) buf[len++] = bin.data[j];
                free(bin.data,bin.mem_length);
                
            } else if (fmt[i] == 'c') {
                uint64_t val = va_arg(args, uint64_t);
                buf[len++] = (char)val;
                
            } else if (fmt[i] == 's') {
                 char *str = ( char *)va_arg(args, uintptr_t);
                for (uint32_t j = 0; str[j] && len < 255; j++) buf[len++] = str[j];
                
            } else if (fmt[i] == 'i') {
                uint64_t val = va_arg(args, long int);
                char temp[21];
                uint32_t temp_len = 0;
            
                if ((int)val < 0) {
                    buf[len++] = '-';
                    val = (uint64_t)(-(int)val);
                }
                
                do {
                    temp[temp_len++] = '0' + (val % 10);
                    val /= 10;
                } while (val && temp_len < 20);
            
                for (int j = temp_len - 1; j >= 0 && len < 255; j--){
                    buf[len++] = temp[j];
                }
            } else if (fmt[i] == 'f' || fmt[i] == 'd') {
                double val = va_arg(args, double);
                if (val < 0){
                    buf[len++] = '-';
                    val = -val;
                }
                uint64_t whole = (uint64_t)val;
                double frac = val - (double)whole;

                char temp[21];
                uint32_t temp_len = 0;
                do {
                    temp[temp_len++] = '0' +(whole % 10);
                    whole /= 10;
                } while(whole && temp_len < 20);

                for (int j = temp_len - 1; j >= 0 && len < 255; j--){
                    buf[len++] = temp[j];
                }
                if (len < 255) buf[len++] = '.';
                
                for (int d = 0; d < 6 && len < 255; d++) {
                    frac *= 10;
                    int digit = (int)frac;
                    buf[len++] = '0' + digit;
                    frac -= digit;
                }
            } else {
                buf[len++] = '%';
                buf[len++] = fmt[i];
            }
        } else {
            buf[len++] = fmt[i];
        }
    }

    buf[len]=0;
    return len;
}

char tolower(char c){
    if (c >= 'A' && c <= 'Z') return c + 'a' - 'A';
    return c;
}

int strcmp(const char *a, const char *b, bool case_insensitive){
    if (a == NULL && b == NULL)return 0; //i guess
    if (a == NULL) return -1;  
    if (b == NULL) return  1;

    while (*a && *b){
        char ca = *a;
        char cb = *b;
        if (case_insensitive){
            ca = tolower((unsigned char)ca);
            cb = tolower((unsigned char)cb);
        }
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (case_insensitive) return tolower((unsigned char)*a) - tolower((unsigned char)*b);
    
    return (unsigned char)*a - (unsigned char)*b;
}

int strstart(const char *a, const char *b, bool case_insensitive){
    int index = 0;
    while (*a && *b){
        char ca = *a;
        char cb = *b;

        if (case_insensitive){
            ca = tolower(ca);
            cb = tolower(cb);
        }

        if (ca != cb) return index;
        a++; b++; index++;
    }
    return index;
}

int strindex(const char *a, const char *b){
    for (int i = 0; a[i]; i++){
        int j = 0;
        while (b[j] && a[i + j] == b[j]) j++;
        if (!b[j]) return i;
    }
    return -1;
}

int strend(const char *a, const char *b, bool case_insensitive){
    while (*a && *b){
        char ca = case_insensitive ? tolower((unsigned char)*a) : *a;
        char cb = case_insensitive ? tolower((unsigned char)*b) : *b;

        if (ca == cb){
            const char *pa = a, *pb = b;
            while (1){
                char cpa = case_insensitive ? tolower((unsigned char)*pa) : *pa;
                char cpb = case_insensitive ? tolower((unsigned char)*pb) : *pb;

                if (!cpa) return 0;
                if (cpa != cpb) break;

                pa++; pb++;
            }
        }
        a++;
    }
    return 1;
}

bool strcont(const char *a, const char *b){
    while (*a){
        const char *p = a, *q = b;
        while (*p && *q && *p == *q){
            p++; q++;
        }
        if (*q == 0) return 1;
        a++;
    }
    return 0;
}

bool utf16tochar(uint16_t* str_in, char* out_str, size_t max_len){
    size_t out_i = 0;
    for (size_t i = 0; i < max_len && str_in[i]; i++){
        uint16_t wc = str_in[i];
        out_str[out_i++] = (wc <= 0x7F) ? (char)(wc & 0xFF) : '?';
    }
    out_str[out_i++] = '\0';
    return true;
}

uint64_t parse_hex_u64(char* str, size_t size){
    uint64_t result = 0;
    for (uint32_t i = 0; i < size; i++){
        char c = str[i];
        uint8_t digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = (result << 4) | digit;
    }
    return result;
}


string string_from_const(const char *lit)
{
    uint32_t len = strlen(lit, 0);
    return (string){ (char *)lit, len, len };
}

string string_concat(string a, string b)
{
    uint32_t len = a.length + b.length;
    char *dst = (char *)malloc(len);
    if (!dst) return (string){0};
    memcpy(dst, a.data, a.length);
    memcpy(dst + a.length, b.data, b.length);
    return (string){ dst, len, len };
}

void string_concat_inplace(string *dest, string src) //b string_concat_inplace
{
    if (!dest || !src.data) return;

    uint32_t new_len = dest->length + src.length;
    uint32_t new_cap = new_len + 1;

    uintptr_t raw = malloc(new_cap);
    if (!raw) return;
    char *dst = (char *)raw;

    if (dest->data && dest->length) {
        memcpy(dst, dest->data, dest->length);
    }

    memcpy(dst + dest->length, src.data, src.length);
    dst[new_len] = '\0';
    if (dest->data) {
        free(dest->data, dest->mem_length);
    }
    dest->data = dst;
    dest->length = new_len;
    dest->mem_length = new_cap;
}

void string_append_bytes(string *dest, const void *buf, uint32_t len)
{
    if (!len) return;
    string tmp = { (char *)buf, len, len };
    string_concat_inplace(dest, tmp);
}