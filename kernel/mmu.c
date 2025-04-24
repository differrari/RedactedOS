#include "mmu.h"
#include "console/serial/uart.h"
#include "ram_e.h"
#include "console/kio.h"
#include "gic.h"

#define MAIR_DEVICE_nGnRnE 0b00000000
#define MAIR_NORMAL_NOCACHE 0b01000100
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE
#define PTE_ATTR_DEVICE (PD_ACCESS | (MAIR_IDX_DEVICE << 2) | PD_TABLE)
#define PTE_ATTR_NORMAL (PD_ACCESS | (MAIR_IDX_NORMAL << 2) | PD_TABLE)

#define PAGE_TABLE_ENTRIES 512
#define PAGE_SIZE PAGE_TABLE_ENTRIES * 8

uint64_t page_table_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

void mmu_map_2mb(uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;

    if (!(page_table_l1[l1_index] & 1)) {
        uint64_t* pud = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pud[i] = 0;
        page_table_l1[l1_index] = ((uint64_t)pud & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* pud = (uint64_t*)(page_table_l1[l1_index] & 0xFFFFFFFFF000ULL);

    if (!(pud[l2_index] & 1)) {
        uint64_t* pmd = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pmd[i] = 0;
        pud[l2_index] = ((uint64_t)pmd & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* pmd = (uint64_t*)(pud[l2_index] & 0xFFFFFFFFF000ULL);

    uint64_t attr = PD_ACCESS | (attr_index << 2) | PD_BLOCK;
    pmd[l3_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void mmu_map_4kb(uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;


    if (!(page_table_l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l2[i] = 0;
        page_table_l1[l1_index] = ((uint64_t)l2 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l2 = (uint64_t*)(page_table_l1[l1_index] & 0xFFFFFFFFF000ULL);
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

void mmu_init() {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        page_table_l1[i] = 0;
    }

    uint64_t start = mem_get_kmem_start();
    uint64_t end = mem_get_kmem_end();
    for (uint64_t addr = start; addr < end; addr += 0x200000)
        mmu_map_2mb(addr, addr, MAIR_IDX_NORMAL);

    for (uint64_t addr = UART0_BASE; addr < UART0_BASE + 0x1000; addr += 0x1000)
        mmu_map_4kb(addr, addr, MAIR_IDX_DEVICE);

    for (uint64_t addr = GICD_BASE; addr < GICD_BASE + 0x12000; addr += 0x1000)
        mmu_map_4kb(addr, addr, MAIR_IDX_DEVICE);

    uint64_t mair = (MAIR_DEVICE_nGnRnE << (MAIR_IDX_DEVICE * 8)) | (MAIR_NORMAL_NOCACHE << (MAIR_IDX_NORMAL * 8));
    asm volatile ("msr mair_el1, %0" :: "r"(mair));

    uint64_t tcr = ((64 - 48) << 0) | ((64 - 48) << 16) | (0b00 << 14) | (0b10 << 30);
    asm volatile ("msr tcr_el1, %0" :: "r"(tcr));

    asm volatile ("dsb ish");
    asm volatile ("isb");

    asm volatile ("msr ttbr0_el1, %0" :: "r"(page_table_l1));

    uint64_t sctlr;
    asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1;
    asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    asm volatile ("isb");

    printf("Finished MMU init");
}

static inline void mmu_flush_all() {
    asm volatile (
        "dsb ishst\n"        // Ensure all memory accesses complete
        "tlbi vmalle1is\n"   // Invalidate all EL1 TLB entries (Inner Shareable)
        "dsb ish\n"          // Ensure completion of TLB invalidation
        "isb\n"              // Synchronize pipeline
    );
}

static inline void mmu_flush_icache() {
    asm volatile (
        "ic iallu\n"         // Invalidate all instruction caches to PoU
        "isb\n"              // Ensure completion before continuing
    );
}

void register_proc_memory(uint64_t va, uint64_t pa){
    mmu_map_4kb(va, pa, MAIR_IDX_NORMAL);
    mmu_flush_all();
    mmu_flush_icache();
}