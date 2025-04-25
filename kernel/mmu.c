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

#define PAGE_TABLE_ENTRIES 512
#define PAGE_SIZE PAGE_TABLE_ENTRIES * 8

#define GRANULE_4KB 0x1000
#define GRANULE_2MB 0x200000

uint64_t page_table_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static bool mmu_verbose;

void mmu_map_2mb(uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l1_index = (va >> 37) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;

    if (mmu_verbose)
        printf("Mapping 2mb memory %h at [%i][%i][%i] for EL1", va, l1_index,l2_index,l3_index);

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
    
    //For now we make this not executable. We'll need to to separate read_write, read_only and executable sections
    uint64_t attr = (1 << 54) | (0 << 53) | PD_ACCESS | (0b11 << 8) | (0b00 << 6) | (attr_index << 2) | PD_BLOCK;
    l3[l3_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void mmu_map_4kb(uint64_t va, uint64_t pa, uint64_t attr_index, int level) {
    uint64_t l1_index = (va >> 37) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;

    if (mmu_verbose)
    printf("Mapping 4kb memory %h at [%i][%i][%i][%i] for EL%i", va, l1_index,l2_index,l3_index,l4_index,level);

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
    uint64_t l3_val = l3[l3_index];
    if (!(l3_val & 1)) {
        uint64_t* l4 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l4[i] = 0;
        l3[l3_index] = ((uint64_t)l4 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    } else if ((l3_val & 0b11) == PD_BLOCK){
        printf("[WARNING]: Region not mapped for address %h, already mapped at higher granularity [%i][%i][%i][%i]",va, l1_index,l2_index,l3_index,l4_index);
        return;
    }

    uint64_t* l4 = (uint64_t*)(l3[l3_index] & 0xFFFFFFFFF000ULL);
    //54 = UXN level | 53 = PXN !level | 8 = share | 6 = Access permission
    uint8_t permission = level == 1 ? 0b00 : 0b01;
    uint64_t attr = (level << 54) | (!level << 53) | PD_ACCESS | (0b11 << 8) | (permission << 6) | (attr_index << 2) | 0b11;
    
    l4[l4_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void mmu_init() {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        page_table_l1[i] = 0;
    }

    uint64_t start = mem_get_kmem_start();
    uint64_t end = mem_get_kmem_end();
    for (uint64_t addr = start; addr < end; addr += GRANULE_2MB)
        mmu_map_2mb(addr, addr, MAIR_IDX_NORMAL);

    for (uint64_t addr = UART0_BASE; addr < UART0_BASE + GRANULE_4KB; addr += GRANULE_4KB)
        mmu_map_4kb(addr, addr, MAIR_IDX_DEVICE, 1);

    for (uint64_t addr = GICD_BASE; addr < GICD_BASE + 0x12000; addr += GRANULE_4KB)
        mmu_map_4kb(addr, addr, MAIR_IDX_DEVICE, 1);

    uint64_t mair = (MAIR_DEVICE_nGnRnE << (MAIR_IDX_DEVICE * 8)) | (MAIR_NORMAL_NOCACHE << (MAIR_IDX_NORMAL * 8));
    asm volatile ("msr mair_el1, %0" :: "r"(mair));

    //30 = Translation granule EL1. 10 = 4kb | 14 = TG EL0 00 = 4kb
    uint64_t tcr = ((64 - 48) << 0) | ((64 - 48) << 16) | (0b00 << 14) | (0b10 << 30);
    asm volatile ("msr tcr_el1, %0" :: "r"(tcr));

    asm volatile ("dsb ish");
    asm volatile ("isb");

    asm volatile ("msr ttbr0_el1, %0" :: "r"(page_table_l1));
    
    asm volatile (
        "mrs x0, sctlr_el1\n"
        "orr x0, x0, #0x1\n"
        "bic x0, x0, #(1 << 19)\n"
        "msr sctlr_el1, x0\n"
        "isb\n"
        ::: "x0", "memory"
    );
    uint64_t sctlr;
    asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

    printf("Finished MMU init");
}

void mmu_enable_verbose(){
    mmu_verbose = true;
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
    mmu_map_4kb(va, pa, MAIR_IDX_NORMAL, 0);
    mmu_flush_all();
    mmu_flush_icache();
}

void debug_mmu_address(uint64_t va){
    uint64_t l1_index = (va >> 37) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;

    printf("Address is meant to be mapped to [%i][%i][%i][%i]",l1_index,l2_index,l3_index,l4_index);

    if (!(page_table_l1[l1_index] & 1)) {
        printf("L1 Table missing");
        return;
    }
    uint64_t* l2 = (uint64_t*)(page_table_l1[l1_index] & 0xFFFFFFFFF000ULL);
    if (!(l2[l2_index] & 1)) {
        printf("L2 Table missing");
        return;
    }
    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);
    uint64_t l3_val = l3[l3_index];
    if (!(l3_val & 1)) {
        printf("L3 Table missing");
        return;
    }

    if (!((l3_val >> 1) & 1)){
        printf("Mapped as 2MB memory in L3");
        printf("Entry: %h", l3_val);
        return;
    }

    uint64_t* l4 = (uint64_t*)(l3[l3_index] & 0xFFFFFFFFF000ULL);
    uint64_t l4_val = l4[l4_index];
    if (!(l4_val & 1)){
        printf("L4 Table entry missing");
        return;
    }
    printf("Entry: %h", l4_val);
    return;
}