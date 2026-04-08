#include "sysregs.h"
.section .boot.data,"aw"
.align 3

.global boot_args //uint64_t boot_args[3]
boot_args:
    .zero 3*8

.global boot_fault_info_el1
.global boot_fault_info_el2
.global boot_fault_info_el3
boot_fault_info_el1: 
    .zero 6*8*3

.set boot_fault_info_el2, boot_fault_info_el1 + 6*8
.set boot_fault_info_el3, boot_fault_info_el1 + 2*6*8

.global _start
.section .boot.entry,"ax"
_start:
    ldr x3, =boot_args
    stp x0, x1, [x3]
    str x2, [x3, #16]

    mrs x1, CurrentEL //get current exception level(CurrentEL EL, bits [3:2])
    lsr x1, x1, #2

    //early vectors
    ldr x2, =boot_vectors_el1
    msr vbar_el1, x2

    cmp x1, #3
    b.eq 1f
    cmp x1, #2
    b.eq 2f
    b 3f
1:
    ldr x2, =boot_vectors_el2
    msr vbar_el2, x2
    ldr x2, =boot_vectors_el3
    msr vbar_el3, x2
    b 3f
2:
    ldr x2, =boot_vectors_el2
    msr vbar_el2, x2
3:
    isb
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, setup_vars
half:
    wfe
    b half

setup_vars:
    ldr     x5, =__boot_bss_start
    ldr     x6, =__boot_bss_end
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
    mov     w20, #1
    b       drop_el3
rpi:
    mov     w20, #2
    b       drop_el3

drop_el3:
    mrs x9, CurrentEL
    lsr x9, x9, #2
    ldr x0, =boot_vectors_el1
    msr vbar_el1, x0

    cmp x9, #3
    b.eq 1f
    cmp x9, #2
    b.eq 2f
    b 3f
1:
    ldr x0, =boot_vectors_el2
    msr vbar_el2, x0
    ldr x0, =boot_vectors_el3
    msr vbar_el3, x0
    b 3f
2:
    ldr x0, =boot_vectors_el2
    msr vbar_el2, x0
3:
    isb

    cmp x9, #3
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
    ldr x0, =SCTLR_VALUE_MMU_DISABLED
    msr sctlr_el1, x0

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne stack_setup

    ldr x1, =boot_vectors_el2
    msr vbar_el2, x1
    ldr x1, =boot_vectors_el1
    msr vbar_el1, x1
    isb

    ldr x0, =CNTHCTL_VALUE
    msr cnthctl_el2, x0

    msr cntvoff_el2, xzr

    mov x0, #3 << 20
    msr cpacr_el1, x0

    ldr x0, =HCR_RW
    msr hcr_el2, x0

    mov x0, 0x1C5
    msr spsr_el2, x0

    adr x0, stack_setup
    msr elr_el2, x0

    eret

stack_setup:
    ldr x1, =boot_stack_top
    mov sp, x1

    mov x29, xzr
    mov x30, xzr
    mrs x1, CPACR_EL1
    orr x1, x1, #(3 << 20)
    msr CPACR_EL1, x1
    mov x0, x20
    bl boot_mmu_setup

    b .

//early exc vectors
.section .boot.vectors,"ax"
.align 11

.global boot_vectors_el1
.global boot_vectors_el2
.global boot_vectors_el3

.macro BOOT_VECTOR_COMMON esr, elr, far, info_sym
    ldr x0, =\info_sym
    mrs x1, \esr
    str x1, [x0, #0]
    mrs x1, \elr
    str x1, [x0, #8]
    mrs x1, \far
    str x1, [x0, #16]
    mov x1, sp
    str x1, [x0, #24]
    mrs x1, mpidr_el1 ///CPU ID
    str x1, [x0, #32]
    mrs x1, CurrentEL
    str x1, [x0, #40]
1:  wfe
    b 1b
.endm

//VBAR EL
// Entry 0 (offset 0x000)
// Entry 1 (offset 0x080)
// Entry 15 (offset 0x780)
//0x800 bytes
.macro VECTOR_TABLE label, esr, elr, far, info
\label:
    BOOT_VECTOR_COMMON \esr, \elr, \far, \info
    .space 128 - (. - \label) % 128 //pad
    BOOT_VECTOR_COMMON \esr, \elr, \far, \info
    .space 128
    BOOT_VECTOR_COMMON \esr, \elr, \far, \info
    .space 128
    BOOT_VECTOR_COMMON \esr, \elr, \far, \info
    .space 128
    .rept 12
        BOOT_VECTOR_COMMON \esr, \elr, \far, \info
        .space 128
    .endr
.endm

VECTOR_TABLE boot_vectors_el1, esr_el1, elr_el1, far_el1, boot_fault_info_el1
.align 11
VECTOR_TABLE boot_vectors_el2, esr_el2, elr_el2, far_el2, boot_fault_info_el2
.align 11
VECTOR_TABLE boot_vectors_el3, esr_el3, elr_el3, far_el3, boot_fault_info_el3