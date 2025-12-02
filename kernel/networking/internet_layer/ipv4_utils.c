#include "ipv4_utils.h"

static char* u8_to_str(uint8_t val, char* out) {
    if (val >= 100) {
        *out++ = '0' + (val / 100);
        val %= 100;
        *out++ = '0' + (val / 10);
        *out++ = '0' + (val % 10);
    } else if (val >= 10) {
        *out++ = '0' + (val / 10);
        *out++ = '0' + (val % 10);
    } else {
        *out++ = '0' + val;
    }
    return out;
}

void ipv4_to_string(uint32_t ip, char* buf) {
    uint8_t a = (ip >> 24) & 0xFF;
    uint8_t b = (ip >> 16) & 0xFF;
    uint8_t c = (ip >> 8) & 0xFF;
    uint8_t d = ip & 0xFF;

    char* p = buf;
    p = u8_to_str(a, p); *p++ = '.';
    p = u8_to_str(b, p); *p++ = '.';
    p = u8_to_str(c, p); *p++ = '.';
    p = u8_to_str(d, p);
    *p = '\0';
}

bool ipv4_parse(const char* s, uint32_t* out) {
    if (!s || !out) return false;
    uint32_t ip = 0, v = 0;
    int oct = 0;
    const char* p = s;
    while (*p) {
        if (*p == '.') {
            if (v > 255 || oct >= 3) return false;
            ip = (ip << 8) | (v & 0xFF);
            v = 0; oct++;
        } else if (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            if (v > 255) return false;
        } else {
            return false;
        }
        ++p;
    }
    if (oct != 3 || v > 255) return false;
    ip = (ip << 8) | (v & 0xFF);
    *out = ip;
    return true;
}
