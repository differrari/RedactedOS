.global sync_el0_handler_as

sync_el0_handler_as:
    msr daifset, #2//Disable interrupts
    mov x15, x0
    mov x14, x1
    mov x9, x2
    mov x13, x29
    mov x12, x30
    mrs     x10, spsr_el1
    lsr     x19, x10, #2
    and     x19, x19, #0b11

    cmp     x19, #1
    b.eq    1f
    cmp     x19, #0
    b.eq    2f

    b       3f

1:  mov     x11, sp
    b       3f

2:  mrs     x11, sp_el0

3:
    b sync_el0_handler_c
    eret