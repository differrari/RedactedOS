#include "proc_allocator.h"
#include "ram_e.h"
#include "gic.h"
#include "console/kio.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE
#define PTE_ATTR_DEVICE (PD_ACCESS | (MAIR_IDX_DEVICE << 2) | PD_TABLE)
#define PTE_ATTR_NORMAL (PD_ACCESS | (MAIR_IDX_NORMAL << 2) | PD_TABLE)

#define PAGE_TABLE_ENTRIES 512
#define PAGE_SIZE PAGE_TABLE_ENTRIES * 8

uint64_t mem_table_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

void proc_map_2mb(uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;

    printf("Proc Addresses for %h: %i %i %i", va, l1_index, l2_index, l3_index);

    if (!(mem_table_l1[l1_index] & 1)) {
        uint64_t* pud = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pud[i] = 0;
        mem_table_l1[l1_index] = ((uint64_t)pud & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* pud = (uint64_t*)(mem_table_l1[l1_index] & 0xFFFFFFFFF000ULL);

    if (!(pud[l2_index] & 1)) {
        uint64_t* pmd = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pmd[i] = 0;
        pud[l2_index] = ((uint64_t)pmd & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* pmd = (uint64_t*)(pud[l2_index] & 0xFFFFFFFFF000ULL);

    uint64_t attr = PD_ACCESS | (attr_index << 2) | PD_BLOCK;
    pmd[l3_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void proc_map_4kb(uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;

    printf("Proc Addresses for %h: %i %i %i %i", va, l1_index, l2_index, l3_index, l4_index);

    if (!(mem_table_l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l2[i] = 0;
        mem_table_l1[l1_index] = ((uint64_t)l2 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l2 = (uint64_t*)(mem_table_l1[l1_index] & 0xFFFFFFFFF000ULL);
    if (!(l2[l2_index] & 1)) {
        uint64_t* l3 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l3[i] = 0;
        l2[l2_index] = ((uint64_t)l3 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);
    if (!(l3[l3_index] & 1)) {
        uint64_t* l4 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l4[i] = 0;
        l3[l3_index] = ((uint64_t)l4 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l4 = (uint64_t*)(l3[l3_index] & 0xFFFFFFFFF000ULL);
    uint64_t attr = PD_ACCESS | (attr_index << 2) | 0b11;
    l4[l4_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void proc_allocator_init() {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        mem_table_l1[i] = 0;
    }
}
