#include "memory_access.h"

uint16_t read_unaligned16(const uint16_t *up) {
    uint8_t *p = (uint8_t*)up;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t read_unaligned32(const void *up) {
    uint8_t *p = (uint8_t*)up;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t read_unaligned64(const uint64_t *up) {
    uint8_t *p = (uint8_t*)up;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

void write_unaligned32(uint32_t *up, uint32_t value) {
    uint8_t *p = (uint8_t*)up;
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((value >> 8) & 0xFF);
    p[2] = (uint8_t)((value >> 16) & 0xFF);
    p[3] = (uint8_t)((value >> 24) & 0xFF);
}