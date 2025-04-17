#include "mmio.h"
#include "types.h"

void mmio_write(uint64_t reg, uint64_t data) {
    *(volatile uint32_t *)(uintptr_t)reg = data;
}

uint64_t mmio_read(uint64_t reg) {
    return *(volatile uint32_t *)(uintptr_t)reg;
}

void mmio_write_barrier() {
    asm volatile("dsb sy" ::: "memory");
}