.global sync_el0_handler_as

sync_el0_handler_as:
    msr daifset, #2//Disable interrupts
    mov x11, x0
    mov x12, x1
    mov x13, x2
    mov x14, x8
    mov x15, x29
    mov x16, x30
    mov x17, #1
    b sync_el0_handler_c
    eret