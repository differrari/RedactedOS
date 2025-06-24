.global _start
.org 0x80000
_start:
    //Halt other cores
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, 1f
    0:  wfe
    b 0b
    1:

    ldr     x5, =stack_top
    mov     sp, x5

    // clear bss
    ldr     x5, =__bss_start
    ldr     w6, =__bss_size
2:  cbz     w6, 3f
    str     xzr, [x5], #8
    sub     w6, w6, #1
    cbnz    w6, 2b

    // jump to C code, should not return
3:  bl      kernel_main
    // for failsafe, halt this core
halt:
    wfe
    b halt