.macro save_ctx
msr daifset, #2
    mrs     x18, spsr_el1
    lsr     x18, x18, #2
    and     x18, x18, #0b11

    cmp     x18, #1
    b.eq    1f
    cmp     x18, #0
    b.eq    2f

    b       3f

1:  mov     x17, sp
    b       3f

2:  mrs     x17, sp_el0
    ldr x18, =ksp
    //TODO: in order to ensure we don't ovewrite any stack data, save it to its own global variable
    mov sp, x18

3:
    
ldr x18, =cpec
ldr x18, [x18]
// Save general-purpose registers x1-x30
stp x0, x1, [x18, #(8 * 0)]
stp x2, x3, [x18, #(8 * 2)]
stp x4, x5, [x18, #(8 * 4)]
stp x6, x7, [x18, #(8 * 6)]
stp x8, x9, [x18, #(8 * 8)]
stp x10, x11, [x18, #(8 * 10)]
stp x12, x13, [x18, #(8 * 12)]
stp x14, x15, [x18, #(8 * 14)]
str x16,      [x18, #(8 * 16)]
str      x19, [x18, #(8 * 19)]
stp x20, x21, [x18, #(8 * 20)]
stp x22, x23, [x18, #(8 * 22)]
stp x24, x25, [x18, #(8 * 24)]
stp x26, x27, [x18, #(8 * 26)]
stp x28, x29, [x18, #(8 * 28)]
str x30,      [x18,#(8 * 30)] 

// SP
str x17, [x18, #(8 * 31)]

//Status bits
mrs x17, spsr_el1
str x17, [x18, #(8 * 33)]

.endm