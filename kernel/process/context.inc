.macro save_ctx
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
str x11, [x0, #(8 * 31)]

//Status bits
and x10, x10, 0xFFF
str x10, [x0, #(8 * 33)]

.endm