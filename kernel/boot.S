.global _start
.org 0x80000
_start:
    ldr     x5, =stack_top
    mov     sp, x5

    // clear bss
    ldr     x5, =__bss_start
    ldr     w6, =__bss_size
1:  cbz     w6, 2f
    str     xzr, [x5], #8
    sub     w6, w6, #1
    cbnz    w6, 1b

    // jump to C code, should not return
2:  bl      kernel_main
    // for failsafe, halt this core
halt:
    wfe
    b halt