#include "sysregs.h"
#include "memory/mmu.h"

#define PAGE_TABLE_ENTRIES 512
#define PD_TABLE 0b11
#define PD_BLOCK 0b01

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_AF (1ULL << 10)
#define PTE_SH_SHIFT 8
#define PTE_AP_SHIFT 6
#define PTE_ATTR_SHIFT 2

#define DMAP_L0_INDEX 256
#define KIMG_L0_INDEX 384

__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_ttbr0_l0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_ttbr1_l0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_l1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_l2_0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_l2_1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_l2_2[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_l2_3[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_kimg_l1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_kimg_l2_0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_kimg_l2_1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_kimg_l2_2[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_kimg_l2_3[PAGE_TABLE_ENTRIES];

extern uint64_t __boot_kimg_pa_start;
extern uint64_t __boot_kimg_pa_end;

extern void boot_vectors_el1(void);
extern void kernel_main(uint64_t board_type, uint64_t dtb_pa);
extern uint64_t boot_args[3];

__attribute__((section(".boot.text"), weak, noreturn))
void boot_mmu_setup(uint64_t board_type) {
    uint64_t dtb_pa = boot_args[2] ? boot_args[2] : boot_args[0];
    uint64_t mmfr0 = 0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));

    uint64_t ips= mmfr0 & 0xFULL;
    if (ips > 6) ips = 6;

    uint64_t tcr = TCR_VALUE_BASE | (ips << 32);

    uint64_t asidbits = (mmfr0 >> 4) & 0xFULL;
    if (asidbits >= 2) tcr |= (0x10ULL << 32);

    boot_ttbr0_l0[0] = ((uint64_t)boot_l1 & PTE_ADDR_MASK) | PD_TABLE;
    boot_ttbr1_l0[DMAP_L0_INDEX] = ((uint64_t)boot_l1 & PTE_ADDR_MASK) | PD_TABLE;
    boot_ttbr1_l0[KIMG_L0_INDEX] = ((uint64_t)boot_kimg_l1 & PTE_ADDR_MASK) | PD_TABLE;

    boot_l1[0] = ((uint64_t)boot_l2_0 & PTE_ADDR_MASK) | PD_TABLE;
    boot_l1[1] = ((uint64_t)boot_l2_1 & PTE_ADDR_MASK) | PD_TABLE;
    boot_l1[2] = ((uint64_t)boot_l2_2 & PTE_ADDR_MASK) | PD_TABLE;
    boot_l1[3] = ((uint64_t)boot_l2_3 & PTE_ADDR_MASK) | PD_TABLE;

    boot_kimg_l1[0] = ((uint64_t)boot_kimg_l2_0 & PTE_ADDR_MASK) | PD_TABLE;
    boot_kimg_l1[1] = ((uint64_t)boot_kimg_l2_1 & PTE_ADDR_MASK) | PD_TABLE;
    boot_kimg_l1[2] = ((uint64_t)boot_kimg_l2_2 & PTE_ADDR_MASK) | PD_TABLE;
    boot_kimg_l1[3] = ((uint64_t)boot_kimg_l2_3 & PTE_ADDR_MASK) | PD_TABLE;

    for (uint64_t gi = 0; gi < 4; gi++){
        uint64_t *l2 = boot_l2_0;
        if (gi == 1) l2 = boot_l2_1;
        if (gi == 2) l2 = boot_l2_2;
        if (gi == 3) l2 = boot_l2_3;

        uint64_t base = gi << 30;
        for (uint64_t li = 0; li < PAGE_TABLE_ENTRIES; li++) {
            uint64_t pa = base + (li << 21);
            uint64_t attr = PTE_AF | (0b11ULL << PTE_SH_SHIFT) | (0ULL << PTE_AP_SHIFT) | ((uint64_t)MAIR_IDX_NORMAL << PTE_ATTR_SHIFT);
            l2[li] = (pa & PTE_ADDR_MASK) | attr | PD_BLOCK;
        }
    }

    uint64_t li = (0x08000000ULL >> 21) & 0x1FF;
    boot_l2_0[li] = (0x08000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;

    li = (0x09000000ULL >> 21) & 0x1FF;
    boot_l2_0[li] = (0x09000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;

    li = (0x10000000ULL >> 21) & 0x1FF;
    boot_l2_0[li] = (0x10000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;

    li = (0x3F000000ULL >> 21) & 0x1FF;
    boot_l2_0[li] = (0x3F000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;

    for (uint64_t pa = 0xFE000000ULL; pa < 0xFF000000ULL; pa += GRANULE_2MB){
        uint64_t g = (pa >> 30) & 0x1FF;
        uint64_t l = (pa >> 21) & 0x1FF;
        if (g == 0) boot_l2_0[l] = (pa & PTE_ADDR_MASK) |(PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
        if (g == 1) boot_l2_1[l] = (pa & PTE_ADDR_MASK) |(PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
        if (g == 2) boot_l2_2[l] = (pa & PTE_ADDR_MASK) |(PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
        if (g == 3) boot_l2_3[l] = (pa & PTE_ADDR_MASK) |(PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
    }

    boot_l1[64] = (0x1000000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
    boot_l1[65] = (0x1040000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;
    boot_l1[124] = (0x1F00000000ULL & PTE_ADDR_MASK) | (PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT)) | PD_BLOCK;

    uint64_t kstart = __boot_kimg_pa_start;
    uint64_t kend = __boot_kimg_pa_end;
    uint64_t ks = kstart & ~(GRANULE_2MB - 1ULL);
    uint64_t ke = (kend + (GRANULE_2MB - 1ULL)) & ~(GRANULE_2MB - 1ULL);

    for (uint64_t pa = ks; pa < ke; pa += GRANULE_2MB){
        uint64_t g = (pa >> 30) & 0x1FF;
        uint64_t l = (pa >> 21) & 0x1FF;
        uint64_t attr = PTE_AF | (0b11ULL << PTE_SH_SHIFT) | (0ULL << PTE_AP_SHIFT) | ((uint64_t)MAIR_IDX_NORMAL << PTE_ATTR_SHIFT);

        if (g == 0) boot_kimg_l2_0[l] = (pa & PTE_ADDR_MASK) | attr | PD_BLOCK;
        if (g == 1) boot_kimg_l2_1[l] = (pa & PTE_ADDR_MASK) | attr | PD_BLOCK;
        if (g == 2) boot_kimg_l2_2[l] = (pa & PTE_ADDR_MASK) | attr | PD_BLOCK;
        if (g == 3) boot_kimg_l2_3[l] = (pa & PTE_ADDR_MASK) | attr | PD_BLOCK;
    }

    asm volatile("msr mair_el1, %0" :: "r"((uint64_t)MAIR_VALUE));
    asm volatile("msr tcr_el1, %0" :: "r"(tcr));
    asm volatile("dsb ish");
    asm volatile("isb");

    uint64_t ttbr0_pa = ((uint64_t)boot_ttbr0_l0) & PTE_ADDR_MASK;
    uint64_t ttbr1_pa = ((uint64_t)boot_ttbr1_l0) & PTE_ADDR_MASK;

    asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_pa));
    asm volatile("msr ttbr1_el1, %0" :: "r"(ttbr1_pa));

    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1ULL;
    sctlr &= ~(1ULL << 19);
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    asm volatile("dsb ish");
    asm volatile("isb");

    asm volatile(
        "ldr x0, =boot_vectors_el1\n"
        "msr vbar_el1, x0\n"
        "isb\n"
        ::: "x0", "memory"
    );

    asm volatile(
        "ldr x2, =ksp\n"
        "mov sp, x2\n"
        "mov x0, %0\n"
        "mov x1, %1\n"
        "ldr x2, =kernel_main\n"
        "blr x2\n"
        "1: wfe\n"
        "b 1b\n"
        :: "r"(board_type),"r"(dtb_pa) : "x0", "x1", "x2", "memory"
    );
}
