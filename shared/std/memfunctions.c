#include "memfunctions.h"

int memcmp(const void *s1, const void *s2, unsigned long count) {
    const unsigned char *a = s1;
    const unsigned char *b = s2;
    for (unsigned long i = 0; i < count; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

void* memset(void* dest, uint32_t val, size_t count) {
    uint64_t *d64 = (uint64_t *)dest;
    uint64_t pattern = ((uint64_t)val << 32) | val;

    uint64_t blocks = count / 32;
    for (uint64_t i = 0; i < blocks; i++) {
        d64[0] = pattern;
        d64[1] = pattern;
        d64[2] = pattern;
        d64[3] = pattern;
        d64 += 4;
    }

    uint64_t remaining = (count % 32) / 8;
    for (uint64_t i = 0; i < remaining; i++) d64[i] = pattern;

    uint8_t *d8 = (uint8_t *)(d64 + remaining);
    uint8_t byte_val = val & 0xFF;
    for (uint64_t i = 0; i < count % 8; i++) d8[i] = byte_val;

    return dest;
}

void* memcpy(void *dest, const void *src, uint64_t count) {
    uint8_t *d8 = (uint8_t *)dest;
    const uint8_t *s8 = (const uint8_t *)src;

    while (count > 0 && 
          (((uintptr_t)d8 & 7) != 0 || ((uintptr_t)s8 & 7) != 0)) {
        *d8++ = *s8++;
        count--;
    }

    uint64_t *d64 = (uint64_t *)d8;
    const uint64_t *s64 = (const uint64_t *)s8;

    uint64_t blocks = count / 32;
    for (uint64_t i = 0; i < blocks; i++) {
        d64[0] = s64[0];
        d64[1] = s64[1];
        d64[2] = s64[2];
        d64[3] = s64[3];
        d64 += 4;
        s64 += 4;
    }

    uint64_t remaining = (count % 32) / 8;
    for (uint64_t i = 0; i < remaining; i++) d64[i] = s64[i];

    d8 = (uint8_t *)(d64 + remaining);
    s8 = (const uint8_t *)(s64 + remaining);
    for (uint64_t i = 0; i < count % 8; i++) d8[i] = s8[i];

    return dest;
}