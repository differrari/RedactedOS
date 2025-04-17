#include "mmio.h"
#include "types.h"

uint8_t mmio_read8(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}

void mmio_write8(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

uint16_t mmio_read16(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}

void mmio_write16(uintptr_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

void mmio_write32(uintptr_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

uint64_t mmio_read64(uintptr_t addr) {
    return *(volatile uint64_t*)addr;
}

void mmio_write64(uintptr_t addr, uint64_t value) {
    *(volatile uint64_t*)addr = value;
}

void mmio_write(uint64_t addr, uint64_t value) {
    mmio_write64(addr, value);
}

uint64_t mmio_read(uint64_t addr) {
    return mmio_read64(addr);
}