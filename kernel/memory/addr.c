#include "addr.h"

bool kva_is_dmap(uintptr_t va){
    return (va & HIGH_VA) == HIGH_VA;
}

uintptr_t dmap_pa_to_kva(paddr_t pa){
    if (!pa) return 0;
    return (uintptr_t)PHYS_TO_VIRT((uintptr_t)pa);
}

paddr_t dmap_kva_to_pa(uintptr_t va){
    if (!va) return 0;
    return (paddr_t)VIRT_TO_PHYS(va);
}

void* pt_pa_to_va(paddr_t pa){
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) return (void*)(uintptr_t)pa;
    return (void*)dmap_pa_to_kva(pa);
}

paddr_t pt_va_to_pa(const void* va){
    uintptr_t v = (uintptr_t)va;
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) return (paddr_t)v;
    return dmap_kva_to_pa(v);
}
