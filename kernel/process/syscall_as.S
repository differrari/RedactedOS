.global sync_el0_handler_as

sync_el0_handler_as:
    msr daifset, #2//Disable interrupts
    mov x11, x0
    mov x12, x1
    mov x13, x2
    mov x14, x8
    mov x15, x29
    mov x16, x30
    mrs     x18, spsr_el1
    lsr     x19, x18, #2
    and     x19, x19, #0b11

    cmp     x19, #1
    b.eq    1f
    cmp     x19, #0
    b.eq    2f

    b       3f

1:  mov     x17, sp
    b       3f

2:  mrs     x17, sp_el0

3:
    b sync_el0_handler_c
    eret