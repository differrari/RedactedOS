.global _start
.section .text
.org 0x80000
_start:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, drop_el3
half:
    wfe
    b half

drop_el3:

    msr sctlr_el2, xzr
    mrs x0, hcr_el2
    orr x0, x0, #(1<<19)
    msr hcr_el2, x0

    mrs x0, scr_el3
    orr x0, x0, #(1<<10)
    orr x0, x0, #(1<<0)
    msr scr_el3, x0

    mov x0, #0b01001//DAIF 0000
    msr spsr_el3, x0

    adr x0, el2_entry
    msr elr_el3, x0
    eret

el2_entry:

    mov	x0, #3 << 20
    msr	cpacr_el1, x0

    ldr x0, =0x30C00800
    msr	sctlr_el1, x0

    mrs x0, hcr_el2
    orr x0, x0, #(1<<31)
    msr hcr_el2, x0

    mov x0, 0x1C5
    msr spsr_el2, x0

    adr x0, el1_non_secure
    msr elr_el2, x0

    eret

el1_non_secure:
    ldr     x5, =__bss_start
    ldr     x6, =__bss_end
clear_bss:
    cmp     x5, x6
    b.ge    stack_setup
    str     xzr, [x5], #8
    b       clear_bss

stack_setup:
    ldr x0, =stack_top
    mov sp, x0

    bl kernel_main

    b halt