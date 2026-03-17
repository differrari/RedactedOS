#include "sysregs.h"
#include "memory/mmu.h"
#include "memory/va_layout.h"
#include "memory/page_allocator.h"

__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_ttbr0_l0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t boot_ttbr1_l0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t low_l1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t low_l2_0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t low_l2_1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t low_l2_2[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t low_l2_3[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t direct_l1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t direct_l2_0[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t direct_l2_1[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t direct_l2_2[PAGE_TABLE_ENTRIES];
__attribute__((aligned(GRANULE_4KB), section(".boot.bss")))
static uint64_t direct_l2_3[PAGE_TABLE_ENTRIES];
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

extern void boot_mmu_enter(uint64_t mair, uint64_t tcr, uint64_t ttbr0, uint64_t ttbr1, uint64_t board_type, uint64_t dtb_pa) __attribute__((noreturn));
extern uint64_t boot_args[3];

__attribute__((section(".boot.text"), weak, noreturn))
void boot_mmu_setup(uint64_t board_type) {
	uint64_t dtb_pa = boot_args[2] ? boot_args[2] : boot_args[0]; // see dtb.c
	uint64_t mmfr0 = 0;
	asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));

	uint64_t ips= mmfr0 & 0xFULL;
	if (ips > 6) ips = 6;

	uint64_t tcr = TCR_VALUE_BASE | (ips << 32);

	uint64_t asidbits = (mmfr0 >> 4) & 0xFULL;
	if (asidbits >= 2) tcr |= (0x10ULL << 32);

	uint64_t attr_normal_exec = PTE_AF | (0b11ULL << PTE_SH_SHIFT) | ((uint64_t)MAIR_IDX_NORMAL << PTE_ATTR_SHIFT) | PTE_UXN;
	uint64_t attr_normal_noexec = attr_normal_exec | PTE_PXN;
	uint64_t attr_device_noexec = PTE_AF | ((uint64_t)MAIR_IDX_DEVICE << PTE_ATTR_SHIFT) | PTE_UXN | PTE_PXN;

	boot_ttbr0_l0[0] = ((uint64_t)low_l1 & PTE_ADDR_MASK) | PD_TABLE;
	boot_ttbr1_l0[(HIGH_VA >> 39) & 0x1FF] = ((uint64_t)direct_l1 & PTE_ADDR_MASK) | PD_TABLE;
	boot_ttbr1_l0[(KERNEL_IMAGE_VA_BASE >> 39) & 0x1FF] = ((uint64_t)boot_kimg_l1 & PTE_ADDR_MASK) | PD_TABLE;

	low_l1[0] = ((uint64_t)low_l2_0 & PTE_ADDR_MASK) | PD_TABLE;
	low_l1[1] = ((uint64_t)low_l2_1 & PTE_ADDR_MASK) | PD_TABLE;
	low_l1[2] = ((uint64_t)low_l2_2 & PTE_ADDR_MASK) | PD_TABLE;
	low_l1[3] = ((uint64_t)low_l2_3 & PTE_ADDR_MASK) | PD_TABLE;

	direct_l1[0] = ((uint64_t)direct_l2_0 & PTE_ADDR_MASK) | PD_TABLE;
	direct_l1[1] = ((uint64_t)direct_l2_1 & PTE_ADDR_MASK) | PD_TABLE;
	direct_l1[2] = ((uint64_t)direct_l2_2 & PTE_ADDR_MASK) | PD_TABLE;
	direct_l1[3] = ((uint64_t)direct_l2_3 & PTE_ADDR_MASK) | PD_TABLE;

	boot_kimg_l1[0] = ((uint64_t)boot_kimg_l2_0 & PTE_ADDR_MASK) | PD_TABLE;
	boot_kimg_l1[1] = ((uint64_t)boot_kimg_l2_1 & PTE_ADDR_MASK) | PD_TABLE;
	boot_kimg_l1[2] = ((uint64_t)boot_kimg_l2_2 & PTE_ADDR_MASK) | PD_TABLE;
	boot_kimg_l1[3] = ((uint64_t)boot_kimg_l2_3 & PTE_ADDR_MASK) | PD_TABLE;

	for (uint64_t li = 0; li < PAGE_TABLE_ENTRIES; li++) {
		uint64_t pa0 = li << 21;
		uint64_t pa1 = (1ULL << 30) +(li << 21);
		uint64_t pa2 = (2ULL << 30) +(li << 21);
		uint64_t pa3 = (3ULL << 30) +(li << 21);

		low_l2_0[li] = (pa0 & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		low_l2_1[li] = (pa1 & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		low_l2_2[li] = (pa2 & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		low_l2_3[li] = (pa3 & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;

		direct_l2_0[li] = (pa0 & PTE_ADDR_MASK) | attr_normal_noexec | PD_BLOCK;
		direct_l2_1[li] = (pa1 & PTE_ADDR_MASK) | attr_normal_noexec | PD_BLOCK;
		direct_l2_2[li] = (pa2 & PTE_ADDR_MASK) | attr_normal_noexec | PD_BLOCK;
		direct_l2_3[li] = (pa3 & PTE_ADDR_MASK) | attr_normal_noexec | PD_BLOCK;
	}

	for (uint64_t pa = 0x08000000ULL; pa < 0x0A000000ULL; pa += GRANULE_2MB) {
		uint64_t l = (pa >> 21) & 0x1FF;
		low_l2_0[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
		direct_l2_0[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
	}

	for (uint64_t pa = 0xFE000000ULL; pa < 0xFF000000ULL; pa += GRANULE_2MB){
		uint64_t g = (pa >> 30) & 0x1FF;
		uint64_t l = (pa >> 21) & 0x1FF;
		if (g == 0) {
			low_l2_0[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
			direct_l2_0[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
		}
		if (g == 1) {
			low_l2_1[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
			direct_l2_1[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
		}
		if (g == 2) {
			low_l2_2[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
			direct_l2_2[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
		}
		if (g == 3) {
			low_l2_3[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
			direct_l2_3[l] = (pa & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
		}
	}

	low_l1[64] = (0x1000000000ULL & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
	low_l1[65] = (0x1040000000ULL & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;
	low_l1[124] = (0x1F00000000ULL & PTE_ADDR_MASK) | attr_device_noexec | PD_BLOCK;

	uint64_t kstart = __boot_kimg_pa_start;
	uint64_t kend = __boot_kimg_pa_end;
	uint64_t ks = kstart & ~(GRANULE_2MB - 1ULL);
	uint64_t ke = (kend + (GRANULE_2MB - 1ULL)) & ~(GRANULE_2MB - 1ULL);

	for (uint64_t pa = ks; pa < ke; pa += GRANULE_2MB){
		uint64_t g = (pa >> 30) & 0x1FF;
		uint64_t l = (pa >> 21) & 0x1FF;

		if (g == 0) boot_kimg_l2_0[l] = (pa & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		if (g == 1) boot_kimg_l2_1[l] = (pa & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		if (g == 2) boot_kimg_l2_2[l] = (pa & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
		if (g == 3) boot_kimg_l2_3[l] = (pa & PTE_ADDR_MASK) | attr_normal_exec | PD_BLOCK;
	}

	boot_mmu_enter((uint64_t)MAIR_VALUE, tcr, (uint64_t)boot_ttbr0_l0 & PTE_ADDR_MASK,(uint64_t)boot_ttbr1_l0 & PTE_ADDR_MASK,board_type, dtb_pa);
}
