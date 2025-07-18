#include "kstring.h"
#include "memory/kalloc.h"
#include "memory/memory_access.h"
#include "std/string.h"
#include "std/memfunctions.h"

//TODO: we can most likely get rid of this class now and use string entirely
static uint32_t compute_length(const char *s, uint32_t max_length) {
    if (s == NULL) {
        return 0;
    }
    uint32_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len] != '\0') {
        len++;
    }
    return len;
}

kstring kstring_l(const char *literal) {
    if (literal == NULL) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    uint32_t len = compute_length(literal, 0);
    char *buf = (char*)talloc(len + 1);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = literal[i];
    }
    buf[len] = '\0';
    return (kstring){ .data = buf, .length = len };
}

kstring kstring_repeat(char symbol, uint32_t amount){
    char *buf = (char*)talloc(amount + 1);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    memset(buf, symbol, amount);
    buf[amount] = '\0';
    return (kstring){ .data = buf, .length = amount };
}

kstring kstring_tail(const char *array, uint32_t max_length){
    if (array == NULL) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    uint32_t len = compute_length(array, 0);
    int offset = (int)len - (int)max_length;
    if (offset < 0) offset = 0;
    uint32_t adjusted_len = len - offset;
    char *buf = (char*)talloc(adjusted_len + 1);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    for (uint32_t i = 0; i < adjusted_len; i++) {
        buf[i] = array[offset + i];
    }
    buf[adjusted_len] = '\0';
    return (kstring){ .data = buf, .length = adjusted_len };
}

kstring kstring_ca_max(const char *array, uint32_t max_length) {
    if (array == NULL) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    uint32_t len = compute_length(array, max_length);
    char *buf = (char*)talloc(len + 1);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = array[i];
    }
    buf[len] = '\0';
    return (kstring){ .data = buf, .length = len };
}

kstring kstring_c(const char c){
    char *buf = (char*)talloc(2);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    buf[0] = c;
    buf[1] = '\0';
    return (kstring){ .data = buf, .length = 1 };
}

kstring kstring_from_hex(uint64_t value) {
    char *buf = (char*)talloc(18);
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
    return (kstring){ .data = buf, .length = len };
}

kstring kstring_from_bin(uint64_t value) {
    char *buf = (char*)talloc(67);
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
    return (kstring){ .data = buf, .length = len };
}

bool kstring_equals(kstring a, kstring b) {
    return strcmp(a.data,b.data, false) == 0;
}

kstring kstring_format_args(const char *fmt, const uint64_t *args, uint32_t arg_count) {
    if (fmt == NULL) {
        return (kstring){ .data = NULL, .length = 0 };
    }
    // args può essere NULL se arg_count==0, altrimenti va validato
    if (arg_count > 0 && args == NULL) {
        return (kstring){ .data = NULL, .length = 0 };
    }

    char *buf = (char*)talloc(256);
    if (!buf) {
        return (kstring){ .data = NULL, .length = 0 };
    }

    uint32_t len = 0;
    uint32_t arg_index = 0;
    for (uint32_t i = 0; fmt[i] && len < 255; i++) {
        if (fmt[i] == '%' && fmt[i+1]) {
            i++;
            if (arg_index >= arg_count) {
                // troppi placeholder, copio il resto letterale
                buf[len++] = '%';
                buf[len++] = fmt[i];
                continue;
            }
            switch (fmt[i]) {
              case 'x': {
                uint64_t val = args[arg_index++];
                kstring hex = kstring_from_hex(val);
                for (uint32_t j = 0; j < hex.length && len < 255; j++) buf[len++] = hex.data[j];
                temp_free(hex.data, hex.length);
                break;
              }
              case 'b': {
                uint64_t val = args[arg_index++];
                kstring bin = kstring_from_bin(val);
                for (uint32_t j = 0; j < bin.length && len < 255; j++) buf[len++] = bin.data[j];
                temp_free(bin.data, bin.length);
                break;
              }
              case 'c': {
                uint64_t val = args[arg_index++];
                buf[len++] = (char)val;
                break;
              }
              case 's': {
                const char *str = (const char *)(uintptr_t)args[arg_index++];
                if (str) {
                  for (uint32_t j = 0; str[j] && len < 255; j++) buf[len++] = str[j];
                }
                break;
              }
              case 'i': {
                uint64_t val = args[arg_index++];
                char temp[21];
                uint32_t temp_len = 0;
                bool negative = false;
                if ((int64_t)val < 0) {
                    negative = true;
                    val = (uint64_t)(-(int64_t)val);
                }
                do {
                    temp[temp_len++] = '0' + (val % 10);
                    val /= 10;
                } while (val && temp_len < sizeof(temp)-1);
                if (negative && temp_len < sizeof(temp)-1) {
                    temp[temp_len++] = '-';
                }
                for (int j = temp_len - 1; j >= 0 && len < 255; j--) {
                    buf[len++] = temp[j];
                }
                break;
              }
              default:
                buf[len++] = '%';
                buf[len++] = fmt[i];
            }
        } else {
            buf[len++] = fmt[i];
        }
    }
    buf[len] = '\0';
    return (kstring){ .data = buf, .length = len };
}