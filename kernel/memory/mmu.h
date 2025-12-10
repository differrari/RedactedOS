#pragma once

#include "types.h"

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

uint64_t* mmu_alloc();
void mmu_init();
#ifdef __cplusplus
extern "C" {
#endif
void mmu_map_kernel(uintptr_t *ttbr);
uintptr_t* mmu_new_ttbr();
void register_device_memory(uint64_t va, uint64_t pa);
void register_device_memory_2mb(uint64_t va, uint64_t pa);
void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level);
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level);
void debug_mmu_address(uint64_t va);
void mmu_enable_verbose();
void mmu_swap_ttbr(uintptr_t* ttbr);
uintptr_t* mmu_default_ttbr();
void mmu_free_ttbr(uintptr_t *ttbr);
uintptr_t mmu_translate(uintptr_t va);
void mmu_map_all(uintptr_t pa);
#ifdef __cplusplus
}
#endif

extern uintptr_t *pttbr;

void mmu_unmap(uint64_t va, uint64_t pa);
void mmu_init_kernel();