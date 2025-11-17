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

.section .vectors, "a", %progbits
.align 11
.global exception_vectors

exception_vectors:

.macro vector_slot handler
    b \handler
    .space 0x80 - 4
.endm

    vector_slot sync_el0_handler_as      // EL1t sync
    vector_slot irq_el1_asm_handler       // EL1t irq
    vector_slot fiq_el1_handler       // EL1t fiq
    vector_slot error_el1_handler     // EL1t serror

    vector_slot sync_el0_handler_as      // EL1h sync
    vector_slot irq_el1_asm_handler       // EL1h irq
    vector_slot fiq_el1_handler       // EL1h fiq
    vector_slot error_el1_handler     // EL1h serror

    vector_slot sync_el0_handler_as      // EL0_64 sync
    vector_slot irq_el1_asm_handler       // EL0_64 irq
    vector_slot fiq_el1_handler       // EL0_64 fiq
    vector_slot error_el1_handler     // EL0_64 serror

    vector_slot sync_el0_handler_as      // EL0_32 sync
    vector_slot irq_el1_asm_handler       // EL0_32 irq
    vector_slot fiq_el1_handler       // EL0_32 fiq
    vector_slot error_el1_handler     // EL0_32 serror

.global irq_el1_asm_handler
irq_el1_asm_handler:
    save_ctx 

    b irq_el1_handler
    eret

.global sync_el0_handler_as
//TODO: Rethink the registers used to be sequential both here and in context_switch and exception_vectors_as
sync_el0_handler_as:
    save_ctx

    b sync_el0_handler_c
    eret