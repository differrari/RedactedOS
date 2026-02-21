#pragma once

#include "types.h"

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

uint64_t* mmu_alloc();
void mmu_init();
#ifdef __cplusplus
extern "C" {
#endif

#define MMU_TR_OK 0
#define MMU_TR_ERR_PARAM 1
#define MMU_TR_ERR_L1 2
#define MMU_TR_ERR_L2 3
#define MMU_TR_ERR_L3 4
#define MMU_TR_ERR_L4 5

void mmu_map_kernel(uintptr_t *ttbr);
uintptr_t* mmu_new_ttbr();
void register_device_memory(uint64_t va, uint64_t pa);
void register_device_memory_2mb(uint64_t va, uint64_t pa);
void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level);
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level);
void mmu_unmap_table(uint64_t *table, uint64_t va, uint64_t pa);
void debug_mmu_address(uint64_t va);
void mmu_enable_verbose();
void mmu_swap_ttbr(uintptr_t* ttbr, uint16_t asid);
void mmu_flush_asid(uint16_t asid);
void mmu_asid_ensure(uint16_t *asid, uint32_t *asid_generation);
bool mmu_unmap_and_get_pa(uint64_t *table, uint64_t va, uint64_t *pa);
uintptr_t* mmu_default_ttbr();
void mmu_free_ttbr(uintptr_t *ttbr);
uintptr_t mmu_translate(uintptr_t va, int *status);
void mmu_map_all(uintptr_t pa);
#ifdef __cplusplus
}
#endif

extern uintptr_t *pttbr;

void mmu_unmap(uint64_t va, uint64_t pa);
void mmu_init_kernel();