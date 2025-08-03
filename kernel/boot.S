#include "sysregs.h"

.global _start
.section .text
_start:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, setup_vars
half:
    wfe
    b half

setup_vars:
    ldr     x5, =__bss_start
    ldr     x6, =__bss_end
clear_bss:
    cmp     x5, x6
    b.ge    check_hw
    str     xzr, [x5], #8
    b       clear_bss

check_hw:
    adr     x0, . 

    movz    x1, #0x4100, lsl #16
    and     x2, x0, #0xFFF00000
    cmp     x2, x1
    b.eq    virt

    movz    x1, #0x4000, lsl #16
    and     x2, x0, #0xFFF00000
    cmp     x2, x1
    b.eq    virt

    mov     x1, #0x80000
    and     x2, x0, #0xFFFFF000
    cmp     x2, x1
    b.eq    rpi

    mov     x1, #0x100000
    and     x2, x0, #0xFFFFF000
    cmp     x2, x1
    b.eq    rpi

    b       .//Halt if none

virt:
    mov     w0, #1
    adrp    x1, BOARD_TYPE
    strb    w0, [x1, #:lo12:BOARD_TYPE]
    b       stack_setup
rpi:
    mov     w0, #2
    adrp    x1, BOARD_TYPE
    strb    w0, [x1, #:lo12:BOARD_TYPE]
    b       drop_el3

drop_el3:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #3
    b.ne el2_entry

    msr sctlr_el2, xzr

    ldr x0, =SCR_VALUE
    msr scr_el3, x0

    ldr x0, =SPSR3_VALUE
    msr spsr_el3, x0

    adr x0, el2_entry
    msr elr_el3, x0
    eret

el2_entry:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne stack_setup

    ldr x0, =CNTHCTL_VALUE
    msr cnthctl_el2, x0

    msr cntvoff_el2, xzr

    mov	x0, #3 << 20
    msr	cpacr_el1, x0

    ldr x0, =SCTLR_VALUE_MMU_DISABLED
    msr	sctlr_el1, x0

    ldr x0, =HCR_RW
    msr hcr_el2, x0

    mov x0, 0x1C5
    msr spsr_el2, x0

    adr x0, stack_setup
    msr elr_el2, x0

    eret

stack_setup:
    ldr x1, =ksp
    mov sp, x1

    mov x29, xzr
    mov x30, xzr
    mrs x1, CPACR_EL1
    orr x1, x1, #(3 << 20)
    msr CPACR_EL1, x1
    bl kernel_main

    b .