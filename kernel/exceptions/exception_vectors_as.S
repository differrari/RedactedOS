#include "process/context.inc"

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
    msr daifset, #2
    mrs     x10, spsr_el1
    lsr     x18, x10, #2
    and     x18, x18, #0b11

    cmp     x18, #1
    b.eq    1f
    cmp     x18, #0
    b.eq    2f

    b       3f

1:  mov     x11, sp
    b       3f

2:  mrs     x11, sp_el0
    ldr x18, =ksp
    //TODO: in order to ensure we don't ovewrite any stack data, save it to its own global variable
    mov sp, x18

3:
    mov x15, x0
    mov x14, x1
    mov x9, x2
    mov x16, x3
    mov x13, x29
    mov x12, x30

    save_ctx 
    
    b irq_el1_handler
    eret