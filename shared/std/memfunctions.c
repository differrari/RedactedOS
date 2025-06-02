#include "memfunctions.h"

int memcmp(const void *s1, const void *s2, unsigned long n) {
    const unsigned char *a = s1;
    const unsigned char *b = s2;
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

void *memset(void *dest, int val, unsigned long count) {
    unsigned char *ptr = dest;
    while (count--) {
        *ptr++ = (unsigned char)val;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, uint64_t n) {
    uint64_t *d64 = (uint64_t *)dest;
    const uint64_t *s64 = (const uint64_t *)src;

    uint64_t blocks = n / 32;
    for (uint64_t i = 0; i < blocks; i++) {
        d64[0] = s64[0];
        d64[1] = s64[1];
        d64[2] = s64[2];
        d64[3] = s64[3];
        d64 += 4;
        s64 += 4;
    }

    uint64_t remaining = (n % 32) / 8;
    for (uint64_t i = 0; i < remaining; i++) {
        d64[i] = s64[i];
    }

    uint8_t *d8 = (uint8_t *)(d64 + remaining);
    const uint8_t *s8 = (const uint8_t *)(s64 + remaining);
    for (uint64_t i = 0; i < n % 8; i++) d8[i] = s8[i];

    return dest;
}