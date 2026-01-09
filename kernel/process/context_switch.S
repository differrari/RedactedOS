#include "sysregs.h"

.global save_pc_interrupt
save_pc_interrupt:
    mrs x1, elr_el1
    str x1, [x0, #(8 * 32)]

    ret

.global restore_context
restore_context:
    ldr x18, =cpec
    ldr x18, [x18]
    // Restore general-purpose registers
    ldp x0, x1, [x18, #(8 * 0)]
    ldp x2, x3, [x18, #(8 * 2)]
    ldp x4, x5, [x18, #(8 * 4)]
    ldp x6, x7, [x18, #(8 * 6)]
    ldp x8, x9, [x18, #(8 * 8)]
    ldp x10, x11, [x18, #(8 * 10)]
    ldp x12, x13, [x18, #(8 * 12)]
    ldp x14, x15, [x18, #(8 * 14)]
    ldr x16,      [x18, #(8 * 16)]
    ldr      x19, [x18, #(8 * 19)]
    ldp x20, x21, [x18, #(8 * 20)]
    ldp x22, x23, [x18, #(8 * 22)]
    ldp x24, x25, [x18, #(8 * 24)]
    ldp x26, x27, [x18, #(8 * 26)]
    ldp x28, x29, [x18, #(8 * 28)]
    ldr x30,      [x18, #(8 * 30)]    

    ldr x17, [x18, #(8 * 33)]
    msr spsr_el1, x17
    lsr     x17, x17, #2
    and     x17, x17, #0b11

    cmp     x17, #1
    b.eq    1f
    cmp     x17, #0
    b.eq    2f

    b       3f

1:  ldr x17, [x18, #(8 * 31)]
    mov     sp, x17
    b       3f

2:  ldr x17, [x18, #(8 * 31)]
    msr     sp_el0, x17

3:
    ldr x17, [x18, #(8 * 32)]
    msr elr_el1, x17

    mov x17, #0

    ldr x18, =pttbr
    
    cmp x18, #0
    b.eq 4f

    ldr x18, [x18]

    cmp x18, #0
    b.eq 4f

    msr ttbr0_el1, x18

    tlbi vmalle1is
4: dsb ish
    isb

    mov x18, #0

    eret
    
.global mmu_swap
mmu_swap:
    ldr x18, =pttbr
    
    cmp x18, #0
    b.eq 4f

    ldr x18, [x18]

    cmp x18, #0
    b.eq 4f

    msr ttbr0_el1, x18

    tlbi vmalle1is
4: dsb ish
    isb
    
    ret