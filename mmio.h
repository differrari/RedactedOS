#ifndef MMIO_H
#define MMIO_H

#include "types.h"

uint8_t mmio_read8(uintptr_t addr);
void mmio_write8(uintptr_t addr, uint8_t value);
uint16_t mmio_read16(uintptr_t addr);
void mmio_write16(uintptr_t addr, uint16_t value);
uint32_t mmio_read32(uintptr_t addr);
void mmio_write32(uintptr_t addr, uint32_t value);
uint64_t mmio_read64(uintptr_t addr);
void mmio_write64(uintptr_t addr, uint64_t value);
void mmio_write(uint64_t addr, uint64_t value);
uint64_t mmio_read(uint64_t addr);
void mmio_write_barrier();

#endif