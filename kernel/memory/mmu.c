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
#include "memory/addr.h"
#include "std/memory.h"
#include "exceptions/exception_handler.h"
#include "alloc/mem_types.h"
#include "memory/talloc.h"
#include "memory/va_layout.h"

#define PAGE_TABLE_ENTRIES 512
#define PD_TABLE 0b11
#define PD_BLOCK 0b01

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_AF (1ULL << 10)
#define PTE_NG (1ULL << 11)
#define PTE_SH_SHIFT 8
#define PTE_AP_SHIFT 6
#define PTE_ATTR_SHIFT 2
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
extern char kcode_end;

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __vectors_start[];
extern char __vectors_end[];
extern char __kstack_bottom[];
extern char __kstack_top[];

static uintptr_t *kernel_ttbr0;
static uintptr_t *kernel_ttbr1;

static bool mmu_verbose;
static inline void mmu_flush_icache();
static inline void mmu_flush_all();


static uint64_t asid_shift;
static uint16_t asid_mask;

static uint32_t asid_gen;
static uint32_t asid_max;
static uint32_t asid_next;
static uint64_t asid_used[65536/64];

uint64_t pttbr_hw;
uint16_t pttbr_asid;
uintptr_t *pttbr;

static inline uint64_t make_pte(uint64_t pa, uint64_t attr_index, uint8_t mem_attr, uint8_t level, uint64_t type) {
    uint64_t sh = (mem_attr & MEM_DEV) ? 0 : 0b11;
    uint64_t ap = 0;

    if (level == MEM_PRIV_KERNEL) ap = (mem_attr & MEM_RW) ? 0 : 0b10;
    else ap = (mem_attr & MEM_RW) ? 1 : 0b11;

    uint64_t attr = PTE_AF | (sh << PTE_SH_SHIFT) | (ap << PTE_AP_SHIFT) | (attr_index << PTE_ATTR_SHIFT);

    if (level == MEM_PRIV_KERNEL) {
        attr |= PTE_UXN;
        if (!(mem_attr & MEM_EXEC)) attr |= PTE_PXN;
    } else if (level == MEM_PRIV_USER) {
        attr |= PTE_NG;
        attr |= PTE_PXN;
        if (!(mem_attr & MEM_EXEC)) attr |= PTE_UXN;
    } else if (level == MEM_PRIV_SHARED) {
        attr |= PTE_NG;
        attr |= PTE_PXN;
        if (!(mem_attr & MEM_EXEC)) attr |= PTE_UXN;
    }

    return (pa & PTE_ADDR_MASK) | attr | type;
}

static uint64_t *walk_or_alloc(uint64_t *table, uint64_t index, int level, uint64_t va) {
    uint64_t e = table[index];

    if (!(e & 1)){
        uint64_t *n = mmu_alloc();
        table[index] = (pt_va_to_pa(n) & PTE_ADDR_MASK) | PD_TABLE;
        return n;
    }

    if ((e & 0b11) != PD_TABLE){
        kprintf("[mmu] *walk_or_alloc bad type l=%d va=%llx idx=%llu e=%llx", level, (uint64_t)va, (uint64_t)index, (uint64_t)e);
        panic("mmu *walk_or_alloc bad type", va);
    }

    return (uint64_t*)pt_pa_to_va(e & PTE_ADDR_MASK);
}

void mmu_enable_verbose(){
    mmu_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (mmu_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

void mmu_map_2mb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attr, uint8_t level) {
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;

    if (!table) panic("mmu_map_2mb null root", va);
    kprintfv("[MMU] Mapping 2mb memory %llx at [%i][%i][%i] for EL%i", (uint64_t)va, (int)l0_index, (int)l1_index, (int)l2_index, (int)level);

    if ((va & (GRANULE_2MB - 1ULL)) || (pa & (GRANULE_2MB - 1ULL))) {
        kprintf("[mmu] map2 align va=%llx pa=%llx", (uint64_t)va, (uint64_t)pa);
        panic("mmu_map_2mb unaligned", va);
    }

    uint64_t* l1 = walk_or_alloc(table, l0_index, 0, va);
    uint64_t* l2 = walk_or_alloc(l1, l1_index, 1, va);

    uint64_t want = make_pte(pa, attr_index, mem_attr, level, PD_BLOCK);
    uint64_t old = l2[l2_index];

    if ((old & 1) == 0){
        l2[l2_index] = want;
        return;
    }

    if ((old & 0b11) == PD_BLOCK){
        if ((old & PTE_ADDR_MASK) == (want & PTE_ADDR_MASK)){
            uint64_t diff = (old ^ want) & ~(PTE_ADDR_MASK | PTE_AF);
            if (diff == 0) return;
        }
        kprintf("[mmu] map2 conflict va=%llx old=%llx newpa=%llx root=%llx", (uint64_t)va, (uint64_t)old, (uint64_t)pa, (uint64_t)table);
        panic("mmu_map_2mb remap conflict", va);
    }

    if ((old & 0b11) == PD_TABLE){
        uint64_t* l3 = (uint64_t*)pt_pa_to_va(old & PTE_ADDR_MASK);
        uint64_t base = pa;
        uint64_t attr = (want & ~PTE_ADDR_MASK) & ~0b11;

        for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
            uint64_t p = base + (i * GRANULE_4KB);
            uint64_t e = l3[i];
            uint64_t expect = (p & PTE_ADDR_MASK) | attr | PD_TABLE;

            if ((e & 1ULL) == 0) {
                l3[i] = expect;
                continue;
            }

            uint64_t diff = (e ^ expect) & ~(PTE_ADDR_MASK | PTE_AF);

            if (diff != 0 || (e & PTE_ADDR_MASK) != (expect & PTE_ADDR_MASK)) {
                kprintf("[mmu] map2 table mismatch va=%llx i=%llu e=%llx", (uint64_t)va, (uint64_t)i, (uint64_t)e);
                panic("mmu_map_2mb table mismatch", va);
            }
        }

        return;
    }

    kprintf("[mmu] map2 bad type va=%llx old=%llx", (uint64_t)va, (uint64_t)old);
    panic("mmu_map_2mb bad type", va);
}

//Level 0 = EL0, Level 1 = EL1, Level 2 = Shared
void mmu_map_4kb(uint64_t *table, uint64_t va, uint64_t pa, uint64_t attr_index, uint8_t mem_attributes, uint8_t level) {
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    if (!table) panic("mmu_map_4kb null root", va);
    if ((table == (uint64_t*)kernel_ttbr0 || table == (uint64_t*)kernel_ttbr1) && level == MEM_PRIV_USER) panic("mmu_map_4kb user map in kernel ttbr", va);

    pa &= ~(GRANULE_4KB - 1ULL);
    va &= ~(GRANULE_4KB - 1ULL);

    uint8_t permission = 0;
    if (level == MEM_PRIV_KERNEL) permission = (mem_attributes & MEM_RW) ? 0 : 0b10;
    else if (level == MEM_PRIV_SHARED) permission = (mem_attributes & MEM_RW) ? 1 : 0b11;
    else permission = (mem_attributes & MEM_RW) ? 1 : 0b11;

    //TODO: proper memory permissions, including accounting for WXN
    kprintfv("[MMU] Mapping 4kb memory %llx at [%i][%i][%i][%i] for EL%i = %llx | %llx permission: %i",
        (uint64_t)va, (int)l0_index, (int)l1_index, (int)l2_index, (int)l3_index, (int)level, (uint64_t)pa,
        (uint64_t)make_pte(pa, attr_index, mem_attributes, level, PD_TABLE), (int)permission);

    uint64_t* l1 = walk_or_alloc(table, l0_index, 0, va);
    uint64_t* l2 = walk_or_alloc(l1, l1_index, 1, va);

    uint64_t l2_val = l2[l2_index];
    if (!(l2_val & 1)) {
        uint64_t* l3 = mmu_alloc();
        l2[l2_index] = (pt_va_to_pa(l3) & PTE_ADDR_MASK) | PD_TABLE;
        l2_val = l2[l2_index];
    } else if ((l2_val & 0b11) == PD_BLOCK){
        uint64_t base = (l2_val & PTE_ADDR_MASK) & ~(GRANULE_2MB - 1);
        uint64_t expected = base + (va & (GRANULE_2MB - 1));
        uint64_t want = make_pte(pa, attr_index, mem_attributes, level, PD_TABLE);

        uint64_t old_attr = (l2_val & ~PTE_ADDR_MASK) & ~0b11;
        uint64_t want_attr = (want & ~PTE_ADDR_MASK) & ~0b11;

        if (expected == pa &&old_attr == want_attr) return;

        {
            uint64_t old = l2[l2_index];
            if ((old & 0b11) != PD_BLOCK){
                kprintf("[mmu] split expected block va=%llx l2=%llu e=%llx", (uint64_t)va, (uint64_t)l2_index, (uint64_t)old);
                panic("mmu_split not a block", va);
            }

            uint64_t base2 = (old & PTE_ADDR_MASK) & ~(GRANULE_2MB - 1);
            uint64_t* l3 = mmu_alloc();
            uint64_t attr = (old & ~PTE_ADDR_MASK) & ~0b11;

            for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++){
                uint64_t p = base2 + (i * GRANULE_4KB);
                l3[i] = (p & PTE_ADDR_MASK) | attr | PD_TABLE;
            }

            l2[l2_index] = (pt_va_to_pa(l3) & PTE_ADDR_MASK) | PD_TABLE;
        }

        l2_val = l2[l2_index];
    }

    if ((l2_val & 0b11) != PD_TABLE){
        kprintf("[mmu] l2 bad type va=%llx e=%llx", (uint64_t)va, (uint64_t)l2_val);
        panic("mmu_map_4kb l2 bad type", va);
    }

    uint64_t *l3 = (uint64_t*)pt_pa_to_va(l2_val & PTE_ADDR_MASK);

    uint64_t want = make_pte(pa, attr_index, mem_attributes, level, PD_TABLE);
    uint64_t old = l3[l3_index];

    if (old & 1){
        if ((old & 0b11) != PD_TABLE){
            kprintf("[mmu] remap non-page va=%llx old=%llx", (uint64_t)va, (uint64_t)old);
            panic("mmu_map_4kb remap non-page", va);
        }

        if ((old & PTE_ADDR_MASK) == (want & PTE_ADDR_MASK)){
            uint64_t diff = (old ^ want) & ~(PTE_ADDR_MASK | PTE_AF);
            if (diff == 0) return;

            uint64_t rs = get_user_ram_start();
            uint64_t re = get_user_ram_end();

            bool kernel_root = table == (uint64_t*)kernel_ttbr0 || table == (uint64_t*)kernel_ttbr1;
            bool in_ram = pa >= rs && pa < re;

            uint64_t apmask = (0x3ULL << PTE_AP_SHIFT);

            if (kernel_root && in_ram){
                if (diff & apmask){
                    uint64_t forced = old & ~apmask;
                    if ((old & apmask) != 0){
                        l3[l3_index] = forced;
                        mmu_flush_all();
                        mmu_flush_icache();
                    }
                    return;
                }

                uint64_t allowed = 0;
                allowed |= (0x7ULL << PTE_ATTR_SHIFT);
                allowed |= (0x3ULL << PTE_SH_SHIFT);
                allowed |= PTE_PXN | PTE_UXN;

                if (!(diff & ~allowed)) {
                    l3[l3_index] = want;
                    mmu_flush_all();
                    mmu_flush_icache();
                    return;
                }
            }
        }

        kprintf("[mmu] remap conflict va=%llx old=%llx want=%llx newpa=%llx ttbr=%llx idx=%llu,%llu,%llu,%llu",
            (uint64_t)va, (uint64_t)old, (uint64_t)want, (uint64_t)pa, (uint64_t)table,
            (uint64_t)l0_index, (uint64_t)l1_index, (uint64_t)l2_index, (uint64_t)l3_index);
        panic("mmu_map_4kb remap conflict", va);
    }

    l3[l3_index] = want;
}

static inline void mmu_flush_all() {
    asm volatile (
        "dsb ishst\n"        // Ensure all memory accesses complete
        "tlbi vmalle1is\n"   // Invalidate all EL1 TLB entries (Inner Shareable)
        "dsb ish\n"          // Ensure completion of TLB invalidation
        "isb\n"              // Synchronize pipeline
        ::: "memory"
    );
}

static inline void mmu_flush_icache() {
    asm volatile (
        "ic iallu\n"         // Invalidate all instruction caches to PoU
        "isb\n"              // Ensure completion before continuing
        ::: "memory"
    );
}

uintptr_t* mmu_default_ttbr(){
    return kernel_ttbr1;
}

void mmu_unmap_table(uint64_t *table, uint64_t va, uint64_t pa){
    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;
    
    if (!table) panic("mmu_unmap null root", va);
    kprintfv("[MMU] Unmapping 4kb memory %llx at [%i][%i][%i][%i] for EL1", (uint64_t)va, (int)l0_index, (int)l1_index, (int)l2_index, (int)l3_index);

    va &= ~(GRANULE_4KB - 1);
    pa &= ~(GRANULE_4KB - 1);

    uint64_t e0 = table[l0_index];
    if (!(e0 & 1)) return;
    if ((e0 & 0b11) != PD_TABLE) panic("mmu_unmap l0 bad type", va);

    uint64_t* l1 = (uint64_t*)pt_pa_to_va(e0 & PTE_ADDR_MASK);
    uint64_t e1 = l1[l1_index];
    if (!(e1 & 1)) return;
    if ((e1 & 0b11) != PD_TABLE) panic("mmu_unmap l1 bad type", va);

    uint64_t* l2 = (uint64_t*)pt_pa_to_va(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[l2_index];
    if (!(e2 & 1)) return;

    if ((e2 & 0b11) == PD_BLOCK) {
        uint64_t old = l2[l2_index];
        if ((old & 0b11) != PD_BLOCK){
            kprintf("[mmu] split expected block va=%llx l2=%llu e=%llx", (uint64_t)va, (uint64_t)l2_index, (uint64_t)old);
            panic("mmu_split not a block", va);
        }

        uint64_t base = (old & PTE_ADDR_MASK) & ~(GRANULE_2MB - 1);
        uint64_t *l3 = mmu_alloc();
        uint64_t attr = (old & ~PTE_ADDR_MASK) & ~0b11;

        for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++){
            uint64_t p = base + (i * GRANULE_4KB);
            l3[i] = (p & PTE_ADDR_MASK) | attr | PD_TABLE;
        }

        l2[l2_index] = (pt_va_to_pa(l3) & PTE_ADDR_MASK) | PD_TABLE;

        e2 = l2[l2_index];
        if (!(e2 & 1)) panic("mmu_unmap split vanished", va);
    }

    if ((e2 & 0b11) != PD_TABLE) panic("mmu_unmap l2 bad type", va);

    uint64_t *l3 = (uint64_t*)pt_pa_to_va(e2 & PTE_ADDR_MASK);
    uint64_t old = l3[l3_index];
    if (!(old & 1)) return;

    if ((old & 0b11) != PD_TABLE) {
        kprintf("[mmu] unmap non-page va=%llx old=%llx", (uint64_t)va, (uint64_t)old);
        panic("mmu_unmap non-page", va);
    }

    if ((old & PTE_ADDR_MASK) != (pa & PTE_ADDR_MASK)) {
        kprintf("[mmu] unmap pa mismatch va=%llx old=%llx want=%llx", (uint64_t)va, (uint64_t)(old & PTE_ADDR_MASK), (uint64_t)(pa & PTE_ADDR_MASK));
        panic("mmu_unmap pa mismatch", va);
    }

    l3[l3_index] = 0;
}

void mmu_unmap(uint64_t va, uint64_t pa){
    mmu_unmap_table((uint64_t*)kernel_ttbr1, va, pa);
    //if (pttbr) mmu_unmap_table((uint64_t*)pttbr, va, pa);

    mmu_flush_all();
    mmu_flush_icache();
}


uint64_t *mmu_alloc(){
    uint64_t* p = (uint64_t*)talloc(GRANULE_4KB);
    if (!p) panic("mmu_alloc out of memory", 0);
    memset(p, 0, GRANULE_4KB);
    return p;
}

extern void mmu_start(uint64_t *ttbr1, uint64_t *ttbr0);

uintptr_t heap_end;

void mmu_init() {
    uint64_t mmfr0 = 0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t asidbits = (mmfr0 >> 4) & 0xF;
    asid_shift = 48;
    asid_mask = (asidbits >= 2) ? 0xFFFF : 0xFF;
    asid_gen = 1;
    asid_max = (uint32_t)asid_mask + 1;
    asid_next = 1;
    memset(asid_used, 0, sizeof(asid_used));
    asid_used[0] = 1;

    kernel_ttbr0 = (uintptr_t*)mmu_alloc();
    kernel_ttbr1 = (uintptr_t*)mmu_alloc();
    uintptr_t kimg_base = KERNEL_IMAGE_VA_BASE;
    uintptr_t kstart_pa = ((uintptr_t)__text_start) - kimg_base;
    uintptr_t kend_pa = ((uintptr_t)&kcode_end) - kimg_base;
    uintptr_t pa0 = kstart_pa & ~(GRANULE_4KB - 1);
    uintptr_t pa1 = (kend_pa + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);

    for (uintptr_t pa = pa0; pa < pa1; pa += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);

    uintptr_t v0 = ((uintptr_t)__text_start) & ~(GRANULE_4KB - 1);
    uintptr_t v1 = (((uintptr_t)__text_end) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_EXEC | MEM_NORM, MEM_PRIV_KERNEL);

    v0 = ((uintptr_t)__vectors_start) & ~(GRANULE_4KB - 1);
    v1 = (((uintptr_t)__vectors_end) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_EXEC | MEM_NORM, MEM_PRIV_KERNEL);

    v0 = ((uintptr_t)__rodata_start) & ~(GRANULE_4KB - 1);
    v1 = (((uintptr_t)__rodata_end) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_NORM, MEM_PRIV_KERNEL);

    v0 = ((uintptr_t)__data_start) & ~(GRANULE_4KB - 1);
    v1 = (((uintptr_t)__data_end) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);

    v0 = ((uintptr_t)__bss_start) & ~(GRANULE_4KB - 1);
    v1 = (((uintptr_t)__bss_end) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);

    v0 = ((uintptr_t)__kstack_bottom) & ~(GRANULE_4KB - 1);
    v1 = (((uintptr_t)__kstack_top) + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
    for (uintptr_t va = v0; va < v1; va += GRANULE_4KB) mmu_map_4kb((uint64_t*)kernel_ttbr1, va, va - kimg_base, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);

    uint64_t dstart = 0;
    uint64_t dsize = 0;
    if (dtb_addresses(&dstart,&dsize)) {
        uint64_t dend = (dstart + dsize + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);
        for (uint64_t addr = dstart & ~(GRANULE_4KB - 1); addr < dend; addr += GRANULE_4KB) {
            //mmu_map_4kb((uint64_t*)kernel_ttbr0, addr, addr, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
            mmu_map_4kb((uint64_t*)kernel_ttbr1, addr | HIGH_VA, addr, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
        }
    }

    uint64_t ram_start = get_user_ram_start();
    uint64_t ram_end = get_user_ram_end();

    for (uint64_t pa = ram_start; pa < ram_end;) {
        if ((!(pa & (GRANULE_2MB - 1))) && (ram_end - pa) >= GRANULE_2MB){
            //mmu_map_2mb((uint64_t*)kernel_ttbr0, pa, pa, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
            mmu_map_2mb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
            pa += GRANULE_2MB;
        } else {
            //mmu_map_4kb((uint64_t*)kernel_ttbr0, pa, pa, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
            mmu_map_4kb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
            pa += GRANULE_4KB;
        }
    }

    uint64_t mmio_phys = VIRT_TO_PHYS(MMIO_BASE);
    uint64_t mmio_end = mmio_phys + 0x10000000ULL; //16mb for raspi 2/3, 64 pi4 TODO move to hw.c 

    for (uint64_t pa = mmio_phys; pa < mmio_end; ) {
        if ((!(pa & (GRANULE_2MB - 1))) && (mmio_end - pa) >= GRANULE_2MB) {
            //mmu_map_2mb((uint64_t*)kernel_ttbr0, pa, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
            mmu_map_2mb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
            pa += GRANULE_2MB;
        } else {
            //mmu_map_4kb((uint64_t*)kernel_ttbr0, pa, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
            mmu_map_4kb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
            pa += GRANULE_4KB;
        }
    }

    for (uint64_t pa = 0x09000000ULL; pa < 0x0A000000ULL; pa += GRANULE_2MB) mmu_map_2mb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    for (uint64_t pa = 0x3F000000ULL; pa < 0x40000000ULL; pa += GRANULE_2MB) mmu_map_2mb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    for (uint64_t pa = 0xFE000000ULL; pa < 0xFF000000ULL; pa += GRANULE_2MB) mmu_map_2mb((uint64_t*)kernel_ttbr1, pa | HIGH_VA, pa, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);

    if (PCI_BASE && (PCI_BASE < (mmio_phys | HIGH_VA) || PCI_BASE >= (mmio_end | HIGH_VA))) {
        uint64_t p = VIRT_TO_PHYS(PCI_BASE) & ~(GRANULE_2MB - 1);
        //mmu_map_2mb((uint64_t*)kernel_ttbr0, p, p, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
        mmu_map_2mb((uint64_t*)kernel_ttbr1, p | HIGH_VA, p, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    }

    if (XHCI_BASE && (XHCI_BASE < (mmio_phys | HIGH_VA) || XHCI_BASE >= (mmio_end | HIGH_VA))) {
        uint64_t p = VIRT_TO_PHYS(XHCI_BASE) & ~(GRANULE_2MB - 1);
        //mmu_map_2mb((uint64_t*)kernel_ttbr0, p, p, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
        mmu_map_2mb((uint64_t*)kernel_ttbr1, p | HIGH_VA, p, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    }

    //hw_high_va();
    //mmu_start((uint64_t*)kernel_ttbr1, (uint64_t*)kernel_ttbr0);

    //kernel_ttbr0 = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)kernel_ttbr0);
    //kernel_ttbr1 = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)kernel_ttbr1);

    talloc_enable_high_va();
    page_alloc_enable_high_va();

    uint64_t ttbr1_pa = pt_va_to_pa(kernel_ttbr1) & PTE_ADDR_MASK;
    asm volatile("msr ttbr1_el1, %0" :: "r"(ttbr1_pa));
    asm volatile("dsb ish");
    asm volatile("isb");

    mmu_swap_ttbr(0, 0);
    asm volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)pttbr_hw));
    asm volatile("dsb ish");
    asm volatile("isb");

    mmu_flush_all();
    mmu_flush_icache();
    // kprintf("Finished MMU init");
}

void mmu_copy(uintptr_t *new_ttbr, uintptr_t *old_ttbr, int level){
    if (!new_ttbr || !old_ttbr) return;
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++){
        if (!(old_ttbr[i] & 1)) continue;

        if (level == 3 || (level == 2 && ((old_ttbr[i] & 0b11) == PD_BLOCK))){
            new_ttbr[i] = old_ttbr[i];
            continue;
        }

        if ((old_ttbr[i] & 0b11) != PD_TABLE){
            kprintf("[mmu] copy bad type lvl=%d i=%d e=%llx", level, i, (uint64_t)old_ttbr[i]);
            panic("mmu_copy bad type", (uint64_t)old_ttbr);
        }

        uintptr_t *old_entry = (uintptr_t*)pt_pa_to_va(old_ttbr[i] & PTE_ADDR_MASK);
        uintptr_t *new_entry = (uintptr_t*)mmu_alloc();
        uint64_t entry = old_ttbr[i] & ~PTE_ADDR_MASK;
        new_ttbr[i] = entry | (pt_va_to_pa(new_entry) & PTE_ADDR_MASK);
        mmu_copy(new_entry, old_entry, level+1);
    }
}

typedef struct {
    uintptr_t *table;
    int level;
    int i;
} mmu_free_frame_t;

void mmu_map_all(paddr_t pa){
    if (!kernel_ttbr1) return;
    uintptr_t base = pa & ~(GRANULE_2MB - 1);

    //mmu_map_2mb((uint64_t*)kernel_ttbr0, base, base, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);
    mmu_map_2mb((uint64_t*)kernel_ttbr1, base | HIGH_VA, base, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_KERNEL);

    mmu_flush_all();
    mmu_flush_icache();
}

void mmu_free_ttbr(uintptr_t *ttbr){
    if (!ttbr) return;

    mmu_free_frame_t stack[4];
    int sp = 0;

    stack[sp++] = (mmu_free_frame_t){ttbr,0,0};

    while (sp > 0){
        mmu_free_frame_t *f = &stack[sp -1];

        if (f->i >= PAGE_TABLE_ENTRIES){
            temp_free(f->table, GRANULE_4KB);
            sp--;
            continue;
        }

        uintptr_t e = f->table[f->i++];
        if (!(e & 1)) continue;
        if (f->level == 3) continue;
        if (f->level == 2 && ((e & 0b11) == PD_BLOCK)) continue;
        if ((e & 0b11) != PD_TABLE) continue;

        uintptr_t *child = (uintptr_t*)pt_pa_to_va(e & PTE_ADDR_MASK);

        if (sp >= 4){
            kprintf("[mmu] free_ttbr stack overflow lvl=%d e=%llx", f->level, (uint64_t)e);
            panic("mmu_free_ttbr stack overflow", (uintptr_t)child);
        }

        stack[sp++] = (mmu_free_frame_t){ child, f->level + 1, 0 };
    }
}

uintptr_t* mmu_new_ttbr(){
    return (uintptr_t*)mmu_alloc();
}

void register_device_memory(uint64_t va, uint64_t pa){
    uint64_t phys = VIRT_TO_PHYS(pa);
    uint64_t vlow = va;
    if (((vlow >> 47) & 1ULL) == 0) vlow = phys | HIGH_VA;
    mmu_map_4kb((uint64_t*)kernel_ttbr1, vlow, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    //mmu_map_4kb((uint64_t*)kernel_ttbr1, vhigh, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    //if (pttbr) mmu_map_4kb((uint64_t*)pttbr, vlow, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);

    mmu_flush_all();
    mmu_flush_icache();
}

void register_device_memory_2mb(uint64_t va, uint64_t pa){
    uint64_t phys = VIRT_TO_PHYS(pa) & ~(GRANULE_2MB - 1);
    uint64_t vlow = va;
    if (((vlow >> 47) & 1ULL) == 0) vlow = phys | HIGH_VA;
    vlow &= ~(GRANULE_2MB - 1);

    //mmu_map_2mb((uint64_t*)kernel_ttbr0, vlow, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    mmu_map_2mb((uint64_t*)kernel_ttbr1, vlow, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);
    //if (pttbr) mmu_map_2mb((uint64_t*)pttbr, vlow, phys, MAIR_IDX_DEVICE, MEM_RW | MEM_DEV, MEM_PRIV_KERNEL);

    mmu_flush_all();
    mmu_flush_icache();
}

void register_proc_memory(uint64_t va, uint64_t pa, uint8_t attributes, uint8_t level){
    uint64_t phys = VIRT_TO_PHYS(pa);

    if (level == MEM_PRIV_USER){
        if (!pttbr) panic("register_proc_memory no pttbr for user", va);
        mmu_map_4kb((uint64_t*)pttbr, va, phys, MAIR_IDX_NORMAL, attributes | MEM_NORM, level);
        mmu_flush_asid(pttbr_asid);
        mmu_flush_icache();
        return;
    }

    uint64_t vlow = va;
    if (((vlow >> 47) & 1ULL) == 0) vlow = phys | HIGH_VA;

    mmu_map_4kb((uint64_t*)kernel_ttbr1, vlow, phys, MAIR_IDX_NORMAL, attributes | MEM_NORM, level);

    //if (pttbr && ((va >> 47) & 1ULL) == 0) mmu_map_4kb((uint64_t*)pttbr, vlow, phys, MAIR_IDX_NORMAL, attributes | MEM_NORM, level);

    mmu_flush_all();
    mmu_flush_icache();
}

uintptr_t mmu_translate(uintptr_t va, int *status){
    int dummy;
    if (!status) status = &dummy;

    if (((uint64_t)va >> 48) != ((0-((((uint64_t)va >> 47)&1))) & 0xFFFF)) {
        *status = MMU_TR_ERR_PARAM;
        return 0;
    }

    uint64_t *root;
    if (((va >> 47) & 1) != 0) root = (uint64_t*)kernel_ttbr1;
    else root = (uint64_t*)(pttbr ? pttbr : kernel_ttbr0);

    if (!root){
        *status = MMU_TR_ERR_PARAM;
        return 0;
    }

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    uint64_t e0 = root[l0_index];
    if ((e0 & 1) == 0){
        *status = MMU_TR_ERR_L1;
        kprintfv("L1 Table missing");
        return 0;
    }
    if ((e0 & 0b11) != PD_TABLE){
        *status = MMU_TR_ERR_L1;
        kprintfv("L1 Table missing");
        return 0;
    }

    uint64_t* l1 = (uint64_t*)pt_pa_to_va(e0 & PTE_ADDR_MASK);
    uint64_t e1 = l1[l1_index];
    if ((e1 & 1) == 0){
        *status = MMU_TR_ERR_L2;
        kprintfv("L2 Table missing");
        return 0;
    }
    if ((e1 & 0b11) != PD_TABLE){
        *status = MMU_TR_ERR_L2;
        kprintfv("L2 Table missing");
        return 0;
    }
    uint64_t* l2 = (uint64_t*)pt_pa_to_va(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[l2_index];
    if (!(e2 & 1)) {
        *status = MMU_TR_ERR_L3;
        kprintfv("L3 Table missing");
        return 0;
    }

    if ((e2 & 0b11) == PD_BLOCK){
        *status = MMU_TR_OK;
        return (uintptr_t)(((e2 & PTE_ADDR_MASK) & ~(GRANULE_2MB - 1ULL)) | ((uint64_t)va & (GRANULE_2MB - 1ULL)));
    }

    if ((e2 & 0b11) != PD_TABLE){
        *status = MMU_TR_ERR_L3;
        kprintfv("L3 Table missing");
        return 0;
    }

    uint64_t* l3 = (uint64_t*)pt_pa_to_va(e2 & PTE_ADDR_MASK);
    uint64_t e3 = l3[l3_index];
    if ((e3 & 1) == 0){
        *status = MMU_TR_ERR_L4;
        kprintfv("L4 Table entry missing");
        return 0;
    }
    if ((e3 & 0b11ULL) != PD_TABLE){
        *status = MMU_TR_ERR_L4;
        kprintfv("L4 Table entry missing");
        return 0;
    }

    *status = MMU_TR_OK;
    return (uintptr_t)((e3 & PTE_ADDR_MASK) | ((uint64_t)va & (GRANULE_4KB - 1)));
}

void debug_mmu_address(uint64_t va){
    int tr = 0;
    uintptr_t pa = mmu_translate((uintptr_t)va, &tr);

    uart_raw_puts("[mmu dbg] VA=");
    uart_puthex(va);
    uart_raw_puts(" PA=");
    uart_puthex(pa);
    uart_raw_puts(" ST=");
    uart_puthex((uintptr_t)tr);
    uart_raw_putc('\n');

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;
    uint64_t *table;
    if (((va >> 47) & 1) != 0) table = (uint64_t*)kernel_ttbr1;
    else table = (uint64_t*)(pttbr ? pttbr : kernel_ttbr0);

    kprintf("Address %llx is meant to be mapped to [%i][%i][%i][%i]",va, l0_index,l1_index,l2_index,l3_index);

    if (!table) {
        kprintf("L1 Table missing");
        return;
    }

    uint64_t e0 = table[l0_index];
    if (!(e0 & 1) || ((e0 & 0b11) != PD_TABLE)) {
        kprintf("L1 Table missing");
        return;
    }

    uint64_t* l1 = (uint64_t*)pt_pa_to_va(e0 & PTE_ADDR_MASK);
    uint64_t e1 = l1[l1_index];
    if (!(e1 & 1) || ((e1 & 0b11) != PD_TABLE)) {
        kprintf("L2 Table missing");
        return;
    }

    uint64_t* l2 = (uint64_t*)pt_pa_to_va(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[l2_index];
    if (!(e2 & 1)){
        kprintf("L3 Table missing");
        return;
    }

    if ((e2 & 0b11) == PD_BLOCK) {
        kprintf("Mapped as 2MB memory in L3");
        kprintf("Entry: %b", (uint64_t)e2);
        return;
    }

    if ((e2 & 0b11) != PD_TABLE) {
        kprintf("L3 Table missing");
        return;
    }

    uint64_t* l3 = (uint64_t*)pt_pa_to_va(e2 & PTE_ADDR_MASK);
    uint64_t e3 = l3[l3_index];
    if (!(e3 & 1)){
        kprintf("L4 Table entry missing");
        return;
    }
    kprintf("Entry: %b", e3);
    return;
}

void mmu_swap_ttbr(uintptr_t* ttbr, uint16_t asid){
    pttbr = ttbr ? ttbr : (uintptr_t*)kernel_ttbr0;
    pttbr_asid = ttbr ? (asid & asid_mask) : 0;
    uint64_t root_pa = pt_va_to_pa(pttbr) & PTE_ADDR_MASK;
    pttbr_hw = ((uint64_t)pttbr_asid << asid_shift) | root_pa;
}

void mmu_flush_asid(uint16_t asid) {
    uint64_t v = (uint64_t)(asid & asid_mask) << asid_shift;
    asm volatile("dsb ishst" ::: "memory");
    asm volatile("tlbi aside1is, %0":: "r"(v) : "memory");
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

void mmu_asid_ensure(uint16_t *asid, uint32_t *asid_generation) {
    if (!asid || !asid_generation) return;
    if (!asid_max) return;
    if (*asid && *asid_generation == asid_gen) return;

    for (;;) {
        uint32_t scanned = 0;
        while (scanned + 1 < asid_max) {
            if (asid_next >= asid_max) asid_next = 1;

            uint32_t a = asid_next++;
            scanned++;

            uint32_t w = a >> 6;
            uint32_t b = a & 63;
            if (asid_used[w] & (1ULL << b)) continue;

            asid_used[w] |= 1ULL << b;
            *asid = (uint16_t)a;
            *asid_generation = asid_gen;
            return;
        }

        asid_gen++;
        mmu_flush_all();
        memset(asid_used, 0, sizeof(asid_used));
        asid_used[0] = 1;
        asid_next = 1;
    }
}

void mmu_asid_release(uint16_t asid, uint32_t asid_generation){
    if (!asid) return;
    if (!asid_max) return;
    if (asid_generation != asid_gen) return;

    uint32_t a = (uint32_t)(asid & asid_mask);
    uint32_t w = a >> 6;
    uint32_t b = a & 63;
    asid_used[w] &= ~(1ULL << b);
    mmu_flush_asid(asid);
}

bool mmu_unmap_and_get_pa(uint64_t *table, uint64_t va, uint64_t *pa) {
    if (!table) return false;
    va &= ~(GRANULE_4KB-1);

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    uint64_t l1_val = table[l0_index];
    if (!(l1_val & 1)) return false;
    if ((l1_val & 0b11) != PD_TABLE) return false;
    uint64_t* l1 = (uint64_t*)pt_pa_to_va(l1_val & PTE_ADDR_MASK);

    uint64_t l2_val = l1[l1_index];
    if (!(l2_val & 1)) return false;
    if ((l2_val & 0b11) != PD_TABLE) return false;
    uint64_t* l2 = (uint64_t*)pt_pa_to_va(l2_val & PTE_ADDR_MASK);

    uint64_t l3_val = l2[l2_index];
    if (!(l3_val & 1)) return false;

    if ((l3_val & 0b11) == PD_BLOCK) {
        uint64_t base = l3_val & PTE_ADDR_MASK;
        uint64_t off = va & (GRANULE_2MB - 1);
        uint64_t p = (base & ~(GRANULE_2MB - 1)) + off;

        if (pa) *pa = p;
        mmu_unmap_table(table, va, p);
        return true;
    }

    if ((l3_val & 0b11) != PD_TABLE) return false;
    uint64_t* l3 = (uint64_t*)pt_pa_to_va(l3_val & PTE_ADDR_MASK);

    uint64_t l4_val = l3[l3_index];
    if (!(l4_val & 1)) return false;

    if (pa) *pa = l4_val & PTE_ADDR_MASK;
    l3[l3_index] = 0;
    if (table == (uint64_t*)kernel_ttbr0 || table == (uint64_t*)kernel_ttbr1) return true;

    bool any = false;
    for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (l3[i] & 1) {
            any = true;
            break;
        }
    }

    if (any) return true;
    temp_free(l3, GRANULE_4KB);
    l2[l2_index] = 0;

    any = false;
    for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (l2[i] & 1) {
            any = true;
            break;
        }
    }

    if (any) return true;
    temp_free(l2, GRANULE_4KB);
    l1[l1_index] = 0;

    any = false;
    for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (l1[i] & 1) {
            any = true;
            break;
        }
    }

    if (any) return true;
    temp_free(l1, GRANULE_4KB);
    table[l0_index] = 0;

    return true;
}

bool mmu_set_access_flag(uint64_t *table, uint64_t va) {
    if (!table) return false;
    va &= ~(GRANULE_4KB-1);

    uint64_t l0_index = (va >> 39) & 0x1FF;
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;

    uint64_t e0 = table[l0_index];
    if (!(e0 & 1)) return false;
    if ((e0 & 0b11) != PD_TABLE) return false;
    uint64_t *l1 = (uint64_t*)pt_pa_to_va(e0 & PTE_ADDR_MASK);

    uint64_t e1 = l1[l1_index];
    if (!(e1 & 1)) return false;
    if ((e1 & 0b11) != PD_TABLE) return false;
    uint64_t *l2 = (uint64_t*)pt_pa_to_va(e1 & PTE_ADDR_MASK);

    uint64_t e2 = l2[l2_index];
    if (!(e2 & 1)) return false;

    if ((e2 & 0b11) == PD_BLOCK) {
        if (e2 & PTE_AF) return true;
        l2[l2_index] = e2 | PTE_AF;
        return true;
    }

    if ((e2 & 0b11) != PD_TABLE) return false;
    uint64_t *l3 = (uint64_t*)pt_pa_to_va(e2 & PTE_ADDR_MASK);

    uint64_t e3 = l3[l3_index];
    if (!(e3 & 1)) return false;
    if ((e3 & 0b11) != PD_TABLE) return false;
    if (e3 & PTE_AF) return true;
    l3[l3_index] = e3 | PTE_AF;
    return true;
}