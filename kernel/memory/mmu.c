#include "mmu.h"
#include "console/serial/uart.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "exceptions/irq.h"
#include "hw/hw.h"
#include "dtb.h"
#include "pci.h"
#include "filesystem/disk.h"
#include "memory/page_allocator.h"
#include "sysregs.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01

#define UXN_BIT 54
#define PXN_BIT 53
#define AF_BIT 10
#define SH_BIT 8
#define AP_BIT 6
#define MAIR_BIT 2

#define PAGE_TABLE_ENTRIES 512

#define VIRT_TO_PHYS(x) ((VirtualAddr)x - HIGH_VA)
#define PHYS_TO_VIRT(x) ((PhysicalAddr)x + HIGH_VA)

uint64_t *kernel_lo_page, *kernel_hi_page;

static bool mmu_verbose;

void mmu_enable_verbose(){
    mmu_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (mmu_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

void mmu_map_2mb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index) {
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;

    kprintfv("[MMU] Mapping 2mb memory %x at [%i][%i][%i] for EL1", va, l0_index,l1_index,l2_index);

    if (!(table[l0_index] & 1)) {
        uint64_t* l1 = (uint64_t*)talloc(PAGE_SIZE);
        table[l0_index] = ((uint64_t)l1 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l1 = (uint64_t*)(table[l0_index] & 0xFFFFFFFFF000ULL);

    if (!(l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)talloc(PAGE_SIZE);
        l1[l1_index] = ((uint64_t)l2 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l2 = (uint64_t*)(l1[l1_index] & 0xFFFFFFFFF000ULL);   
    
    //For now we make this not executable. We'll need to to separate read_write, read_only and executable sections
    uint64_t attr = ((uint64_t)1 << UXN_BIT) | ((uint64_t)0 << PXN_BIT) | (1 << AF_BIT) | (0b11 << SH_BIT) | (0b00 << AP_BIT) | (attr_index << MAIR_BIT) | PD_BLOCK;
    l2[l2_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

//Level 0 = EL0, Level 1 = EL1, Level 2 = Shared
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level) {
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    if (!(table[l0_index] & 1)) {
        uint64_t* l1 = (uint64_t*)talloc(PAGE_SIZE);
        table[l0_index] = ((uint64_t)l1 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }
    
    uint64_t* l1 = (uint64_t*)(table[l0_index] & 0xFFFFFFFFF000ULL);
    if (!(l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)talloc(PAGE_SIZE);
        l1[l1_index] = ((uint64_t)l2 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }
    
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & 0xFFFFFFFFF000ULL);
    uint64_t l2_val = l2[l2_index];
    if (!(l2_val & 1)) {
        uint64_t* l3 = (uint64_t*)talloc(PAGE_SIZE);
        l2[l2_index] = ((uint64_t)l3 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    } else if ((l2_val & 0b11) == PD_BLOCK){
        kprintf("[MMU error]: Region not mapped for address %x, already mapped at higher granularity [%i][%i][%i][%i]",va, l0_index,l1_index,l2_index,l3_index);
        return;
    }
    
    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);
    
    if (l3[l3_index] & 1){
        kprintf("[MMU warning]: Section already mapped %x",va);
        return;
    }
    
    uint8_t permission = 0;
    
    //TODO: proper memory permissions, including accounting for WXN
    switch (level)
    {
    case MEM_PRIV_USER:   permission = 0b01; break;
    case MEM_PRIV_SHARED: permission = mem_attributes & MEM_EXEC ? 0b11 : 0b01; break;
    case MEM_PRIV_KERNEL: permission = 0b00; break;
    
    default:
        break;
    }
    //TODO:
    //Kernel always rw (0 << 1)
    //Kernel-only (0 << 0)
    //User rw (01)
    //User ro (11) - makes kernel ro too
    uint64_t attr = ((uint64_t)(level == MEM_PRIV_KERNEL) << UXN_BIT) | ((uint64_t)(level == MEM_PRIV_USER) << PXN_BIT) | (1 << AF_BIT) | (0b01 << SH_BIT) | (permission << AP_BIT) | (attr_index << MAIR_BIT) | 0b11;
    kprintfv("[MMU] Mapping 4kb memory %x at [%i][%i][%i][%i] for EL%i = %x | %x permission: %i", va, l0_index,l1_index,l2_index,l3_index,level,pa,attr,permission);
    
    l3[l3_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
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

void mmu_unmap(uint64_t va, uint64_t pa){

    uint64_t *table = kernel_lo_page;
    
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;
    
    kprintfv("[MMU] Unmapping 4kb memory %x at [%i][%i][%i][%i] for EL1", va, l0_index,l1_index,l2_index, l3_index);
    if (!(table[l0_index] & 1)) return;
    
    uint64_t* l1 = (uint64_t*)(table[l0_index] & 0xFFFFFFFFF000ULL);
    if (!(l1[l1_index] & 1)) return;
    
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & 0xFFFFFFFFF000ULL);
    uint64_t l3_val = l2[l2_index];
    if (!(l3_val & 1)) return;
    else if ((l3_val & 0b11) == PD_BLOCK){
        l2[l2_index] = 0;
        return;
    }
    
    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);

    l3[l3_index] = 0;

    mmu_flush_all();
    mmu_flush_icache();
}

void mmu_init_kernel(){
    kernel_lo_page = mmu_alloc();
    kernel_hi_page = mmu_alloc();
}

uint64_t *mmu_alloc(){
    return (uint64_t*)talloc(PAGE_SIZE);
}

extern uint64_t shared_start;
extern uint64_t shared_code_end;
extern uint64_t shared_ro_end;
extern uint64_t shared_end;

extern void mmu_start(uint64_t *low, uint64_t *high);

void mmu_init() {
    //TODO: Move these hardcoded mappings to their own file
    uint64_t kstart = mem_get_kmem_start();
    uint64_t kend = mem_get_kmem_end();

    for (uint64_t addr = kstart; addr < kend; addr += GRANULE_2MB){
        mmu_map_2mb(kernel_lo_page, addr, addr, MAIR_IDX_NORMAL);
        mmu_map_2mb(kernel_hi_page, HIGH_VA + addr, addr, MAIR_IDX_NORMAL);
    }

    for (uint64_t addr = get_uart_base(); addr <= get_uart_base(); addr += GRANULE_4KB){
        mmu_map_4kb(kernel_lo_page, addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
        mmu_map_4kb(kernel_hi_page, HIGH_VA + addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
    }

    for (uint64_t addr = GICD_BASE; addr <= GICC_BASE + 0x1000; addr += GRANULE_4KB){
        mmu_map_4kb(kernel_lo_page, addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
        mmu_map_4kb(kernel_hi_page, HIGH_VA + addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
    }

    if (XHCI_BASE)
        for (uint64_t addr = XHCI_BASE; addr <= XHCI_BASE + 0x1000; addr += GRANULE_4KB){
            mmu_map_4kb(kernel_lo_page, addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
            mmu_map_4kb(kernel_hi_page, HIGH_VA + addr, addr, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
        }

    uint64_t dstart;
    uint64_t dsize;
    if (dtb_addresses(&dstart,&dsize)){
        for (uint64_t addr = dstart; addr <= dstart + dsize; addr += GRANULE_4KB){
            mmu_map_4kb(kernel_lo_page, addr, addr, MAIR_IDX_NORMAL, MEM_RO, MEM_PRIV_KERNEL);
            mmu_map_4kb(kernel_hi_page, HIGH_VA + addr, addr, MAIR_IDX_NORMAL, MEM_RO, MEM_PRIV_KERNEL);
        }
    }
    
    mmu_start(kernel_lo_page, kernel_hi_page);

    kprintf("Finished MMU init");
}

void register_device_memory(uint64_t va, uint64_t pa){
    mmu_map_4kb(kernel_lo_page, va, pa, MAIR_IDX_DEVICE, MEM_RW, 1);
    mmu_flush_all();
    mmu_flush_icache();
}

void register_device_memory_2mb(uint64_t va, uint64_t pa){
    mmu_map_2mb(kernel_lo_page, va, pa, MAIR_IDX_DEVICE);
    mmu_flush_all();
    mmu_flush_icache();
}

void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level){
    mmu_map_4kb(kernel_lo_page, va, pa, MAIR_IDX_NORMAL, attributes, level);
    mmu_flush_all();
    mmu_flush_icache();
}

void debug_mmu_address(uint64_t va){

    uint64_t *table = kernel_lo_page;

    uint64_t l0_index = (va >> 37) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    kprintf("Address %x is meant to be mapped to [%i][%i][%i][%i]",va, l0_index,l1_index,l2_index,l3_index);

    if (!(table[l0_index] & 1)) {
        kprintf("L1 Table missing");
        return;
    }
    uint64_t* l1 = (uint64_t*)(table[l0_index] & 0xFFFFFFFFF000ULL);
    if (!(l1[l1_index] & 1)) {
        kprintf("L2 Table missing");
        return;
    }
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & 0xFFFFFFFFF000ULL);
    uint64_t l3_val = l2[l2_index];
    if (!(l3_val & 1)) {
        kprintf("L3 Table missing");
        return;
    }

    if (!((l3_val >> 1) & 1)){
        kprintf("Mapped as 2MB memory in L3");
        kprintf("Entry: %b", l3_val);
        return;
    }

    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);
    uint64_t l4_val = l3[l3_index];
    if (!(l4_val & 1)){
        kprintf("L4 Table entry missing");
        return;
    }
    kprintf("Entry: %b", l4_val);
    return;
}
