#include "string.h"
#include "mmio.h"

static uint32_t compute_length(const char *s, uint32_t max_length) {
    uint32_t len = 0;
    while (len < max_length && s[len] != '\0') {
        len++;
    }
    return len;
}

string string_l(const char *literal) {
    uint32_t len = compute_length(literal, (uint32_t)-1);
    string str;
    str.data = (char *)literal;
    str.length = len;
    return str;
}

string string_c(const char *array, uint32_t max_length) {
    uint32_t len = compute_length(array, max_length);
    char *copy = (char*)alloc(len + 1);
    for (uint32_t i = 0; i < len; i++) {
        copy[i] = array[i];
    }
    copy[len] = '\0';
    string str;
    str.data = copy;
    str.length = len;
    return str;
}

bool string_equals(string a, string b) {
    if (a.length != b.length) return 0;
    for (uint32_t i = 0; i < a.length; i++) {
        if (a.data[i] != b.data[i]) return 0;
    }
    return 1;
}