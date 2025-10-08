#pragma once

#include "types.h"

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

uint64_t* mmu_alloc();
void mmu_init();
#ifdef __cplusplus
extern "C" {
#endif
void register_device_memory(uint64_t va, uint64_t pa);
void register_device_memory_2mb(uint64_t va, uint64_t pa);
void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level);
void debug_mmu_address(uint64_t va);
void mmu_enable_verbose();
#ifdef __cplusplus
}
#endif

void mmu_unmap(uint64_t va, uint64_t pa);
void mmu_init_kernel();