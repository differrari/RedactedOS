#include "std/string.h"
#include "syscalls/syscalls.h"
#include "memory/memory_access.h"
#include "std/memfunctions.h"

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
    return (string){ .data = buf, .length = len, .mem_length = len+1 };
}

string string_repeat(char symbol, uint32_t amount){
    char *buf = (char*)malloc(amount + 1);
    memset(buf, symbol, amount);
    buf[amount] = 0;
    return (string){ .data = buf, .length = amount, .mem_length = amount+1 };
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
    return (string){ .data = buf, .length = 1, .mem_length = 2 };
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

string string_from_hex(uint64_t value) {
    char *buf = (char*)malloc(18);
    uint32_t len = parse_hex(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 18 };
}

uint32_t parse_bin(uint64_t value, char* buf){
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'b';
    bool started = false;
    for (uint32_t i = 63;; i --) {
        char bit = (value >> i) & 1  ? '1' : '0';
        if (started || bit != '0' || i == 0) {
            started = true;
            buf[len++] = bit;
        }
        if (i == 0) break;
    }

    buf[len] = 0;
    return len;
}

string string_from_bin(uint64_t value) {
    char *buf = (char*)malloc(66);
    uint32_t len = parse_bin(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 66 };
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
            if (fmt[i] == 'x') {
                uint64_t val = args[arg_index++];
                len += parse_hex(val,(char*)(buf + len));
            } else if (fmt[i] == 'b') {
                uint64_t val = args[arg_index++];
                string bin = string_from_bin(val);
                for (uint32_t j = 0; j < bin.length && len < 255; j++) buf[len++] = bin.data[j];
                free(bin.data,bin.mem_length);
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
                    buf[len++] = '-';
                    val = (uint64_t)(-(int)val);
                }
            
                do {
                    temp[temp_len++] = '0' + (val % 10);
                    val /= 10;
                } while (val && temp_len < 20);
            
                for (int j = temp_len - 1; j >= 0 && len < 255; j--) {
                    buf[len++] = temp[j];
                }
            }else if (fmt[i] == 'f') {
                float val = *((float*)&args[arg_index++]);
                if (val < 0) {
                    buf[len++] = '-';
                    val = -val;
                }

                uint64_t whole = (uint64_t)val;
                double frac = val - (double)whole;

                char temp[21];
                uint32_t temp_len = 0;
                do {
                    temp[temp_len++] = '0' + (whole % 10);
                    whole /= 10;
                } while (whole && temp_len < 20);

                for (int j = temp_len - 1; j >= 0 && len < 255; j--) {
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

    buf[len] = 0;
    return (string){ .data = buf, .length = len, .mem_length = 256 };
}

int strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strstart(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return 0;
}

int strend(const char *a, const char *b) {
    while (*a && *b) {
        if (*a == *b) {
            const char *pa = a, *pb = b;
            while (*pa == *pb) {
                if (!*pa) return 0;
                pa++; pb++;
            }
        }
        a++;
    }
    return 1;
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

bool utf16tochar(const uint16_t* str_in, char* out_str, size_t max_len) {
    size_t out_i = 0;
    while (str_in[out_i] != 0 && out_i + 1 < max_len) {
        uint16_t wc = str_in[out_i];
        out_str[out_i] = (wc <= 0x7F) ? (char)wc : '?';
        out_i++;
    }
    out_str[out_i] = '\0';
    return true;
}