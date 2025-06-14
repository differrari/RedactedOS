#pragma once

#include "types.h"

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

void mmu_alloc();
void mmu_init();
void register_device_memory(uint64_t va, uint64_t pa);
void register_device_memory_2mb(uint64_t va, uint64_t pa);
void register_proc_memory(uint64_t va, uint64_t pa, bool kernel);
void debug_mmu_address(uint64_t va);
void mmu_enable_verbose();

void mmu_unmap(uint64_t va, uint64_t pa);