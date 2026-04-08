#include "sysregs.h"

.global boot_mmu_enter
.extern boot_vectors_el1
.extern kernel_main
.extern ksp
.section .boot.text, "ax"

boot_mmu_enter:
    msr mair_el1, x0
    msr tcr_el1, x1
    dsb ish
    isb

    msr ttbr0_el1, x2
    msr ttbr1_el1, x3

    mrs x6, sctlr_el1
    orr x6, x6, #1
    mov x7, #(1 << 19)
    bic x6, x6, x7
    msr sctlr_el1, x6
    dsb ish
    isb

    ldr x6, =boot_vectors_el1
    msr vbar_el1, x6
    isb

    ldr x6, =ksp
    mov sp, x6
    mov x0, x4
    mov x1, x5
    bl kernel_main
