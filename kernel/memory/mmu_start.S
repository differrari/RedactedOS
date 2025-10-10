#include "sysregs.h"

.global mmu_start
mmu_start://(uint64_t *low x0, uint64_t *high x1)
    ldr x3, =MAIR_VALUE
    msr mair_el1, x3
    ldr x3, =TCR_VALUE
    msr tcr_el1, x3
    dsb ish
    isb

    msr ttbr0_el1, x0
    msr ttbr1_el1, x1
    
    mrs x0, sctlr_el1
    orr x0, x0, #0x1
    bic x0, x0, #(1 << 19)
    msr sctlr_el1, x0
    isb

    ldr x1, =HIGH_VA

    ldr x0, =ksp
    orr x0, x0, x1
    adrp x3, ksp
    str x0, [x3]

    mov x0, sp
    orr x0, x0, x1
    mov sp, x0

    mov x0, x29
    orr x0, x0, x1
    mov x29, x0

    mov x0, x30
    orr x0, x0, x1
    mov x30, x0

    mrs x0, vbar_el1
    orr x0, x0, x1
    msr vbar_el1, x0

    ldr x0, =mmu_finish
    orr x0, x0, x1
    br x0