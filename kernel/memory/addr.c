#include "addr.h"

#include "va_layout.h"

bool kva_is_dmap(kaddr_t va){
    uint64_t v = (uint64_t)va;
    return v >= HIGH_VA && v < KERNEL_IMAGE_VA_BASE;
}

kaddr_t dmap_pa_to_kva(paddr_t pa){
    if (!pa) return 0;
    return (kaddr_t)PHYS_TO_VIRT((uintptr_t)pa);
}

paddr_t dmap_kva_to_pa(kaddr_t va){
    if (!va) return 0;
    return (paddr_t)VIRT_TO_PHYS((uintptr_t)va);
}

void* pt_pa_to_va(paddr_t pa){
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) return (void*)(uintptr_t)pa;
    return (void*)(uintptr_t)dmap_pa_to_kva(pa);
}

paddr_t pt_va_to_pa(const void* va){
    uintptr_t v = (uintptr_t)va;
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) return (paddr_t)v;
    if (v >= KERNEL_IMAGE_VA_BASE) return kimg_va_to_pa((kaddr_t)v);
    return dmap_kva_to_pa((kaddr_t)v);
}
