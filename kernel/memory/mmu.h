#pragma once

#include "types.h"

typedef struct mm_struct mm_struct;

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

#define MMU_MAP_EXEC 0x01

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_AF (1ULL << 10)
#define PTE_NG (1ULL << 11)
#define PTE_SH_SHIFT 8
#define PTE_AP_SHIFT 6
#define PTE_ATTR_SHIFT 2

#define PAGE_TABLE_ENTRIES 512
#define PD_TABLE 0b11
#define PD_BLOCK 0b01

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
void register_device_memory(kaddr_t va, paddr_t pa);
void register_device_memory_dmap(kaddr_t va);
void register_device_memory_2mb(kaddr_t va, paddr_t pa);
void register_proc_memory(uint64_t va, paddr_t pa, uint8_t attributes, uint8_t level);
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level);
void mmu_unmap_table(uint64_t *table, uint64_t va, uint64_t pa);
void debug_mmu_address(uint64_t va);
void mmu_enable_verbose();
void mmu_swap_ttbr(mm_struct *mm);
void mmu_ttbr0_disable_user();
void mmu_ttbr0_enable_user();
bool mmu_ttbr0_user_enabled();
void mmu_flush_asid(uint16_t asid);
void mmu_asid_ensure(mm_struct *mm);
void mmu_asid_release(mm_struct *mm);
bool mmu_unmap_and_get_pa(uint64_t *table, uint64_t va, uint64_t *pa);
bool mmu_set_access_flag(uint64_t *table, uint64_t va);
uintptr_t* mmu_default_ttbr();
void mmu_free_ttbr(uintptr_t *ttbr);
uintptr_t mmu_translate(uint64_t *root, uintptr_t va, int *status);
void mmu_map_all(paddr_t pa);
#ifdef __cplusplus
}
#endif


void mmu_unmap(uint64_t va, uint64_t pa);
void mmu_init_kernel();