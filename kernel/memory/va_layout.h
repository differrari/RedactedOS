#pragma once

#include "types.h"

#include "memory/addr.h"

#define KERNEL_IMAGE_VA_BASE 0xFFFFC00000000000ULL

static inline paddr_t kimg_va_to_pa(kaddr_t va) {
    return (paddr_t)((uint64_t)va - KERNEL_IMAGE_VA_BASE);
}

static inline kaddr_t kimg_pa_to_va(paddr_t pa) {
    return (kaddr_t)((uint64_t)pa + KERNEL_IMAGE_VA_BASE);
}

static inline kaddr_t mmio_pa_to_kva(paddr_t pa) {
    return (kaddr_t)((uint64_t)pa | HIGH_VA);
}
