.global sync_el0_handler_as

//TODO: Rethink the registers used to be sequential both here and in context_switch and exception_vectors_as
sync_el0_handler_as:
    mrs     x10, spsr_el1
    lsr     x18, x10, #2
    and     x18, x18, #0b11

    cmp     x18, #1
    b.eq    1f
    cmp     x18, #0
    b.eq    2f

    b       3f

1:  mov     x11, sp
    b       3f

2:  mrs     x11, sp_el0

3:
    msr daifset, #2//Disable interrupts
    mov x15, x0
    mov x14, x1
    mov x9, x2
    mov x16, x3
    mov x13, x29
    mov x12, x30
    b sync_el0_handler_c
    eret