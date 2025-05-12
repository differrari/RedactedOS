#include "proc_allocator.h"
#include "ram_e.h"
#include "interrupts/gic.h"
#include "console/kio.h"
#include "mmu.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 512
#define PAGE_SIZE 4096

uint64_t mem_table_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

void proc_map_2mb(uint64_t va, uint64_t pa) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;

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

    uint64_t attr = PD_ACCESS | (1 << 2) | PD_BLOCK;
    pmd[l3_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void proc_map_4kb(uint64_t va, uint64_t pa) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;

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
    uint64_t attr = PD_ACCESS | (1 << 2) | 0b11;
    l4[l4_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void proc_allocator_init() {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        mem_table_l1[i] = 0;
    }
}

void* alloc_proc_mem(uint64_t size, bool kernel) {
    uint64_t start = get_user_ram_start();
    uint64_t end = get_user_ram_end();

    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end = end & ~(PAGE_SIZE - 1);

    size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    for (uint64_t va = start; va + size <= end; va += PAGE_SIZE) {
        bool free = true;
        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            uint64_t v = va + offset;
            uint64_t l1 = (v >> 39) & 0x1FF;
            uint64_t l2 = (v >> 30) & 0x1FF;
            uint64_t l3 = (v >> 21) & 0x1FF;
            uint64_t l4 = (v >> 12) & 0x1FF;

            if (!(mem_table_l1[l1] & 1)) continue;
            uint64_t* l2t = (uint64_t*)(mem_table_l1[l1] & ~0xFFF);
            if (!(l2t[l2] & 1)) continue;
            uint64_t* l3t = (uint64_t*)(l2t[l2] & ~0xFFF);
            if (!(l3t[l3] & 1)) continue;
            uint64_t* l4t = (uint64_t*)(l3t[l3] & ~0xFFF);
            if (l4t[l4] & 1) {
                free = false;
                break;
            }
        }
        if (free) {
            for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE){
                proc_map_4kb(va + offset, va + offset);
                register_proc_memory(va + offset, va + offset, kernel);
            }
            return (void*)va;
        }
    }
    return 0;
}
