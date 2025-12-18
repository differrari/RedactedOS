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
#include "process/process.h"
#include "process/scheduler.h"
#include "sysregs.h"
#include "std/memory.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01

#define UXN_BIT 54
#define PXN_BIT 53
#define AF_BIT 10
#define SH_BIT 8
#define AP_BIT 6
#define MAIR_BIT 2

#define PAGE_TABLE_ENTRIES 512

#define ADDR_MASK 0xFFFFFFFFF000ULL

uint64_t *kernel_mmu_page;

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
        table[l0_index] = ((uint64_t)l1 & ADDR_MASK) | PD_TABLE;
    }

    uint64_t* l1 = (uint64_t*)(table[l0_index] & ADDR_MASK);

    if (!(l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)talloc(PAGE_SIZE);
        l1[l1_index] = ((uint64_t)l2 & ADDR_MASK) | PD_TABLE;
    }

    uint64_t* l2 = (uint64_t*)(l1[l1_index] & ADDR_MASK);   
    
    //For now we make this not executable. We'll need to to separate read_write, read_only and executable sections
    uint64_t attr = ((uint64_t)1 << UXN_BIT) | ((uint64_t)0 << PXN_BIT) | (1 << AF_BIT) | (0b11 << SH_BIT) | (0b00 << AP_BIT) | (attr_index << MAIR_BIT) | PD_BLOCK;
    l2[l2_index] = (pa & ADDR_MASK) | attr;
}

//Level 0 = EL0, Level 1 = EL1, Level 2 = Shared
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level) {
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    if (!(table[l0_index] & 1)) {
        uint64_t* l1 = (uint64_t*)talloc(PAGE_SIZE);
        table[l0_index] = ((uint64_t)l1 & ADDR_MASK) | PD_TABLE;
    }
    
    uint64_t* l1 = (uint64_t*)(table[l0_index] & ADDR_MASK);
    if (!(l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)talloc(PAGE_SIZE);
        l1[l1_index] = ((uint64_t)l2 & ADDR_MASK) | PD_TABLE;
    }
    
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & ADDR_MASK);
    uint64_t l2_val = l2[l2_index];
    if (!(l2_val & 1)) {
        uint64_t* l3 = (uint64_t*)talloc(PAGE_SIZE);
        l2[l2_index] = ((uint64_t)l3 & ADDR_MASK) | PD_TABLE;
    } else if ((l2_val & 0b11) == PD_BLOCK){
        uart_puts("[MMU error]: Region not mapped for address ");
        uart_puthex(va);
        uart_puts("already mapped at higher granularity\n");
        return;
    }
    
    uint64_t* l3 = (uint64_t*)(l2[l2_index] & ADDR_MASK);
    
    if (l3[l3_index] & 1){
        uart_puts("[MMU warning]: Section already mapped ");
        uart_puthex(va);
        uart_puts(" ");
        uart_puthex((uintptr_t)table);
        uart_putc('\n');
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
    
    l3[l3_index] = (pa & ADDR_MASK) | attr;
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

uintptr_t* mmu_default_ttbr(){
    return kernel_mmu_page;
}

void mmu_unmap_table(uintptr_t *table, uintptr_t va, uintptr_t pa){
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;
    
    kprintfv("[MMU] Unmapping 4kb memory %x at [%i][%i][%i][%i] for EL1", va, l0_index,l1_index,l2_index, l3_index);
    if (!(table[l0_index] & 1)) return;
    
    uint64_t* l1 = (uint64_t*)(table[l0_index] & ADDR_MASK);
    if (!(l1[l1_index] & 1)) return;
    
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & ADDR_MASK);
    uint64_t l3_val = l2[l2_index];
    if (!(l3_val & 1)) return;
    else if ((l3_val & 0b11) == PD_BLOCK){
        l2[l2_index] = 0;
        return;
    }
    
    uint64_t* l3 = (uint64_t*)(l2[l2_index] & ADDR_MASK);

    l3[l3_index] = 0;

    mmu_flush_all();
    mmu_flush_icache();
}

void mmu_unmap(uint64_t va, uint64_t pa){
    mmu_unmap_table(kernel_mmu_page, va, pa);
    mmu_unmap_table(pttbr, va, pa);
}

uint64_t *mmu_alloc(){
    return (uint64_t*)talloc(PAGE_SIZE);
}

extern uintptr_t cpec;
extern uintptr_t ksp;

extern void mmu_start(uint64_t *mmu);

uintptr_t heap_end;

void mmu_init() {
    kernel_mmu_page = mmu_alloc();
    uintptr_t kstart = mem_get_kmem_start();
    uintptr_t kend = mem_get_kmem_end();
    uintptr_t heapstart = get_user_ram_start();

    for (uint64_t addr = kstart; addr < kend; addr += GRANULE_2MB)
        mmu_map_2mb(kernel_mmu_page, addr, addr, MAIR_IDX_NORMAL);
    
    for (uint64_t addr = heapstart; addr < heap_end; addr += GRANULE_4KB)
        mmu_map_4kb(kernel_mmu_page, addr, addr, MAIR_IDX_NORMAL, MEM_DEV | MEM_RW, MEM_PRIV_KERNEL);
    
    mmu_map_2mb(kernel_mmu_page, (uintptr_t)kernel_mmu_page, (uintptr_t)kernel_mmu_page, MAIR_IDX_DEVICE);

    uint64_t dstart;
    uint64_t dsize;
    if (dtb_addresses(&dstart,&dsize))
        for (uint64_t addr = dstart; addr <= dstart + dsize; addr += GRANULE_4KB)
            mmu_map_4kb(kernel_mmu_page, addr, addr, MAIR_IDX_NORMAL, MEM_RO, MEM_PRIV_KERNEL);

    hw_high_va();

    mmu_start(kernel_mmu_page);

    // kprintf("Finished MMU init");
}

void mmu_copy(uintptr_t *new_ttbr, uintptr_t *old_ttbr, int level){
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++){
        if (old_ttbr[i] & 1){
            if (level == 3 || (level == 2 && ((old_ttbr[i] & 0b11) == PD_BLOCK))){
                new_ttbr[i] = old_ttbr[i];
            } else {
                uintptr_t *old_entry = (uintptr_t*)(old_ttbr[i] & ADDR_MASK);
                uintptr_t *new_entry = mmu_alloc();
                if (!old_entry || !new_entry) continue;
                uint64_t entry = old_ttbr[i] & ~(ADDR_MASK);
                new_ttbr[i] = entry | ((uintptr_t)new_entry & ADDR_MASK);
                mmu_copy(new_entry, old_entry, level+1);
            }
        }
    }
}

void mmu_free_ttbr_l(uintptr_t *ttbr, int level){
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++){
        if (ttbr[i] & 1){
            if (level == 3 || (level == 2 && ((ttbr[i] & 0b11) == PD_BLOCK))) continue;
            uintptr_t *entry = (uintptr_t*)(ttbr[i] & ADDR_MASK);
            mmu_free_ttbr_l(entry, level+1);
        }
    }
    temp_free(ttbr, PAGE_SIZE);
}

void mmu_map_all(uintptr_t pa){
    process_t *processes = get_all_processes();
    for (int i = 0; i < MAX_PROCS; i++){
        if (processes[i].state != STOPPED && processes[i].ttbr){
            mmu_map_2mb(processes[i].ttbr, pa, pa, MAIR_IDX_DEVICE);
        }
    }
    mmu_flush_all();
    mmu_flush_icache();
}

void mmu_free_ttbr(uintptr_t *ttbr){
    mmu_free_ttbr_l(ttbr, 0);
}

uintptr_t* mmu_new_ttbr(){
    uintptr_t *ttbr = mmu_alloc();
    mmu_copy(ttbr, kernel_mmu_page,0);
    return ttbr;
}

void register_device_memory(uint64_t va, uint64_t pa){
    if (pttbr && pttbr != kernel_mmu_page)//TODO: This won't be necessary once kernel is exclusively in ttbr1
        mmu_map_4kb(pttbr, va, pa, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
    mmu_map_4kb(kernel_mmu_page, va, pa, MAIR_IDX_DEVICE, MEM_RW, MEM_PRIV_KERNEL);
    mmu_flush_all();
    mmu_flush_icache();
}

void register_device_memory_2mb(uint64_t va, uint64_t pa){
    if (pttbr && pttbr != kernel_mmu_page)//TODO: This won't be necessary once kernel is exclusively in ttbr1
        mmu_map_2mb(pttbr, va, pa, MAIR_IDX_DEVICE);
    mmu_map_2mb(kernel_mmu_page, va, pa, MAIR_IDX_DEVICE);
    mmu_flush_all();
    mmu_flush_icache();
}

void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level){
    if (pttbr && pttbr != kernel_mmu_page)
        mmu_map_4kb(pttbr, va, pa, MAIR_IDX_NORMAL, attributes, level);
    mmu_map_4kb(kernel_mmu_page, va, pa, MAIR_IDX_NORMAL, attributes, level);
    mmu_flush_all();
    mmu_flush_icache();
}

uintptr_t mmu_translate(uintptr_t va){
    uint64_t *table = pttbr && (va >> 48) == 0 ? pttbr : kernel_mmu_page;

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    if (!(table[l0_index] & 1)) {
        kprintfv("L1 Table missing");
        return 0;
    }
    uint64_t* l1 = (uint64_t*)(table[l0_index] & ADDR_MASK);
    if (!(l1[l1_index] & 1)) {
        kprintfv("L2 Table missing");
        return 0;
    }
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & ADDR_MASK);
    uint64_t l3_val = l2[l2_index];
    if (!(l3_val & 1)) {
        kprintfv("L3 Table missing");
        return 0;
    }

    if (!((l3_val >> 1) & 1)){
        return l3_val & ADDR_MASK;
    }

    uint64_t* l3 = (uint64_t*)(l2[l2_index] & ADDR_MASK);
    uint64_t l4_val = l3[l3_index];
    if (!(l4_val & 1)){
        kprintfv("L4 Table entry missing");
        return 0;
    }
    return l4_val & ADDR_MASK;
}

void debug_mmu_address(uint64_t va){

    uint64_t *table = pttbr ? pttbr : kernel_mmu_page;

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    kprintf("Address %llx is meant to be mapped to [%i][%i][%i][%i]",va, l0_index,l1_index,l2_index,l3_index);

    if (!(table[l0_index] & 1)) {
        kprintf("L1 Table missing");
        return;
    }
    uint64_t* l1 = (uint64_t*)(table[l0_index] & ADDR_MASK);
    if (!(l1[l1_index] & 1)) {
        kprintf("L2 Table missing");
        return;
    }
    uint64_t* l2 = (uint64_t*)(l1[l1_index] & ADDR_MASK);
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

    uint64_t* l3 = (uint64_t*)(l2[l2_index] & ADDR_MASK);
    uint64_t l4_val = l3[l3_index];
    if (!(l4_val & 1)){
        kprintf("L4 Table entry missing");
        return;
    }
    kprintf("Entry: %b", l4_val);
    return;
}

extern void mmu_swap(uintptr_t* ttbr);

uintptr_t *pttbr;

void mmu_swap_ttbr(uintptr_t* ttbr){
    pttbr = ttbr ? ttbr : kernel_mmu_page;
}
