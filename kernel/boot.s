.global _start
_start:
    ldr x0, =stack_top
    mov sp, x0
    mov x29, xzr
    mov x30, xzr
    bl kernel_main
    b .