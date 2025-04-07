.global _start
_start:
    ldr x0, =stack_top
    mov sp, x0
    bl kernel_main            
    b .