#pragma once
// ***************************************
// SCTLR_EL1, System Control Register (EL1), Page 2654 of AArch64-Reference-Manual.
// ***************************************

#define SCTLR_RESERVED (3 << 28) | (3 << 22) | (1 << 20) | (1 << 11)
#define SCTRL_NOT_ALIGN_ACCESS_DISABLED (1 << 6)
#define SCTLR_EE_LITTLE_ENDIAN (0 << 25)
#define SCTLR_EOE_LITTLE_ENDIAN (0 << 24)
#define SCTLR_I_CACHE_DISABLED (0 << 12)
#define SCTLR_D_CACHE_DISABLED (0 << 2)
#define SCTLR_I_CACHE_ENABLED (1 << 12)
#define SCTLR_D_CACHE_ENABLED (1 << 2)
#define SCTLR_MMU_DISABLED (0 << 0)
#define SCTLR_MMU_ENABLED (1 << 0)

#define SCTLR_VALUE_MMU_DISABLED (SCTLR_RESERVED | SCTRL_NOT_ALIGN_ACCESS_DISABLED | SCTLR_EE_LITTLE_ENDIAN | SCTLR_I_CACHE_ENABLED | SCTLR_D_CACHE_ENABLED | SCTLR_MMU_DISABLED)

// ***************************************
// HCR_EL2, Hypervisor Configuration Register (EL2), Page 2487 of AArch64-Reference-Manual.
// ***************************************

#define HCR_RW (1 << 31)
#define HCR_VALUE HCR_RW

// ***************************************
// SCR_EL3, Secure Configuration Register (EL3), Page 2648 of AArch64-Reference-Manual.
// ***************************************

#define SCR_RESERVED (3 << 4)
#define SCR_RW (1 << 10)
#define SCR_NS (1 << 0)
#define SCR_VALUE (SCR_RESERVED | SCR_RW | SCR_NS)

// ***************************************
// SPSR_EL3, Saved Program Status Register (EL3) Page 389 of AArch64-Reference-Manual.
// ***************************************

#define SPSR_MASK_ALL (7 << 6)
#define SPSR_EL1h (5 << 0)
#define SPSR_EL2h (9 << 0)
#define SPSR3_VALUE (SPSR_MASK_ALL | SPSR_EL2h)
#define SCTLR_VALUE_EARLY (SCTLR_RESERVED | SCTLR_EE_LITTLE_ENDIAN | SCTLR_I_CACHE_DISABLED | SCTLR_D_CACHE_DISABLED | SCTLR_MMU_DISABLED)

// ***************************************
// CNTHCTL_EL2, Counter-timer Hypervisor Control Register (EL2) Page 9569 of AArch64-Reference-Manual.
// ***************************************

#define TRAP_PHYS_TIMER_DISABLED (1 << 0) 
#define TRAV_VIRT_TIMER_DISABLED (1 << 1) //Disable trapping of timer calls on hypervisor level
#define CNTHCTL_VALUE (TRAP_PHYS_TIMER_DISABLED | TRAV_VIRT_TIMER_DISABLED)

// ***************************************
// MMU
// ***************************************

#define TCR_T0SZ (64-48)
#define TCR_T1SZ (64-48)
#define TCR_TG0_4KB (0b00)
#define TCR_TG1_4KB (0b10)
#define TCR_SH_INNER (0b11)
#define TCR_RGN_WBWA (0b01)

#define TCR_VALUE_BASE (0ULL | (TCR_T0SZ << 0) | (TCR_T1SZ << 16) \
        | (TCR_TG0_4KB << 14) | (TCR_TG1_4KB << 30) | (TCR_SH_INNER << 12) \
        | (TCR_SH_INNER << 28) | (TCR_RGN_WBWA << 8) | (TCR_RGN_WBWA << 10) \
        | (TCR_RGN_WBWA << 24) | (TCR_RGN_WBWA << 26))

#define MAIR_DEVICE_nGnRnE 0b00000000
#define MAIR_NORMAL_WBWA 0xFF
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1

#define MAIR_VALUE ((MAIR_DEVICE_nGnRnE << (MAIR_IDX_DEVICE * 8)) | (MAIR_NORMAL_WBWA << (MAIR_IDX_NORMAL * 8)))

#define HIGH_VA 0xFFFF800000000000ULL
#define PHYS_TO_VIRT(x) (((uintptr_t)(x) != 0) ? ((uintptr_t)(x) | HIGH_VA) : 0)
#define VIRT_TO_PHYS(x) (((uintptr_t)(x) != 0) ? ((uintptr_t)(x) & ~HIGH_VA) : 0)

#define PHYS_TO_VIRT_P(x) (((uintptr_t)(x) != 0) ? (void*)(((uintptr_t)(x)) | HIGH_VA) : 0)
#define VIRT_TO_PHYS_P(x) (((uintptr_t)(x) != 0) ? (void*)(((uintptr_t)(x)) & ~HIGH_VA) : 0)