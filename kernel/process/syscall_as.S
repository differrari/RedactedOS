.global sync_el0_handler_as

sync_el0_handler_as:
    cmp x8, #3
    b.ne .error
    ldr x0, =my_message
    mov x1, #0
    mov x2, #0
    bl uart_puts
    b .done
.error:
    adr x0, .
    bl uart_puthex
    mrs x0, elr_el1
    bl uart_puthex
    ldr x0, =error_message
    bl uart_puts
.done:
    mov x8, #0 //Clear the instruction
    eret

.section .rodata
my_message:
    .asciz "hi from kernel syscall handler\n"
error_message:
    .asciz " <- Wrong instruction\n"

