#pragma once

#include "types.h"

uint8_t read8(uintptr_t addr);
void write8(uintptr_t addr, uint8_t value);
uint16_t read16(uintptr_t addr);
void write16(uintptr_t addr, uint16_t value);
uint32_t read32(uintptr_t addr);
void write32(uintptr_t addr, uint32_t value);
uint64_t read64(uintptr_t addr);
void write64(uintptr_t addr, uint64_t value);
void write(uint64_t addr, uint64_t value);
uint64_t read(uint64_t addr);
void write_barrier();

#ifdef __cplusplus
extern "C" {
#endif

int memcmp(const void *s1, const void *s2, unsigned long n);
void *memset(void *dest, int val, unsigned long count);
void *memcpy(void *dest, const void *src, uint64_t n);

#ifdef __cplusplus
}
#endif