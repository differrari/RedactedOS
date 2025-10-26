#pragma once

#include "types.h"

#define read8(addr) (*(volatile uint8_t*)(addr))
#define write8(addr, value) (*(volatile uint8_t*)(addr) = (value))

#define read16(addr) (*(volatile uint16_t*)(addr))
#define write16(addr, value) (*(volatile uint16_t*)(addr) = (value))

#define read32(addr) (*(volatile uint32_t*)(addr))
#define write32(addr, value) (*(volatile uint32_t*)(addr) = (value))

#define read64(addr) (*(volatile uint64_t*)(addr))
#define write64(addr, value) (*(volatile uint64_t*)(addr) = (value))

#ifdef __cplusplus
extern "C" {
#endif

uint16_t read_unaligned16(const uint16_t *p);
uint32_t read_unaligned32(const void *p);
uint64_t read_unaligned64(const uint64_t *p);
void write_unaligned32(uint32_t *p, uint32_t value);

#ifdef __cplusplus
}
#endif