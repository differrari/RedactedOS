#include "kstring.h"
#include "ram_e.h"

static uint32_t compute_length(const char *s, uint32_t max_length) {
    uint32_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len] != '\0') {
        len++;
    }
    return len;
}

kstring string_l(const char *literal) {
    uint32_t len = compute_length(literal, 0);
    char *buf = (char*)talloc(len+1);
    for (int i = 0; i < len; i++)
        buf[i] = literal[i];
    buf[len] = 0;
    return (kstring){ .data = buf, .length = len };
}

kstring string_repeat(char symbol, uint32_t amount){
    char *buf = (char*)talloc(amount + 1);
    memset(buf, symbol, amount);
    buf[amount] = 0;
    return (kstring){ .data = buf, .length = amount };
}

kstring string_tail(const char *array, uint32_t max_length){
    uint32_t len = compute_length(array, 0);
    int offset = len-max_length;
    if (offset < 0) 
        offset = 0; 
    uint32_t adjusted_len = len - offset;
    char *buf = (char*)talloc(adjusted_len + 1);
    for (int i = offset; i < len; i++)
        buf[i-offset] = array[i];
    buf[len-offset] = 0;
    return (kstring){ .data = buf, .length = adjusted_len };
}

kstring string_ca_max(const char *array, uint32_t max_length) {
    uint32_t len = compute_length(array, max_length);
    char *buf = (char*)talloc(len + 1);
    for (int i = 0; i < len; i++)
        buf[i] = array[i];
    buf[len] = 0;
    return (kstring){ .data = buf, .length = len };
}

kstring string_c(const char c){
    char *buf = (char*)talloc(2);
    buf[0] = c;
    buf[1] = 0;
    return (kstring){ .data = buf, .length = 1 };
}

kstring string_from_hex(uint64_t value) {
    char *buf = (char*)talloc(18);
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';

    bool started = false;
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char curr_char = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        if (started || curr_char != '0' || i == 0) {
            started = true;
            buf[len++] = curr_char;
        }
    }

    buf[len] = 0;
    return (kstring){ .data = buf, .length = len };
}

bool string_equals(kstring a, kstring b) {
    return strcmp(a.data,b.data) == 0;
}

kstring string_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count) {
    char *buf = (char*)talloc(256);
    uint32_t len = 0;
    uint32_t arg_index = 0;

    for (uint32_t i = 0; fmt[i] && len < 255; i++) {
        if (fmt[i] == '%' && fmt[i+1]) {
            i++;
            if (arg_index >= arg_count) break;
            if (fmt[i] == 'h') {
                uint64_t val = args[arg_index++];
                kstring hex = string_from_hex(val);
                for (uint32_t j = 0; j < hex.length && len < 255; j++) buf[len++] = hex.data[j];
                temp_free(hex.data,18);
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
    return (kstring){ .data = buf, .length = len };
}

bool strcmp(const char *a, const char *b) {
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