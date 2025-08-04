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

uint16_t read_unaligned16(const uint16_t *p);
uint32_t read_unaligned32(const uint32_t *p);
uint64_t read_unaligned64(const uint64_t *p);
void write_unaligned32(uint32_t *p, uint32_t value);

#ifdef __cplusplus
}
#endif