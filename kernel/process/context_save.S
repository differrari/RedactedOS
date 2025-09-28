.macro save_ctx
msr daifset, #2
    mrs     x10, spsr_el1
    lsr     x18, x10, #2
    and     x18, x18, #0b11

    cmp     x18, #1
    b.eq    1f
    cmp     x18, #0
    b.eq    2f

    b       3f

1:  mov     x16, sp
    b       3f

2:  mrs     x16, sp_el0
    ldr x18, =ksp
    //TODO: in order to ensure we don't ovewrite any stack data, save it to its own global variable
    mov sp, x18

3:
    mov x15, x0
    
ldr x0, =cpec
ldr x0, [x0]
// Save general-purpose registers x1-x30 (excluding x0 which holds the pointer)
stp x1, x2, [x0, #(8 * 0)]
stp x3, x4, [x0, #(8 * 2)]
stp x5, x6, [x0, #(8 * 4)]
stp x7, x8, [x0, #(8 * 6)]
stp x9, x10, [x0, #(8 * 8)]
stp x11, x12, [x0, #(8 * 10)]
stp x13, x14, [x0, #(8 * 12)]
stp x15, x16, [x0, #(8 * 14)]
stp x17, x18, [x0, #(8 * 16)]
stp x19, x20, [x0, #(8 * 18)]
stp x21, x22, [x0, #(8 * 20)]
stp x23, x24, [x0, #(8 * 22)]
stp x25, x26, [x0, #(8 * 24)]
stp x27, x28, [x0, #(8 * 26)]
stp x29, x30, [x0, #(8 * 28)]

// SP
str x16, [x0, #(8 * 31)]

//Status bits
str x10, [x0, #(8 * 33)]

.endm