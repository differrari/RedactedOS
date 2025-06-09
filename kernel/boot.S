.global _start
_start:
    ldr x1, =stack_top
    mov sp, x1
    mov x29, xzr
    mov x30, xzr
    mrs x0, CPACR_EL1
    orr x0, x0, #(3 << 20)
    msr CPACR_EL1, x0
    bl kernel_main
    b .