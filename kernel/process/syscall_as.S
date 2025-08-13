.global sync_el0_handler_as

//TODO: Rethink the registers used to be sequential both here and in context_switch and exception_vectors_as
sync_el0_handler_as:
    mrs     x15, spsr_el1
    lsr     x18, x15, #2
    and     x18, x18, #0b11

    cmp     x18, #1
    b.eq    1f
    cmp     x18, #0
    b.eq    2f

    b       3f

1:  mov     x18, sp
    b       3f

2:  mrs     x18, sp_el0

3:
    msr daifset, #2//Disable interrupts
    mov x8, x0
    mov x9, x1
    mov x10, x2
    mov x11, x3
    mov x16, x29
    mov x17, x30
    b sync_el0_handler_c
    eret