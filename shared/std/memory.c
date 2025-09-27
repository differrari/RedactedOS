#include "memory.h"

int memcmp(const void *s1, const void *s2, unsigned long count) {
    const unsigned char *a = s1;
    const unsigned char *b = s2;
    for (unsigned long i = 0; i < count; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

void* memset(void* dest, uint32_t val, size_t count) {
    uint8_t *d8 = (uint8_t *)dest;
    uint8_t byte = (uint8_t)(val & 0xFF);

    while (((uintptr_t)d8 & 7) && count > 0) {
        *d8++ = byte;
        count--;
    }

    if (count >= 8) {
        uint64_t pattern = 0;
        for (int i = 0; i < 8; i++) {
            pattern <<= 8;
            pattern |= byte;
        }
        size_t blocks = count / 8;
        for (size_t i = 0; i < blocks; i++) {
            *((uint64_t *)d8) = pattern;
            d8 += 8;
        }
        count %= 8;
    }

    if (count >= 4) {
        uint32_t pattern32 = byte;
        pattern32 |= pattern32 << 8;
        pattern32 |= pattern32 << 16;
        *((uint32_t *)d8) = pattern32;
        d8 += 4;
        count -= 4;
    }

    if (count >= 2) {
        uint16_t pattern16 = (uint16_t)byte | ((uint16_t)byte << 8);
        *((uint16_t *)d8) = pattern16;
        d8 += 2;
        count -= 2;
    }

    if (count == 1) {
        *d8 = byte;
    }

    return dest;
}

void* memcpy(void *dest, const void *src, uint64_t count) {
    uint8_t *d8 = (uint8_t *)dest;
    const uint8_t *s8 = (const uint8_t *)src;

    while (count > 0 && (((uintptr_t)d8 & 7) != 0 || ((uintptr_t)s8 & 7) != 0)) {
        *d8++ = *s8++;
        count--;
    }

    size_t blocks = count / 8;
    for (size_t i = 0; i < blocks; i++) {
        *((uint64_t *)d8) = *((uint64_t *)s8);
        d8 += 8;
        s8 += 8;
    }

    size_t remaining = count % 8;
    if (remaining >= 4) {
        *((uint32_t *)d8) = *((uint32_t *)s8);
        d8 += 4;
        s8 += 4;
        remaining -= 4;
    }
    if (remaining >= 2) {
        *((uint16_t *)d8) = *((uint16_t *)s8);
        d8 += 2;
        s8 += 2;
        remaining -= 2;
    }
    if (remaining >= 1)
        *d8 = *s8 & 0xFF;

    return dest;
}