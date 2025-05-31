#include "std/string.h"
#include "syscalls/syscalls.h"
#include "memory/memory_access.h"

static uint32_t compute_length(const char *s, uint32_t max_length) {
    uint32_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len] != '\0') {
        len++;
    }
    return len;
}

string string_l(const char *literal) {
    uint32_t len = compute_length(literal, 0);
    char *buf = (char*)malloc(len+1);
    for (uint32_t i = 0; i < len; i++)
        buf[i] = literal[i];
    buf[len] = 0;
    return (string){ .data = buf, .length = len };
}

string string_repeat(char symbol, uint32_t amount){
    char *buf = (char*)malloc(amount + 1);
    memset(buf, symbol, amount);
    buf[amount] = 0;
    return (string){ .data = buf, .length = amount };
}

string string_tail(const char *array, uint32_t max_length){
    uint32_t len = compute_length(array, 0);
    int offset = len-max_length;
    if (offset < 0) 
        offset = 0; 
    uint32_t adjusted_len = len - offset;
    char *buf = (char*)malloc(adjusted_len + 1);
    for (uint32_t i = offset; i < len; i++)
        buf[i-offset] = array[i];
    buf[len-offset] = 0;
    return (string){ .data = buf, .length = adjusted_len };
}

string string_ca_max(const char *array, uint32_t max_length) {
    uint32_t len = compute_length(array, max_length);
    char *buf = (char*)malloc(len + 1);
    for (uint32_t i = 0; i < len; i++)
        buf[i] = array[i];
    buf[len] = 0;
    return (string){ .data = buf, .length = len };
}

string string_c(const char c){
    char *buf = (char*)malloc(2);
    buf[0] = c;
    buf[1] = 0;
    return (string){ .data = buf, .length = 1 };
}

string string_from_hex(uint64_t value) {
    char *buf = (char*)malloc(18);
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
    return (string){ .data = buf, .length = len };
}

string string_from_bin(uint64_t value) {
    char *buf = (char*)malloc(67);
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'b';

    bool started = false;
    for (uint32_t i = 60;; i --) {
        char bit = (value >> i) & 1  ? '1' : '0';
        if (started || bit != '0' || i == 0) {
            started = true;
            buf[len++] = bit;
        }
        if (i == 0) break;
    }

    buf[len] = 0;
    return (string){ .data = buf, .length = len };
}

bool string_equals(string a, string b) {
    return strcmp(a.data,b.data) == 0;
}

string string_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count) {
    char *buf = (char*)malloc(256);
    uint32_t len = 0;
    uint32_t arg_index = 0;

    for (uint32_t i = 0; fmt[i] && len < 255; i++) {
        if (fmt[i] == '%' && fmt[i+1]) {
            i++;
            if (arg_index >= arg_count) break;
            if (fmt[i] == 'h') {
                uint64_t val = args[arg_index++];
                string hex = string_from_hex(val);
                for (uint32_t j = 0; j < hex.length && len < 255; j++) buf[len++] = hex.data[j];
                free(hex.data,hex.length);
            } else if (fmt[i] == 'b') {
                uint64_t val = args[arg_index++];
                string bin = string_from_bin(val);
                for (uint32_t j = 0; j < bin.length && len < 255; j++) buf[len++] = bin.data[j];
                free(bin.data,bin.length);
            } else if (fmt[i] == 'c') {
                uint64_t val = args[arg_index++];
                buf[len++] = (char)val;
            } else if (fmt[i] == 's') {
                const char *str = (const char *)(uintptr_t)args[arg_index++];
                for (uint32_t j = 0; str[j] && len < 255; j++) buf[len++] = str[j];
            } else if (fmt[i] == 'i') {
                uint64_t val = args[arg_index++];
                char temp[21];
                uint32_t temp_len = 0;
                bool negative = false;
            
                if ((int)val < 0) {
                    negative = true;
                    val = (uint64_t)(-(int)val);
                }
            
                do {
                    temp[temp_len++] = '0' + (val % 10);
                    val /= 10;
                } while (val && temp_len < 20);
            
                if (negative && temp_len < 20) {
                    temp[temp_len++] = '-';
                }
            
                for (int j = temp_len - 1; j >= 0 && len < 255; j--) {
                    buf[len++] = temp[j];
                }
            } else {
                buf[len++] = '%';
                buf[len++] = fmt[i];
            }
        } else {
            buf[len++] = fmt[i];
        }
    }

    buf[len] = 0;
    return (string){ .data = buf, .length = len };
}

int strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool strcont(const char *a, const char *b) {
    while (*a) {
        const char *p = a, *q = b;
        while (*p && *q && *p == *q) {
            p++; q++;
        }
        if (*q == 0) return 1;
        a++;
    }
    return 0;
}