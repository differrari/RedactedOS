#include "memory_access.h"

uint8_t read8(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}

void write8(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

uint16_t read16(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}

void write16(uintptr_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

uint32_t read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

void write32(uintptr_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

uint64_t read64(uintptr_t addr) {
    return *(volatile uint64_t*)addr;
}

void write64(uintptr_t addr, uint64_t value) {
    *(volatile uint64_t*)addr = value;
}

void write(uint64_t addr, uint64_t value) {
    write64(addr, value);
}

uint64_t read(uint64_t addr) {
    return read64(addr);
}

uint16_t read_unaligned16(const uint16_t *up) {
    uint8_t *p = (uint8_t*)up;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t read_unaligned32(const uint32_t *up) {
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